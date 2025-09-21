/*
 * SQL From Scratch
 *
 * Database Pager
 *
 * The pager provides an abstraction layer between the higher-level SQL engine
 * and the filesystem. It manages fixed-size pages, implements an LRU
 * cache, and provides ACID transactions through rollback-journaling.
 *
 * Pages: The database file is divided into fixed-size pages. Page 0 is
 * reserved as the "root page" containing metadata. All other pages can be
 * used for data or placed on a free list for reuse.
 *
 * Cache: A LRU cache keeps frequently accessed pages in memory.
 * When the cache is full, the least recently used page is evicted.
 * Dirty pages are written to disk on eviction.
 *
 * Free List: Deleted pages are linked into a singly-linked free list, with
 * the head pointer stored in the root page. New allocations preferentially
 * reuse free pages before growing the file. The caller is responsible for
 * not using deleted pages.
 *
 * Transactions: The pager implements transactions using a rollback-journal.
 * Before modifying a page, its original content is saved to a journal file.
 * On commit, changes are written to the main file, then the journal is deleted.
 * On rollback or crash recovery, the journal is replayed to restore the
 * original state.
 *
 * Journal Format:
 *   - Offset 0: Original root page (saved at transaction begin)
 *   - Offset PAGE_SIZE+: Original content of modified data pages
 *   Each page stores its own index for recovery purposes.
 *
 * Page Allocation:
 *   1. Check free list for available pages
 *   2. If empty, increment page counter to grow file
 *   3. New pages are zero-initialized
 *
 * Crash Recovery:
 *   On startup, if a journal exists, the database was interrupted
 * mid-transaction. Recovery replays the journal to restore all pages to their
 * pre-transaction state, then deletes the journal.
 *
 * The pager allows callers to view the database file as a practically infinite
 * array of fixed sized nodes. The Caching layer is hidden from the caller, such
 * that when they call 'pager_get(x)' they might as well be doing an index into
 * 'char file[LARGE_NUMBER][PAGE_SIZE]'
 *
 * Keeping the pager agnositic to how pages are implemented means that we can
 * use it with different storage backends, for example we have internal and leaf
 * b+tree nodes, and BLOB nodes, an internally free-pages.
 *
 * The drawback of this is that because the pager doesn't know the contents of
 * the pages, it can't implement a compaction mechanism in the case that free
 * pages build up, since the b+tree and other structures maintain relationships
 * via integer indexes pointing to slots in the file, shifting the pages without
 * updating these keys would break everything.
 *
 * Therefore a compaction mechanism would have to be implemented by the actual
 * callers of the pager, as they know how to interpret their own pages.
 *
 */
#include "pager.hpp"
#include "arena.hpp"
#include "os_layer.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/*
 * The pager arena is initialized on startup, and reset in the event of a
 * rollback
 */
struct pager_arena {};

#define INVALID_SLOT -1
#define FILENAME_SIZE 32
#define JOURNAL_POSTFIX "%s-journal"
#define JOURNAL_FILENAME_SIZE FILENAME_SIZE + 12
#define ROOT_PAGE_INDEX 0U

/*
 * When a page is deleted, it joins a linked list for future reuse,
 * storing the list node data in the page itself.
 */
struct free_page {
  uint32_t index;              /* Self-reference for journal recovery */
  uint32_t previous_free_page; /* Forms singly-linked list (0 = end) */
  uint8_t padding[PAGE_SIZE - sizeof(uint32_t) * 2];
};

/*
 * The pager reserves the first page offset (0) in the data file and journal
 * file for the root, which contains metadata. Keeping metadata in the data file
 * is easier for atomicity, as only data file (the whole database) needs to be
 * valid, as opposed to data+meta files.
 */
struct root_page {
  uint32_t page_counter;   /* Next page index to allocate */
  uint32_t free_page_head; /* Head of free list (0 = empty list) */
  char padding[PAGE_SIZE - (sizeof(uint32_t) * 2)];
};

/*
 * Invariants to ensure our reinterpret_casts are safe
 * and that disk I/O aligns with OS page boundaries
 */
static_assert(PAGE_SIZE == sizeof(base_page), "Page size mismatch");
static_assert(PAGE_SIZE == sizeof(root_page), "root_page size mismatch");
static_assert(PAGE_SIZE == sizeof(free_page), "free_page size mismatch");

/*
 * Each cache slot has associated metadata for the LRU eviction policy.
 * Separating metadata from data improves cache locality when scanning
 * the LRU list.
 *
 * The doubly-linked list allows O(1) removal from arbitrary positions,
 * essential for the LRU policy when a page hit occurs.
 */
