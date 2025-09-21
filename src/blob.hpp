/*
 * SQL From Scratch
 *
 * Binary Large Object (BLOB) Storage
 */

#pragma once
#include <cstddef>
#include <cstdint>

uint32_t
blob_create(void *data, uint32_t size);

void
blob_delete(uint32_t first_page);

uint32_t
blob_get_size(uint32_t first_page);

uint8_t *
blob_read_full(uint32_t first_page, size_t *size);

/*
4096 byte page example

 1. SINGLE PAGE BLOB (fits in one page)
 ----------------------------------------

	 btree column
	 ┌──────────┐
	 │ page: 42 │ ──────┐
	 └──────────┘       │
						▼
				   Page #42 (4096 bytes)
				   ┌──────────────────────────────────────┐
				   │ index: 42   (4 bytes)                │
				   │ next:  0    (4 bytes) [terminates]   │
				   │ size:  1500 (2 bytes)                │
				   │ flags: 0    (2 bytes)                │
				   ├──────────────────────────────────────┤
				   │ data: [1500 bytes of actual content] │
				   │       [............................] │
				   │       [2584 bytes unused]            │
				   └──────────────────────────────────────┘
						  12 byte header + 4084 data area


 2. MULTI-PAGE BLOB (chained across 3 pages)
 ---------------------------------------------

	 btree column
	 ┌──────────┐
	 │ page: 42 │ ──────┐
	 └──────────┘       │
						▼
				   Page #42                    Page #57                    Page #89
	 ┌─────────────────────────┐  ┌─────────────────────────┐  ┌─────────────────────────┐
	 │ index: 42               │  │ index: 57               │  │ index: 89               │
	 │ next:  57 ──────────────┼─▶  next:  89   ────────────┼──▶ next:  0  [end]         │
	 │ size:  4084             │  │ size:  4084             │  │ size:  2000             │
	 │ flags: 0                │  │ flags: 0                │  │ flags: 0                │
	 ├─────────────────────────┤  ├─────────────────────────┤  ├─────────────────────────┤
	 │ data: [4084 bytes full] │  │ data: [4084 bytes full] │  │ data: [2000 bytes]      │
	 │       [████████████████]│  │       [████████████████]│  │       [████████]        │
	 │       [████████████████]│  │       [████████████████]│  │       [        ]        │
	 └─────────────────────────┘  └─────────────────────────┘  └─────────────────────────┘
		  Total: 10,168 bytes of user data across 3 pages

 */
