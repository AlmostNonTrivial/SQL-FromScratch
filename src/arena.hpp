/*
 * SQL From Scratch
 *
 * Arena Allocator
 *
 * This is my own allocator lifted from other hobby projects, not SQL specific.
 * It's a glorified bump allocator, with some reclamation ability.
 *
 * Depending on the size of what reclaim is called on, it will go into a bucket, and when doing an alloc
 * requesting a certain size, look in corresponding buckets.
 *
 * Usage pattern:
 * 1. Arena<MyTag>::init() reserves virtual address space
 * 2. Allocations commit pages as needed
 * 3. Containers can reclaim() memory when growing
 * 4. reset() nukes everything but keeps pages committed
 * 4. reset_and_decommit() nukes everything and give's back pages
 */

#pragma once
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include "common.hpp"
#include <cstdint>
#include <string_view>
#include <typeinfo>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* Cross-platform virtual memory operations for the custom allocators*/
struct virtual_memory
{
	static void *
	reserve(size_t size)
	{
#ifdef _WIN32
		return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
#else
		void *ptr = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		return (ptr == MAP_FAILED) ? nullptr : ptr;
#endif
	}

	static bool
	commit(void *addr, size_t size)
	{
#ifdef _WIN32
		return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
		return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
#endif
	}

	static void
	decommit(void *addr, size_t size)
	{
#ifdef _WIN32
		VirtualFree(addr, size, MEM_DECOMMIT);
#else
		madvise(addr, size, MADV_DONTNEED);
		mprotect(addr, size, PROT_NONE);
#endif
	}

	static void
	release(void *addr, size_t size)
	{
#ifdef _WIN32
		(void)size;
		VirtualFree(addr, 0, MEM_RELEASE);
#else
		munmap(addr, size);
#endif
	}

	static size_t
	page_size()
	{
		static size_t cached_size = 0;
		if (cached_size == 0)
		{
#ifdef _WIN32
			SYSTEM_INFO si;
			GetSystemInfo(&si);
			cached_size = si.dwPageSize;
#else
			cached_size = sysconf(_SC_PAGESIZE);
#endif
		}
		return cached_size;
	}

	static size_t
	round_to_pages(size_t size)
	{
		size_t page_sz = page_size();
		return ((size + page_sz - 1) / page_sz) * page_sz;
	}
};

struct global_arena
{
};