struct cache_metadata {
  uint32_t page_index; /* Which page is cached in this slot */
  bool is_dirty;       /* Needs write-back on eviction? */
  bool is_occupied;    /* Is this slot currently in use? */
  int32_t lru_next;    /* Next slot in LRU order (-1 = end) */
  int32_t lru_prev;    /* Previous slot in LRU order (-1 = end) */
};

/*
 * Single global instance simplifies the API
 */
static struct {
  os_file_handle_t data_fd;
  os_file_handle_t journal_fd;

  /* In-memory root page, accessed separate from the cache */
  root_page root;

  /* Page cache with parallel arrays pattern for better memory layout */
  cache_metadata cache_meta[MAX_CACHE_ENTRIES]; /* LRU and state tracking */
  base_page cache_data[MAX_CACHE_ENTRIES];      /* Actual page data */

  /* LRU list endpoints for O(1) access to head (MRU) and tail (LRU) */
  int32_t lru_head; /* Most recently used slot */
  int32_t lru_tail; /* Least recently used slot (eviction candidate) */

  /* Transaction state */
  bool in_transaction;
  char data_file[FILENAME_SIZE];
  char journal_file[JOURNAL_FILENAME_SIZE];

  /*
   * page_to_cache: Lookup of "is page X cached, and where?"
   * journaled_or_new_pages: Track pages that don't need journaling
   *                         (already saved or newly created)
   */
  hash_map<uint32_t, uint32_t, pager_arena> page_to_cache;
  hash_set<uint32_t, pager_arena> journaled_or_new_pages;

} PAGER = {};

static void write_page_to_disk(uint32_t page_index, const void *data) {
  os_file_seek(PAGER.data_fd, page_index * PAGE_SIZE);
  os_file_write(PAGER.data_fd, data, PAGE_SIZE);
}

static bool read_page_from_disk(uint32_t page_index, void *data) {
  os_file_seek(PAGER.data_fd, page_index * PAGE_SIZE);
  return os_file_read(PAGER.data_fd, data, PAGE_SIZE) == PAGE_SIZE;
}

/*
 * Write a page to the journal file.
 *
 *   1. Mark page as journaled in the journaled_or_new_pages set
 *   2. If root page, write at offset 0
 *   3. Otherwise, append to end of journal
 *   4. fsync journal to ensure durability
 */
static void journal_write_page(uint32_t page_index, const void *data) {
  PAGER.journaled_or_new_pages.insert(page_index, 1);

  if (page_index == ROOT_PAGE_INDEX) {
    os_file_seek(PAGER.journal_fd, 0);
  } else {
    int64_t journal_size = os_file_size(PAGER.journal_fd);
    if (journal_size < PAGE_SIZE) {
      journal_size = PAGE_SIZE;
    }
    os_file_seek(PAGER.journal_fd, journal_size);
  }

  os_file_write(PAGER.journal_fd, data, PAGE_SIZE);
  os_file_sync(PAGER.journal_fd);
}

static void lru_remove_from_list(int32_t slot) {
  cache_metadata *entry = &PAGER.cache_meta[slot];

  if (entry->lru_prev != INVALID_SLOT) {
    PAGER.cache_meta[entry->lru_prev].lru_next = entry->lru_next;
  } else {
    PAGER.lru_head = entry->lru_next;
  }

  if (entry->lru_next != INVALID_SLOT) {
    PAGER.cache_meta[entry->lru_next].lru_prev = entry->lru_prev;
  } else {
    PAGER.lru_tail = entry->lru_prev;
  }

  entry->lru_next = INVALID_SLOT;
  entry->lru_prev = INVALID_SLOT;
}

static void lru_add_to_head(int32_t slot) {
  cache_metadata *entry = &PAGER.cache_meta[slot];

  entry->lru_next = PAGER.lru_head;
  entry->lru_prev = INVALID_SLOT;

  if (PAGER.lru_head != INVALID_SLOT) {
    PAGER.cache_meta[PAGER.lru_head].lru_prev = slot;
  }

  PAGER.lru_head = slot;

  if (PAGER.lru_tail == INVALID_SLOT) {
    PAGER.lru_tail = slot;
  }
}

static void cache_move_to_head(int32_t slot) {
  if (PAGER.lru_head == slot) {
    return;
  }

  lru_remove_from_list(slot);
  lru_add_to_head(slot);
}

/*
 * Evict the least recently used page from the cache.
 *
 *   1. Select the tail of the LRU list
 *   2. If page is dirty, write to disk
 *   3. Remove from page_to_cache map
 *   4. Remove from LRU list
 *   5. Mark slot as invalid
 *   6. Return the slot for reuse
 */
