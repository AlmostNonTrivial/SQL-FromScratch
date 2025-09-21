/*
 * SQL From Scratch
 *
 * Database Pager
*/

#pragma once
#include <cstdint>
#include "common.hpp"

#define PAGE_INVALID 0
/*
 * Lowering this value, or the PAGE_SIZE to low can result in the tree being
 * corrupted. In a large tree a merge/split can propagate up, causing a node in use
 * to be evicted from the cache, making the pointer stale.
 *
 * To fix this, we could track which pages were used within a transaction, if everything
 * in the cache is, then we could just allocate additional cache nodes, then on commit shrink
 * down to a watermark level.
 */
#define MAX_CACHE_ENTRIES 128

/*
* The generic page layout used for all data pages. A page knowing it's own
* index allows us to append them in arbitrary positions in the journal,
* and rollback.
*
* This is the "type" that free_page and other page types can be cast from,
* since they all share the index field at offset 0.
*
* struct page_imp {
*   uint32_t index;
*   literally_anything_else data;
* };
*
* such that sizeof(page_imp) == PAGE_SIZE
*
* instance = (page_imp*)(page);
* instance->data;
*/
struct base_page
{
	uint32_t index; /* Page's position in the database file (self-identifying) */
	char	 data[PAGE_SIZE - sizeof(uint32_t)];
};

struct pager_meta
{
	uint32_t total_pages, cached_pages, dirty_pages, free_pages;
};

bool
pager_open(const char *filename);
base_page *
pager_get(uint32_t page_index);
uint32_t
pager_new();
bool
pager_ensure_journaled(uint32_t page_index);
bool
pager_delete(uint32_t page_index);
bool
pager_begin_transaction();
bool
pager_commit();
bool
pager_rollback();
uint32_t
pager_get_next();
pager_meta
pager_get_stats();
void
pager_close();