template <typename Tag = global_arena, bool zero_on_reset = true, size_t Align = 8> struct arena
{
	static_assert((Align & (Align - 1)) == 0, "Alignment must be power of 2");
	static_assert(Align >= sizeof(void *), "Alignment must be at least pointer size");

	static inline uint8_t *base = nullptr;
	static inline uint8_t *current = nullptr;
	static inline size_t   reserved_capacity = 0;
	static inline size_t   committed_capacity = 0;
	static inline size_t   max_capacity = 0;
	static inline size_t   initial_commit = 0;

	struct free_block
	{
		free_block *next;
		size_t		size;
	};

	/*
	 * Freelist buckets organized by power-of-2 size classes.
	 * freelists[4] = blocks of size [16, 32]
	 * freelists[5] = blocks of size [32, 64]
	 * etc.
	 */
	static inline free_block *freelists[32] = {};
	static inline uint32_t	  occupied_buckets = 0; // Bitmask: which buckets have blocks

	static bool
	init(size_t initial = PAGE_SIZE, size_t maximum = 0)
	{
		if (base)
		{
			return true;
		}

		initial_commit = virtual_memory::round_to_pages(initial);
		max_capacity = maximum;

		/*
		 * Reserve a huge virtual address range upfront.
		 * This costs nothing on 64-bit systems
		 * We'll commit physical pages lazily as needed.
		 *
		 * This means that each arena can have it's own address space giving it a
		 * contiguous view of memory
		 */
		reserved_capacity = max_capacity ? max_capacity : (1ULL << 33); // 8GB

		base = (uint8_t *)virtual_memory::reserve(reserved_capacity);
		if (!base)
		{
			fprintf(stderr, "Failed to reserve virtual memory\n");
			return false;
		}

		current = base;
		committed_capacity = 0;

		if (initial_commit > 0)
		{
			if (!virtual_memory::commit(base, initial_commit))
			{
				fprintf(stderr, "Failed to commit initial memory: %zu bytes\n", initial_commit);
				virtual_memory::release(base, reserved_capacity);
				base = nullptr;
				return false;
			}
			committed_capacity = initial_commit;
		}

		for (int i = 0; i < 32; i++)
		{
			freelists[i] = nullptr;
		}
		occupied_buckets = 0;
		return true;
	}

	static void
	shutdown()
	{
		if (!base)
		{
			return;
		}
		virtual_memory::release(base, reserved_capacity);
		base = nullptr;
		current = nullptr;
		reserved_capacity = 0;
		committed_capacity = 0;
		max_capacity = 0;

		for (int i = 0; i < 32; i++)
		{
			freelists[i] = nullptr;
		}
		occupied_buckets = 0;
	}

	/*
	 * Maps allocation size to freelist bucket index.
	 *
	 * The index i such that 2^i <= size < 2^(i+1) is needed
	 * This is just finding the position of the highest set bit.
	 *
	 * The XOR dance at the end clamps the result to [0, 31] branchlessly.
	 */
	static inline int
	get_size_class(size_t size)
	{
		size = size | 0x2; // needs to be at least 2 to work

#ifdef _MSC_VER
		unsigned long index;
		_BitScanReverse64(&index, size - 1);
		return (int)index ^ ((index ^ 31) & -((int)index > 31));
#else
		int cls = 63 - __builtin_clzll(size - 1);
		return cls ^ ((cls ^ 31) & -(cls > 31));
#endif
	}

	/*
	 * Called by containers when they grow and abandon their old buffer.
	 * Add it to the appropriate freelist for future reuse.
	 */
	static void
	reclaim(void *ptr, size_t size)
	{
		if (!ptr || !base || size < sizeof(free_block))
		{
			return;
		}

		uint8_t *addr = (uint8_t *)ptr;

		if (addr < base || addr >= base + reserved_capacity || addr >= current)
		{
			return;
		}

		int size_class = get_size_class(size);

		free_block *block = (free_block *)ptr;
		block->size = size;
		block->next = freelists[size_class];
		freelists[size_class] = block;

		occupied_buckets |= (1u << size_class);
	}

	/*
	 * Check freelists for a suitable reclaimed block but
	 * fall back to bump allocation from current pointer
	 *
	 * Use the occupied_buckets bitmask to quickly find the smallest
	 * bucket that can satisfy our request.
	 */
	static void *
	try_alloc_from_freelist(size_t size)
	{
		int size_class = get_size_class(size);

		/*
		 * If size is exactly 2^n, it fits in bucket n.
		 * If size is 2^n + 1, bucket n+1 is need.
		 */
		if (size > (1u << size_class))
		{
			size_class++;
		}

		/*
		 * Create a mask of all buckets >= size_class.
		 * Then AND with occupied_buckets to find available buckets.
		 */
		uint32_t mask = ~((1u << size_class) - 1);
		uint32_t candidates = occupied_buckets & mask;

		if (!candidates)
		{
			return nullptr;
		}

		/* Find the lowest set bit = smallest suitable bucket */
#ifdef _MSC_VER
		unsigned long cls;
		_BitScanForward(&cls, candidates);
#else
		int cls = __builtin_ctz(candidates);
#endif

		free_block *block = freelists[cls];
		freelists[cls] = block->next;

		if (!freelists[cls])
		{
			occupied_buckets &= ~(1u << cls); // Bucket now empty
		}

		return block;
	}

	static void *
	alloc(size_t size)
	{
		if (!base || size == 0 || size >= reserved_capacity)
		{
			return nullptr;
		}

		void *recycled = try_alloc_from_freelist(size);
		if (recycled)
		{
			return recycled;
		}

		uint8_t *aligned = (uint8_t *)(((uintptr_t)current + (Align - 1)) & ~(Align - 1));
		uint8_t *next = aligned + size;

		if (!ensure_committed(next))
		{
			return nullptr;
		}

		current = next;
		return aligned;
	}

	static bool
	ensure_committed(uint8_t *next)
	{
		if (next <= base + committed_capacity)
		{
			return true;
		}
		size_t needed = next - base;

		if (max_capacity > 0 && needed > max_capacity)
		{
			fprintf(stderr, "Arena exhausted: requested %zu, max %zu\n", needed, max_capacity);
			return false;
		}

		if (needed > reserved_capacity)
		{
			fprintf(stderr, "Arena exhausted: requested %zu, reserved %zu\n", needed, reserved_capacity);
			return false;
		}

		size_t new_committed = virtual_memory::round_to_pages(needed);

		if (max_capacity > 0 && new_committed > max_capacity)
		{
			new_committed = max_capacity;
		}

		if (new_committed > reserved_capacity)
		{
			new_committed = reserved_capacity;
		}

		size_t commit_size = new_committed - committed_capacity;
		if (!virtual_memory::commit(base + committed_capacity, commit_size))
		{
			fprintf(stderr, "Failed to commit memory: %zu bytes\n", commit_size);
			return false;
		}

		committed_capacity = new_committed;
		return true;
	}

	/*
	 * Unaligned bump allocation for stream writers and other sequential data.
	 * Skips freelists and alignment, just moves the pointer forward.
	 */
	static void *
	bump_alloc(size_t size)
	{
		if (!base || size == 0 || size >= reserved_capacity)
		{
			return nullptr;
		}

		uint8_t *result = current;
		uint8_t *next = current + size;

		if (!ensure_committed(next))
		{
			return nullptr;
		}

		current = next;
		return result;
	}

	/*
	 * Instead of zeroing all pages, tell the OS
	 * to discard the page contents. Next access will get zero pages.
	 */
	static void
	zero_pages_lazy(void *addr, size_t size)
	{
#ifdef _WIN32
		VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
#elif defined(__linux__)
		madvise(addr, size, MADV_DONTNEED);
#else
		memset(addr, 0, size);
#endif
	}

	static void
	reset()
	{
		current = base;

		if constexpr (zero_on_reset)
		{
			if (base && committed_capacity > 0)
			{
				zero_pages_lazy(base, committed_capacity);
			}
		}

		for (int i = 0; i < 32; i++)
		{
			freelists[i] = nullptr;
		}
		occupied_buckets = 0;
	}

	static void
	reset_and_decommit()
	{
		current = base;

		if (committed_capacity > initial_commit)
		{
			virtual_memory::decommit(base + initial_commit, committed_capacity - initial_commit);
			committed_capacity = initial_commit;
		}

		if constexpr (zero_on_reset)
		{
			if (base && committed_capacity > 0)
			{
				zero_pages_lazy(base, committed_capacity);
			}
		}

		for (int i = 0; i < 32; i++)
		{
			freelists[i] = nullptr;
		}
		occupied_buckets = 0;
	}

	static size_t
	used()
	{
		return base ? current - base : 0;
	}
	static size_t
	committed()
	{
		return committed_capacity;
	}
	static size_t
	reserved()
	{
		return reserved_capacity;
	}

	static void
	print_info()
	{
		printf("Arena<%s>: [%p - %p] using %zu KB of %zu KB reserved\n", typeid(Tag).name(), base,
			   base + reserved_capacity, used() / (1024), reserved_capacity / (1024));
	}
};