static int32_t cache_evict_lru_entry() {
  int32_t slot = PAGER.lru_tail;
  cache_metadata *entry = &PAGER.cache_meta[slot];

  if (entry->is_dirty) {
    write_page_to_disk(entry->page_index, &PAGER.cache_data[slot]);
  }

  PAGER.page_to_cache.remove(entry->page_index);

  lru_remove_from_list(slot);

  entry->is_occupied = false;
  entry->is_dirty = false;
  entry->page_index = ROOT_PAGE_INDEX;

  return slot;
}

static uint32_t cache_find_free_slot() {
  for (uint32_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (!PAGER.cache_meta[i].is_occupied) {
      return i;
    }
  }

  return cache_evict_lru_entry();
}

static void cache_reset() {
  for (uint32_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    PAGER.cache_meta[i].page_index = ROOT_PAGE_INDEX;
    PAGER.cache_meta[i].is_dirty = false;
    PAGER.cache_meta[i].is_occupied = false;
    PAGER.cache_meta[i].lru_next = INVALID_SLOT;
    PAGER.cache_meta[i].lru_prev = INVALID_SLOT;
  }

  PAGER.page_to_cache.clear();
  PAGER.journaled_or_new_pages.clear();

  PAGER.lru_head = INVALID_SLOT;
  PAGER.lru_tail = INVALID_SLOT;
}

/*
 * Fetch a page into cache.
 *
 *   1. Check if page is already cached via page_to_cache map
 *   2. If cached, move to head of LRU and return
 *   3. Otherwise find a free cache slot (may evict)
 *   4. Read page from disk into slot
 *   5. Update cache metadata
 *   6. Insert into page_to_cache map
 *   7. Add to head of LRU list
 */
static base_page *cache_get_or_load(uint32_t page_index) {
  uint32_t *slot_ptr = PAGER.page_to_cache.get(page_index);
  if (slot_ptr) {
    uint32_t slot = *slot_ptr;
    cache_move_to_head(slot);
    return &PAGER.cache_data[slot];
  }

  uint32_t slot = cache_find_free_slot();
  cache_metadata *entry = &PAGER.cache_meta[slot];

  read_page_from_disk(page_index, &PAGER.cache_data[slot]);

  entry->page_index = page_index;
  entry->is_occupied = true;
  entry->is_dirty = false;

  PAGER.page_to_cache.insert(page_index, slot);
  lru_add_to_head(slot);

  return &PAGER.cache_data[slot];
}

/*
 * Add a page to the free list.
 *
 *   1. Ensure page is loaded
 *   2. Ensure it's journaled
 *   3. Reinterpret the page as a free_page, the index will be the same
 *   4. Set previous_free_page to the current free list head
 *   5. Update root to point to new head
 */
static void add_page_to_free_list(uint32_t page_index) {
  free_page *free_page_ptr =
      reinterpret_cast<free_page *>(cache_get_or_load(page_index));

  pager_ensure_journaled(page_index);

  uint32_t current_free_page = PAGER.root.free_page_head;
  assert(free_page_ptr->index == page_index &&
         "Conversion to free_page should not affect index at bytes 0-4");
  free_page_ptr->previous_free_page = current_free_page;

  PAGER.root.free_page_head = page_index;
}

/*
 * Take a page from the free list.
 *
 *   1. Check if free list is empty (head == 0), return 0 if so
 *   2. Load the current head of free list
 *   3. Ensure it's journaled
 *   4. Update root to point to the current's previous_free_page
 *   5. Return the reclaimed page index
 */
static uint32_t take_page_from_free_list() {
  if (PAGER.root.free_page_head == 0) {
    return ROOT_PAGE_INDEX;
  }

  uint32_t current_index = PAGER.root.free_page_head;
  free_page *current_free_page =
      reinterpret_cast<free_page *>(cache_get_or_load(current_index));
  pager_ensure_journaled(current_index);

  PAGER.root.free_page_head = current_free_page->previous_free_page;

  return current_index;
}

/*
 * Count free pages by walking the linked list.
 *
 *   1. Start from root.free_page_head
 *   2. Load each free page
 *   3. Follow previous_free_page link
 *   4. Stop when reaching ROOT_PAGE_INDEX (sentinel)
 */
static uint32_t count_free_pages() {
  uint32_t count = 0;
  uint32_t current_free_page_index = PAGER.root.free_page_head;
  while (ROOT_PAGE_INDEX != current_free_page_index) {
    free_page *free_page_ptr = reinterpret_cast<free_page *>(
        cache_get_or_load(current_free_page_index));

    current_free_page_index = free_page_ptr->previous_free_page;

    count++;
  }

  return count;
}