/*
PAGER ROLLBACK JOURNAL MECHANISM

NORMAL STATE (No Transaction)
------------------------------
	DATA FILE                          MEMORY
	┌─────────────┐                    ┌──────────────────┐
	│ Page 0:     │                    │ PAGER.root       │
	│ ROOT PAGE   │◄───────────────────│ (in-memory copy) │
	│ counter: 5  │                    │                  │
	│ free: 3─────┼──┐                 └──────────────────┘
	├─────────────┤  │                 ┌──────────────────┐
	│ Page 1:     │  │                 │ LRU CACHE        │
	│ DATA        │  │                 │ ┌──────────────┐ │
	├─────────────┤  │                 │ │ Page 1 data  │ │
	│ Page 2:     │  │                 │ ├──────────────┤ │
	│ DATA        │  │                 │ │ Page 4 data  │ │
	├─────────────┤  │                 │ └──────────────┘ │
	│ Page 3:     │◄─┘                 └──────────────────┘
	│ FREE        │
	│ prev: 0     │      (No Journal File Exists)
	├─────────────┤
	│ Page 4:     │
	│ DATA        │
	└─────────────┘



BEGIN TRANSACTION
-----------------
	DATA FILE                 JOURNAL FILE                MEMORY
	┌─────────────┐          ┌─────────────┐            ┌─────────────────┐
	│ Page 0:     │          │ Page 0:     │            │ journaled_or_   │
	│ ROOT PAGE   │────────► │ ROOT PAGE   │            │ new_pages:      │
	│ counter: 5  │  copy    │ (original)  │            │ {0}             │
	│ free: 3     │          └─────────────┘            └─────────────────┘
	├─────────────┤
	│ Page 1:     │          Journal created             in_transaction=true
	│ DATA        │          with root at
	├─────────────┤          offset 0
	│ Page 2:     │
	│ DATA        │
	├─────────────┤
	│ Page 3:     │
	│ FREE        │
	├─────────────┤
	│ Page 4:     │
	│ DATA        │
	└─────────────┘



MODIFYING PAGE 2 (First Modification)
--------------------------------------
Step 1: pager_ensure_journaled(2) - Journal original content BEFORE modification

	DATA FILE                 JOURNAL FILE                MEMORY
	┌─────────────┐          ┌─────────────┐            ┌─────────────────┐
	│ Page 0:     │          │ Page 0:     │            │ journaled_or_   │
	│ ROOT        │          │ ROOT PAGE   │            │ new_pages:      │
	├─────────────┤          ├─────────────┤            │ {0, 2}          │
	│ Page 1:     │          │ Page 2:     │◄─── append └─────────────────┘
	│ DATA        │  copy    │ DATA        │     original
	├─────────────┤  ───────►│ (original)  │     content
	│ Page 2:     │          └─────────────┘
	│ DATA        │
	│ (original)  │          Page 2 added to journal
	├─────────────┤          BEFORE any changes made
	│ Page 3:     │
	│ FREE        │
	├─────────────┤
	│ Page 4:     │
	│ DATA        │
	└─────────────┘

Step 2: Actual modification happens in cache

	CACHE (after modification)
	┌──────────────┐
	│ Page 2:      │
	│ MODIFIED     │ (dirty flag set)
	│ DATA         │
	└──────────────┘



ALLOCATING NEW PAGE
-------------------
	DATA FILE                 JOURNAL FILE                MEMORY
	┌─────────────┐          ┌─────────────┐            ┌─────────────────┐
	│ Page 0:     │          │ Page 0:     │            │ journaled_or_   │
	│ ROOT        │          │ ROOT PAGE   │            │ new_pages:      │
	│ counter: 6  │          ├─────────────┤            │ {0, 2, 5}       │
	├─────────────┤          │ Page 2:     │            └─────────────────┘
	│ ...         │          │ DATA        │
	├─────────────┤          └─────────────┘            Page 5 marked in set
	│ Page 5:     │                                      but NOT journaled
	│ NEW DATA    │          (No journal entry          (no original state
	└─────────────┘           for new pages)             to preserve)



COMMIT TRANSACTION
------------------
1. Write all dirty pages to data file
2. Write root page to data file
3. fsync(data_fd)
4. Delete journal file ← ATOMIC COMMIT POINT
5. Clear transaction state

	DATA FILE                                            MEMORY
	┌─────────────┐                                     ┌─────────────────┐
	│ Page 0:     │                                     │ journaled_or_   │
	│ ROOT PAGE   │                                     │ new_pages:      │
	│ (updated)   │                                     │ {} (cleared)    │
	├─────────────┤                                     └─────────────────┘
	│ Page 1:     │
	│ DATA        │          ✗ Journal deleted          in_transaction=false
	├─────────────┤            (commit complete)
	│ Page 2:     │
	│ MODIFIED    │
	├─────────────┤
	│ ...         │
	└─────────────┘



ROLLBACK/CRASH RECOVERY
-----------------------
If journal exists at startup or on explicit rollback:

	JOURNAL FILE                 DATA FILE (Being Restored)
	┌─────────────┐             ┌─────────────┐
	│ Page 0:     │──restore───►│ Page 0:     │
	│ ROOT PAGE   │             │ ROOT PAGE   │ ← Original metadata
	│ counter: 5  │             │ counter: 5  │   (including page count)
	├─────────────┤             ├─────────────┤
	│ Page 2:     │──restore───►│ Page 2:     │
	│ DATA        │             │ DATA        │ ← Original content
	│ (original)  │             │ (original)  │
	└─────────────┘             ├─────────────┤
								│ Page 5:     │ ← Will be truncated
								│ NEW DATA    │   based on original
								└─────────────┘   page_counter

Steps:
1. Read root from journal offset 0 → restore to data file
2. Read each page from journal → restore using page's self-identifying index
3. Truncate data file to (original page_counter * PAGE_SIZE)
4. Delete journal file
5. Reset cache


• Write-ahead logging: Original content journaled BEFORE modification
• Self-identifying pages: Each page stores its index, enabling simple
  append-only journal without separate index
• Atomic commit: Journal deletion is the commit point
• journaled_or_new_pages set: Ensures each page journaled at most once
  (capturing pre-transaction state) and new pages never journaled
• Root at offset 0: Fixed location simplifies recovery
• Crash safety: Journal presence at startup triggers automatic recovery




FREE PAGE MANAGEMENT SYSTEM


	PAGE STRUCTURE POLYMORPHISM
	────────────────────────────
	All pages share base layout:
	┌──────────────────────────┐
	│  base_page (PAGE_SIZE B) │
	├──────────────────────────┤
	│ index (4B) │   data...   │
	└──────────────────────────┘
		  ↓ reinterpret_cast based on usage

	┌────────────────┬────────────────┬────────────────┐
	│   root_page    │   free_page    │   data_page    │
	├────────────────┼────────────────┼────────────────┤
	│ page_counter   │ index          │ index          │
	│ free_page_head │ previous_free  │ [actual data]  │
	│ [padding...]   │ [padding...]   │                │
	└────────────────┴────────────────┴────────────────┘


	CREATING A FREE PAGE (pager_delete)
	════════════════════════════════════════════════════════════════════

	Initial State: Page 42 is active data page
	──────────────────────────────────────────
	Root Page (0)          Free List
	┌──────────────┐       ┌─────┐    ┌─────┐
	│ page_counter │       │  7  │───→│  3  │───→ 0 (end)
	│ free_head: 7 │       └─────┘    └─────┘
	└──────────────┘

	Page 42
	┌──────────────┐
	│ index: 42    │
	│ [user data]  │
	└──────────────┘

	Step 1: Load page 42 into cache & mark dirty
	─────────────────────────────────────────────
	cache_get_or_load(42) → Ensures page is in cache
	pager_ensure_journaled(42)  → Will be journaled

	Step 2: Reinterpret as free_page
	─────────────────────────────────
	Page 42 (reinterpreted)
	┌───────────────────┐
	│ index: 42         │  ← Unchanged
	│ previous_free: ?? │  ← To be set
	│ [padding...]      │
	└───────────────────┘

	Step 3: Insert at head of free list
	────────────────────────────────────
	free_page->previous_free = root.free_page_head (7)
	root.free_page_head = 42

	Final State: Page 42 is now free
	──────────────────────────────────
	Root Page (0)          Free List
	┌──────────────┐       ┌─────┐    ┌─────┐    ┌─────┐
	│ page_counter │       │ 42  │───→│  7  │───→│  3  │───→ 0
	│ free_head: 42│       └─────┘    └─────┘    └─────┘
	└──────────────┘       (newest)              (oldest)



	RECLAIMING A FREE PAGE (pager_new)

	Initial State: Need new page, free list available
	──────────────────────────────────────────────────
	Root Page (0)          Free List
	┌──────────────┐       ┌─────┐    ┌─────┐    ┌─────┐
	│ counter: 100 │       │ 42  │───→│  7  │───→│  3  │───→ 0
	│ free_head: 42│       └─────┘    └─────┘    └─────┘
	└──────────────┘

	Step 1: Check free list (take_page_from_free_list)
	────────────────────────────────────────────────────
	if (root.free_page_head != 0)  // We have free pages!
		current_index = 42

	Step 2: Load free page & extract next pointer
	───────────────────────────────────────────────
	Free Page 42
	┌───────────────────┐
	│ index: 42         │
	│ previous_free: 7  │ ← Save this
	│ [padding...]      │
	└───────────────────┘

	Step 3: Update free list head
	──────────────────────────────
	root.free_page_head = free_page->previous_free (7)

	Step 4: Mark as new & initialize
	─────────────────────────────────
	PAGER.journaled_or_new_pages.insert(42)  // Won't journal old data
	memset(page, 0, PAGE_SIZE)                // Clear old free_page data
	page->index = 42                          // Restore index

	Final State: Page 42 reclaimed for use
	────────────────────────────────────────
	Root Page (0)          Free List (shorter)
	┌──────────────┐       ┌─────┐    ┌─────┐
	│ counter: 100 │       │  7  │───→│  3  │───→ 0
	│ free_head: 7 │       └─────┘    └─────┘
	└──────────────┘

	Page 42 (ready for data)
	┌──────────────┐
	│ index: 42    │
	│ [zeros...]   │ ← Clean slate
	└──────────────┘





LRU CACHE SYSTEM

	base_page* page = pager_get(42);

	pager_ensure_journaled(42); // Tell pager it needs to change
	page->data[0] = 'X';  // Direct modification of cached page


	INTERNAL ARCHITECTURE
	════════════════════════════════════════════════════════════════════

	Global Pager State:
	┌─────────────────────────────────────────────────────────────────┐
	│ cache_meta[MAX_CACHE_ENTRIES]  │ cache_data[MAX_CACHE_ENTRIES]  │
	├────────────────────────────────┼────────────────────────────────┤
	│ [0] page_idx=7,  dirty, occ... │ [0] Page 7 data (4KB)          │
	│ [1] page_idx=42, clean, occ... │ [1] Page 42 data (4KB)         │
	│ [2] page_idx=15, dirty, occ... │ [2] Page 15 data (4KB)         │
	│ [3] empty                      │ [3] uninitialized              │
	│ ...                            │ ...                            │
	└────────────────────────────────┴────────────────────────────────┘
	Separated for cache locality when scanning metadata


	┌──────────────────────────────┐     ┌─────────────────────────┐
	│   page_to_cache (hash_map)   │     │    LRU Doubly-Linked    │
	├──────────────────────────────┤     │         List            │
	│ Page 7  → Slot 0             │     ├─────────────────────────┤
	│ Page 42 → Slot 1             │     │ head → 1 ↔ 2 ↔ 0 ← tail │
	│ Page 15 → Slot 2             │     │  (MRU)           (LRU)  │
	└──────────────────────────────┘     └─────────────────────────┘
	  O(1) lookup: "Is page           O(1) operations for LRU policy
	  cached? Where?"

 */