/*
 * Creating a stream_writer and calling .write will use the unaligned bump allocator.
 *
 * This is only a contiguous stream if no other allocations from the same arena are made
 * while the stream is ongoing. If discontinuity is detected, it will fail.
 */
template <typename Tag = global_arena> struct stream_result
{
	const char *data;
	size_t		size;
	size_t		allocated_size;

	std::string_view
	as_view() const
	{
		return std::string_view(data, size);
	}

	const char *
	c_str() const
	{
		return data;
	}
};

template <typename Tag = global_arena> struct stream_writer
{
	uint8_t *start;
	size_t	 written;

	static stream_writer
	begin()
	{
		if (!arena<Tag>::base)
		{
			arena<Tag>::init();
		}
		return {arena<Tag>::current, 0};
	}

	bool
	write(const void *data, size_t size)
	{
		uint8_t *dest = (uint8_t *)arena<Tag>::bump_alloc(size);
		if (!dest)
		{
			return false;
		}

		// Check contiguity: if we've written before, the new allocation
		if (written > 0 && dest != start + written)
		{
			size_t gap = dest - (start + written);
			fprintf(stderr, "stream_writer: non-contiguous allocation detected\n");
			fprintf(stderr, "  Expected next allocation at: %p\n", start + written);
			fprintf(stderr, "  Actually allocated at: %p\n", dest);
			fprintf(stderr, "  Gap of %zu bytes - something else allocated from arena\n", gap);

			// Roll back the allocation and fail
			arena<Tag>::current = dest;
			return false;
		}

		memcpy(dest, data, size);
		written += size;
		return true;
	}

	bool
	write(std::string_view sv)
	{
		return write(sv.data(), sv.size());
	}

	bool
	write(const char *str)
	{
		return write(str, strlen(str));
	}

	size_t
	size() const
	{
		return written;
	}

	stream_result<Tag>
	finish()
	{
		uint8_t *null_pos = (uint8_t *)arena<Tag>::bump_alloc(1);
		if (!null_pos)
		{
			return {nullptr, 0, 0};
		}

		// Check that null terminator is contiguous too
		if (written > 0 && null_pos != start + written)
		{
			size_t gap = null_pos - (start + written);
			fprintf(stderr, "stream_writer::finish: non-contiguous allocation detected\n");
			fprintf(stderr, "  Expected null terminator at: %p\n", start + written);
			fprintf(stderr, "  Actually allocated at: %p\n", null_pos);
			fprintf(stderr, "  Gap of %zu bytes - something else allocated from arena\n", gap);

			arena<Tag>::current = null_pos;
			return {nullptr, 0, 0};
		}

		*null_pos = '\0';
		return {(const char *)start, written, written + 1};
	}

	void
	abandon()
	{
		arena<Tag>::current = start;
		written = 0;
	}
};