/*
 * Open a database file.
 *
 *   1. Initialize arena allocator
 *   2. Open data file (create if needed)
 *   3. Check for journal file (crash recovery)
 *   4. If journal exists, rollback incomplete transaction
 *   5. If existing database, load root page
 *   6. If new database, initialize root page
 */
bool pager_open(const char *filename) {
  if (strlen(filename) >= FILENAME_SIZE) {
    return false;
  }

  arena<pager_arena>::init();

  strcpy(PAGER.data_file, filename);

  snprintf(PAGER.journal_file, sizeof(PAGER.journal_file), JOURNAL_POSTFIX,
           filename);

  bool exists = os_file_exists(filename);
  PAGER.data_fd = os_file_open(filename, true, true);
  if (OS_INVALID_HANDLE == PAGER.data_fd) {
    return false;
  }

  bool journal_exists = os_file_exists(PAGER.journal_file);
  if (journal_exists) {
    PAGER.in_transaction = true;
    PAGER.journal_fd = os_file_open(PAGER.journal_file, true, false);
    if (OS_INVALID_HANDLE == PAGER.journal_fd) {
      pager_close();
      return false;
    }
    pager_rollback();
  } else {
    cache_reset();
  }

  if (exists) {
    read_page_from_disk(ROOT_PAGE_INDEX, &PAGER.root);
  } else {
    PAGER.root.page_counter = 1;
    PAGER.root.free_page_head = ROOT_PAGE_INDEX;
    write_page_to_disk(ROOT_PAGE_INDEX, &PAGER.root);
  }

  return true;
}

/*
 * Get a page for reading/writing.
 *
 *   1. Validate page index is in valid range
 *   2. Check page is not root (internal only)
 *   3. Load page into cache and return pointer to cache memory
 */
base_page *pager_get(uint32_t page_index) {
  assert(page_index < PAGER.root.page_counter &&
         page_index != ROOT_PAGE_INDEX && "Page requested is invalid");

  return cache_get_or_load(page_index);
}

/*
 * Allocate a new page.
 *
 *   1. Verify transaction is active
 *   2. Try to reclaim a page from free list
 *   3. If no free pages, allocate new page index
 *   4. Mark as new, so as not to be added to the journal
 *   5. Find cache slot and initialize page data
 *   6. Ensure it's in the journaled_or_new set
 */
uint32_t pager_new() {
  assert(PAGER.in_transaction && "Must be in a transaction to allocate a page");

  uint32_t page_index = take_page_from_free_list();

  if (page_index == ROOT_PAGE_INDEX) {
    page_index = PAGER.root.page_counter++;
  }

  PAGER.journaled_or_new_pages.insert(page_index, 1);

  uint32_t slot = cache_find_free_slot();
  cache_metadata *entry = &PAGER.cache_meta[slot];

  memset(&PAGER.cache_data[slot], 0, PAGE_SIZE);
  PAGER.cache_data[slot].index = page_index;

  entry->page_index = page_index;
  entry->is_occupied = true;
  entry->is_dirty = true;

  PAGER.page_to_cache.insert(page_index, slot);
  lru_add_to_head(slot);

  return page_index;
}

/*
 * Ensure a page is either journaled and/or in the journaled_or_new_pages set
 *
 * NOTE:
 * This must be called BEFORE modifying the page data, so the pre-modified data
 * is journaled.
 *
 *   1. Validate page index and transaction state
 *   2. If page not yet journaled, write to journal
 *   3. Mark in journaled_or_new_pages set
 *   4. If cached, set dirty flag
 */
bool pager_ensure_journaled(uint32_t page_index) {
  assert(page_index < PAGER.root.page_counter &&
         page_index != ROOT_PAGE_INDEX &&
         "Requesting journaling of invalid page");

  assert(PAGER.in_transaction && "Must be in a transaction to journal a page");

  if (!PAGER.journaled_or_new_pages.contains(page_index)) {
    journal_write_page(page_index, cache_get_or_load(page_index));
  }

  uint32_t *slot_ptr = PAGER.page_to_cache.get(page_index);
  if (slot_ptr) {
    PAGER.cache_meta[*slot_ptr].is_dirty = true;
  }

  return true;
}

/*
 * Delete a page.
 *
 *   1. Validate page can be deleted (not root, valid index)
 *   2. Verify transaction is active
 *   3. Add page to free list
 */
bool pager_delete(uint32_t page_index) {
  assert(page_index < PAGER.root.page_counter &&
         page_index != ROOT_PAGE_INDEX && "Requesting delete of invalid page");
  assert(PAGER.in_transaction && "Must be in a transaction to delete a page");

  add_page_to_free_list(page_index);

  return true;
}