template <typename Tag = global_arena>
void
stream_result_reclaim(const stream_result<Tag> &result)
{
	arena<Tag>::reclaim((void *)result.data, result.allocated_size);
}

template <size_t N> struct fixed_string
{
	char data[N];

  private:
	void
	set_from_string(const char *str, size_t len)
	{
		if (len >= N)
		{
			len = N - 1;
		}
		memcpy(data, str, len);
		data[len] = '\0';
		if (len + 1 < N)
		{
			memset(data + len + 1, 0, N - len - 1);
		}
	}

  public:
	fixed_string()
	{
		memset(data, 0, N);
	}

	fixed_string(const char *str)
	{
		if (str)
		{
			set_from_string(str, strlen(str));
		}
		else
		{
			memset(data, 0, N);
		}
	}

	fixed_string(std::string_view sv)
	{
		set_from_string(sv.data(), sv.size());
	}

	fixed_string &
	operator=(const char *str)
	{
		if (str)
		{
			set_from_string(str, strlen(str));
		}
		else
		{
			memset(data, 0, N);
		}
		return *this;
	}

	bool
	operator==(const fixed_string &other) const
	{
		return strcmp(data, other.data) == 0;
	}

	bool
	operator==(const char *str) const
	{
		return str && strcmp(data, str) == 0;
	}

	bool
	operator==(std::string_view sv) const
	{
		size_t my_len = strlen(data);
		return my_len == sv.size() && memcmp(data, sv.data(), my_len) == 0;
	}

	bool
	operator!=(const fixed_string &other) const
	{
		return !(*this == other);
	}

	size_t
	length() const
	{
		return strlen(data);
	}

	bool
	empty() const
	{
		return data[0] == '\0';
	}

	const char *
	c_str() const
	{
		return data;
	}

	char *
	c_str()
	{
		return data;
	}
};

/*
 * IMO, with an arena, the idea of a string owning itself
 * doesn't really track. So instead make heavy use of string_views
 * with (duplicate allowing) interning
 */

template <typename Tag = global_arena>
std::string_view
arena_intern(const char *str, size_t len = 0)
{
	size_t l = len != 0 ? len : std::strlen(str);
	size_t alloc_size = l;
	char  *memory = (char *)arena<Tag>::alloc(alloc_size);
	if (!memory)
	{
		return std::string_view();
	}
	memcpy(memory, str, l);
	return std::string_view(memory, l);
}

template <typename Tag = global_arena>
std::string_view
arena_intern(std::string_view sv)
{
	return arena_intern<Tag>(sv.data(), sv.size());
}

template <typename Tag = global_arena>
void
arena_reclaim_string(std::string_view str)
{
	void *memory = (void *)(str.begin());
	arena<Tag>::reclaim(memory, str.size());
}

/*
 * The following dynamically resizing containers pull from their
 * template-specified arena. There isn't any RAII mechanism for
 * reclamation, but calling .clear() will call reclaim memory they
 * have.
 *
 * When the arena resets, it's possible for the stack allocated
 * containers themselves to get out of sync, where their 'm_size' value
 * is non-zero, but the memory has been decommited.
 * If a container lives through it's arenas reset, ensure it calls 'clear',
 * not for memory reclamation, but to reset it's metadata (m_size, etc).
 */

template <typename T>
inline T
round_up_power_of_2(T n)
{
	static_assert(std::is_unsigned_v<T>);
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	if constexpr (sizeof(T) > 1)
	{
		n |= n >> 16;
	}

	if constexpr (sizeof(T) > 4)
	{
		n |= n >> 32;
	}

	return n + 1;
}

inline uint32_t
hash_bytes(const void *data, size_t len)
{
	if (!data || len == 0)
	{
		return 1;
	}

	const uint8_t *bytes = (const uint8_t *)data;
	uint32_t	   h = 2166136261u;
	for (size_t i = 0; i < len; i++)
	{
		h ^= bytes[i];
		h *= 16777619u;
	}
	return h ? h : 1;
}

inline uint32_t
hash_int(uint64_t x)
{
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	x = x ^ (x >> 31);
	return static_cast<uint32_t>(x) ? static_cast<uint32_t>(x) : 1;
}
template <typename T, typename arena_tag = global_arena> struct array
{
  private:
	T		*m_data = nullptr;
	uint32_t m_size = 0;
	uint32_t m_capacity = 0;

  public:
	array() = default;

	array(std::initializer_list<T> init)
	{
		if (init.size() > 0)
		{
			reserve(init.size());
			for (const T &value : init)
			{
				push(value);
			}
		}
	}

	bool
	reserve(uint32_t min_capacity)
	{
		if (m_capacity >= min_capacity)
		{
			return true;
		}

		uint32_t new_cap = m_capacity ? m_capacity * 2 : 16;
		if (new_cap < min_capacity)
		{
			new_cap = min_capacity;
		}

		T		*old_data = m_data;
		uint32_t old_capacity = m_capacity;

		m_data = (T *)arena<arena_tag>::alloc(new_cap * sizeof(T));
		if (!m_data)
		{
			m_data = old_data;
			return false;
		}
		m_capacity = new_cap;

		if (old_data && m_size > 0)
		{
			memcpy(m_data, old_data, m_size * sizeof(T));
			arena<arena_tag>::reclaim(old_data, old_capacity * sizeof(T));
		}
		return true;
	}

	bool
	push(const T &value)
	{
		if (m_size >= m_capacity)
		{
			if (!reserve(m_size + 1))
			{
				return false;
			}
		}
		m_data[m_size++] = value;
		return true;
	}

	T *
	pop_back()
	{
		if (m_size == 0)
		{
			return nullptr;
		}
		return &m_data[--m_size];
	}

	void
	clear()
	{
		if (m_data && m_capacity > 0)
		{
			arena<arena_tag>::reclaim(m_data, m_capacity * sizeof(T));
		}
		m_data = nullptr;
		m_size = 0;
		m_capacity = 0;
	}

	T *
	get(uint32_t index)
	{
		if (index >= m_size)
		{
			return nullptr;
		}
		return &m_data[index];
	}

	T &
	operator[](uint32_t index)
	{
		return m_data[index];
	}

	T *
	back()
	{
		return m_size > 0 ? &m_data[m_size - 1] : nullptr;
	}

	T *
	front()
	{
		return m_size > 0 ? &m_data[0] : nullptr;
	}

	T *
	begin()
	{
		return m_data;
	}
	T *
	end()
	{
		return m_data + m_size;
	}

	bool
	empty()
	{
		return m_size == 0;
	}
	uint32_t
	size()
	{
		return m_size;
	}
	uint32_t
	capacity()
	{
		return m_capacity;
	}
	T *
	data()
	{
		return m_data;
	}
};