/*
 * Begin a transaction.
 *
 *   1. Check not already in transaction
 *   2. Create journal file
 *   3. Write root page to journal
 *   4. Set transaction flag
 */
bool pager_begin_transaction() {
  if (PAGER.in_transaction) {
    return true;
  }

  PAGER.journal_fd = os_file_open(PAGER.journal_file, true, true);

  if (OS_INVALID_HANDLE == PAGER.journal_fd) {
    return false;
  }

  PAGER.in_transaction = true;

  journal_write_page(ROOT_PAGE_INDEX, &PAGER.root);

  return true;
}

/*
 * Commit a transaction.
 *
 *   1. Write all dirty cached pages to disk
 *   2. Write root page with updated metadata
 *   3. Sync data file
 *   4. Delete journal (atomic commit point)
 *   5. Clear transaction state
 */
bool pager_commit() {
  if (!PAGER.in_transaction) {
    return true;
  }

  for (int32_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (PAGER.cache_meta[i].is_occupied && PAGER.cache_meta[i].is_dirty) {
      write_page_to_disk(PAGER.cache_meta[i].page_index, &PAGER.cache_data[i]);
      PAGER.cache_meta[i].is_dirty = false;
    }
  }

  write_page_to_disk(ROOT_PAGE_INDEX, &PAGER.root);
  os_file_sync(PAGER.data_fd);

  os_file_close(PAGER.journal_fd);
  os_file_delete(PAGER.journal_file);

  PAGER.journal_fd = OS_INVALID_HANDLE;
  PAGER.in_transaction = false;

  PAGER.journaled_or_new_pages.clear();

  return true;
}

/*
 * Rollback a transaction.
 *
 * NOTE:
 * The root page always goes at offset 0 in the journal, other pages can simply
 * be read sequentially as they contain their own index in the data file
 *
 *   1. Read root page from journal
 *   2. Restore root to disk
 *   3. Read each journaled page and restore to original location
 *   4. Truncate file to remove any newly allocated pages
 *   5. Delete journal
 *   6. Reset cache
 */
bool pager_rollback() {
  if (!PAGER.in_transaction) {
    return true;
  }

  int64_t journal_size = os_file_size(PAGER.journal_fd);

  if (journal_size >= PAGE_SIZE) {
    os_file_seek(PAGER.journal_fd, 0);
    if (os_file_read(PAGER.journal_fd, &PAGER.root, PAGE_SIZE) == PAGE_SIZE) {
      write_page_to_disk(ROOT_PAGE_INDEX, &PAGER.root);
    }

    for (int64_t offset = PAGE_SIZE; offset < journal_size;
         offset += PAGE_SIZE) {
      uint8_t page_buffer[PAGE_SIZE];
      os_file_seek(PAGER.journal_fd, offset);
      if (os_file_read(PAGER.journal_fd, page_buffer, PAGE_SIZE) != PAGE_SIZE) {
        break;
      }

      base_page *page_ptr = reinterpret_cast<base_page *>(page_buffer);
      uint32_t page_index = page_ptr->index;
      write_page_to_disk(page_index, page_buffer);
    }

    os_file_truncate(PAGER.data_fd, PAGER.root.page_counter * PAGE_SIZE);
  }

  os_file_close(PAGER.journal_fd);
  os_file_delete(PAGER.journal_file);
  PAGER.journal_fd = OS_INVALID_HANDLE;

  arena<pager_arena>::reset_and_decommit();
  cache_reset();

  PAGER.in_transaction = false;

  return true;
}

/*
 * Returns the next page that will be allocated
 */
uint32_t pager_get_next() {
  uint32_t free_page = PAGER.root.free_page_head;
  return free_page != ROOT_PAGE_INDEX ? free_page : PAGER.root.page_counter;
}

void pager_close() {
  os_file_close(PAGER.data_fd);

  PAGER.in_transaction = false;
  PAGER.data_fd = OS_INVALID_HANDLE;

  arena<pager_arena>::shutdown();
}

pager_meta pager_get_stats() {
  pager_meta stats;

  stats.total_pages = PAGER.root.page_counter - 1;
  stats.free_pages = count_free_pages();

  stats.cached_pages = 0;
  stats.dirty_pages = 0;

  for (int32_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (PAGER.cache_meta[i].is_occupied) {
      stats.cached_pages++;
      if (PAGER.cache_meta[i].is_dirty) {
        stats.dirty_pages++;
      }
    }
  }

  return stats;
}