template <typename T, typename arena_tag = global_arena> struct queue
{

	queue() = default;

	queue(std::initializer_list<T> init)
	{
		if (init.size() > 0)
		{
			reserve(init.size());
			for (const T &value : init)
			{
				push(value);
			}
		}
	}

  private:
	T		*m_data = nullptr;
	uint32_t m_capacity = 0;
	uint32_t m_head = 0;
	uint32_t m_tail = 0;
	uint32_t m_count = 0;

	struct queue_iterator
	{
		T		*data;
		uint32_t capacity;
		uint32_t index;
		uint32_t remaining;

		T &
		operator*()
		{
			return data[index];
		}
		T *
		operator->()
		{
			return &data[index];
		}

		queue_iterator &
		operator++()
		{
			if (remaining > 0)
			{
				index = (index + 1) % capacity;
				remaining--;
			}
			return *this;
		}

		bool
		operator!=(const queue_iterator &other) const
		{
			return remaining != other.remaining;
		}
	};

  public:
	bool
	reserve(uint32_t min_capacity)
	{
		if (m_capacity >= min_capacity)
		{
			return true;
		}

		uint32_t new_cap = m_capacity ? m_capacity * 2 : 16;
		if (new_cap < min_capacity)
		{
			new_cap = round_up_power_of_2(min_capacity);
		}

		T		*old_data = m_data;
		uint32_t old_cap = m_capacity;

		m_data = (T *)arena<arena_tag>::alloc(new_cap * sizeof(T));
		if (!m_data)
		{
			m_data = old_data;
			return false;
		}
		m_capacity = new_cap;

		if (m_count > 0 && old_data)
		{
			if (m_head < m_tail)
			{
				memcpy(m_data, old_data + m_head, m_count * sizeof(T));
			}
			else
			{
				uint32_t first_part = old_cap - m_head;
				memcpy(m_data, old_data + m_head, first_part * sizeof(T));
				memcpy(m_data + first_part, old_data, m_tail * sizeof(T));
			}
		}

		if (old_data)
		{
			arena<arena_tag>::reclaim(old_data, old_cap * sizeof(T));
		}

		m_head = 0;
		m_tail = m_count;
		return true;
	}

	bool
	push(const T &value)
	{
		if (m_count == m_capacity)
		{
			if (!reserve(m_count + 1))
			{
				return false;
			}
		}
		m_data[m_tail] = value;
		m_tail = (m_tail + 1) % m_capacity;
		m_count++;
		return true;
	}

	T *
	pop()
	{
		if (m_count == 0)
		{
			return nullptr;
		}
		T *result = &m_data[m_head];
		m_head = (m_head + 1) % m_capacity;
		m_count--;
		return result;
	}

	T *
	front()
	{
		return m_count > 0 ? &m_data[m_head] : nullptr;
	}

	T *
	back()
	{
		if (m_count == 0)
			return nullptr;
		uint32_t back_idx = (m_tail + m_capacity - 1) % m_capacity;
		return &m_data[back_idx];
	}

	void
	clear()
	{
		if (m_data && m_capacity > 0)
		{
			arena<arena_tag>::reclaim(m_data, m_capacity * sizeof(T));
		}
		m_data = nullptr;
		m_capacity = 0;
		m_head = 0;
		m_tail = 0;
		m_count = 0;
	}

	queue_iterator
	begin()
	{
		return {m_data, m_capacity, m_head, m_count};
	}
	queue_iterator
	end()
	{
		return {m_data, m_capacity, 0, 0};
	}

	bool
	empty()
	{
		return m_count == 0;
	}
	uint32_t
	size()
	{
		return m_count;
	}
	uint32_t
	capacity()
	{
		return m_capacity;
	}
};

template <typename K, typename V, typename arena_tag = global_arena> struct hash_map
{
	struct entry
	{
		K		 key;
		V		 value;
		uint32_t hash;
		uint8_t	 state;
	};

  public:
	hash_map() = default;
	hash_map(std::initializer_list<std::pair<K, V>> init)
	{
		if (init.size() > 0)
		{
			reserve(init.size() * 2);
			for (const auto &[key, value] : init)
			{
				insert(key, value);
			}
		}
	}

  private:
	entry	*m_data = nullptr;
	uint32_t m_capacity = 0;
	uint32_t m_size = 0;
	uint32_t m_tombstones = 0;

	template <typename T, typename = void> struct has_c_str : std::false_type
	{
	};

	template <typename T> struct has_c_str<T, std::void_t<decltype(std::declval<T>().c_str())>> : std::true_type
	{
	};

	uint32_t
	hash_key(const K &key) const
	{
		if constexpr (has_c_str<K>::value)
		{
			const char *s = key.c_str();
			return hash_bytes(s, strlen(s));
		}
		else if constexpr (std::is_same_v<K, std::string_view>)
		{
			return hash_bytes(key.data(), key.size());
		}
		else if constexpr (std::is_pointer_v<K>)
		{
			return hash_int(reinterpret_cast<uintptr_t>(key));
		}
		else if constexpr (std::is_integral_v<K>)
		{
			return hash_int(static_cast<uint64_t>(key));
		}
		else
		{
			static_assert(sizeof(K) == 0, "Unsupported key type");
			return 0;
		}
	}

	V *
	insert_into(const K &key, uint32_t hash, const V &value)
	{
		uint32_t mask = m_capacity - 1;
		uint32_t idx = hash & mask;
		uint32_t first_deleted = -1;

		while (true)
		{
			entry &e = m_data[idx];

			if (e.state == 0)
			{
				entry *target = (first_deleted != -1) ? &m_data[first_deleted] : &e;
				target->key = key;
				target->value = value;
				target->hash = hash;
				target->state = 1;

				if (first_deleted != -1)
				{
					m_tombstones--;
				}
				m_size++;
				return &target->value;
			}

			if (e.state == 2 && first_deleted == -1)
			{
				first_deleted = idx;
			}
			else if (e.state == 1 && e.hash == hash && e.key == key)
			{
				e.value = value;
				return &e.value;
			}

			idx = (idx + 1) & mask;
		}
	}

	struct map_iterator
	{
		entry	*entries;
		uint32_t capacity;
		uint32_t index;

		void
		advance_to_next_valid()
		{
			while (index < capacity && entries && entries[index].state != 1)
			{
				++index;
			}
		}

		map_iterator(entry *e, uint32_t cap, uint32_t idx) : entries(e), capacity(cap), index(idx)
		{
			advance_to_next_valid();
		}

		std::pair<K &, V &>
		operator*()
		{
			return {entries[index].key, entries[index].value};
		}

		struct arrow_proxy
		{
			std::pair<K &, V &> p;
			std::pair<K &, V &> *
			operator->()
			{
				return &p;
			}
		};

		arrow_proxy
		operator->()
		{
			return {{entries[index].key, entries[index].value}};
		}

		map_iterator &
		operator++()
		{
			++index;
			advance_to_next_valid();
			return *this;
		}

		bool
		operator!=(const map_iterator &other) const
		{
			return index != other.index;
		}
	};

  public:
	bool
	reserve(uint32_t min_capacity = 16)
	{
		min_capacity = round_up_power_of_2(min_capacity);

		if (m_capacity >= min_capacity)
		{
			return true;
		}

		if (!m_data)
		{
			m_data = (entry *)arena<arena_tag>::alloc(min_capacity * sizeof(entry));
			if (!m_data)
			{
				return false;
			}
			memset(m_data, 0, min_capacity * sizeof(entry));
			m_capacity = min_capacity;
			m_size = 0;
			m_tombstones = 0;
			return true;
		}

		uint32_t old_capacity = m_capacity;
		entry	*old_data = m_data;

		uint32_t new_capacity = m_capacity * 2;
		if (new_capacity < min_capacity)
		{
			new_capacity = min_capacity;
		}

		entry *new_data = (entry *)arena<arena_tag>::alloc(new_capacity * sizeof(entry));
		if (!new_data)
		{
			return false;
		}
		memset(new_data, 0, new_capacity * sizeof(entry));
		m_data = new_data;
		m_capacity = new_capacity;

		uint32_t old_size = m_size;
		m_size = 0;
		m_tombstones = 0;

		for (uint32_t i = 0; i < old_capacity; i++)
		{
			if (old_data[i].state == 1)
			{
				insert_into(old_data[i].key, old_data[i].hash, old_data[i].value);
			}
		}

		if (old_data)
		{
			arena<arena_tag>::reclaim(old_data, old_capacity * sizeof(entry));
		}
		return true;
	}

	V *
	get(const K &key)
	{
		if (!m_data || m_size == 0)
		{
			return nullptr;
		}

		uint32_t hash = hash_key(key);
		uint32_t mask = m_capacity - 1;
		uint32_t idx = hash & mask;

		while (true)
		{
			entry &e = m_data[idx];
			if (e.state == 0)
			{
				return nullptr;
			}
			if (e.state == 1 && e.hash == hash && e.key == key)
			{
				return &e.value;
			}
			idx = (idx + 1) & mask;
		}
	}

	V *
	insert(const K &key, const V &value)
	{
		if (!m_data)
		{
			if (!reserve(16))
			{
				return nullptr;
			}
		}

		if ((m_size + m_tombstones) * 4 >= m_capacity * 3)
		{
			if (!reserve(m_capacity * 2))
			{
				return nullptr;
			}
		}

		uint32_t hash = hash_key(key);
		return insert_into(key, hash, value);
	}

	bool
	remove(const K &key)
	{
		if (!m_data || m_size == 0)
		{
			return false;
		}

		uint32_t hash = hash_key(key);
		uint32_t mask = m_capacity - 1;
		uint32_t idx = hash & mask;

		while (true)
		{
			entry &e = m_data[idx];
			if (e.state == 0)
			{
				return false;
			}
			if (e.state == 1 && e.hash == hash && e.key == key)
			{
				e.state = 2;
				m_size--;
				m_tombstones++;
				return true;
			}
			idx = (idx + 1) & mask;
		}
	}

	void
	clear()
	{
		if (m_data && m_capacity > 0)
		{
			arena<arena_tag>::reclaim(m_data, m_capacity * sizeof(entry));
		}
		m_data = nullptr;
		m_capacity = 0;
		m_size = 0;
		m_tombstones = 0;
	}

	map_iterator
	begin()
	{
		if (!m_data)
			return end();
		return map_iterator(m_data, m_capacity, 0);
	}

	map_iterator
	end()
	{
		return map_iterator(m_data, m_capacity, m_capacity);
	}

	bool
	empty()
	{
		return m_size == 0;
	}
	uint32_t
	size()
	{
		return m_size;
	}
	uint32_t
	capacity()
	{
		return m_capacity;
	}
	bool
	contains(const K &key)
	{
		return get(key) != nullptr;
	}
	entry *
	data()
	{
		return m_data;
	}
};

template <typename K, typename arena_tag = global_arena> using hash_set = hash_map<K, char, arena_tag>;
