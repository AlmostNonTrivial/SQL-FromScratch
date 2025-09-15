/*
 * SQL From Scratch - Educational Database Engine
 *
 * B+Tree Storage
 */

#pragma once

#pragma once
#include "types.hpp"
#include <cstdint>

struct btree
{
	uint32_t root_page_index; /* Root node location */

	/* Node capacity limits */
	uint32_t internal_max_keys; /* Max keys in internal node */
	uint32_t leaf_max_keys;		/* Max keys in leaf node */
	uint32_t internal_min_keys; /* Min keys (non-root internal) */
	uint32_t leaf_min_keys;		/* Min keys (non-root leaf) */

	/* Split points for overflow handling */
	uint32_t internal_split_index; /* Where to split internal nodes */
	uint32_t leaf_split_index;	   /* Where to split leaf nodes */

	/* Data configuration */
	uint32_t  record_size;	 /* Size of value/record */
	uint32_t  node_key_size; /* Size of key */
	data_type node_key_type; /* Key data type */
};

btree
bt_create(data_type key, uint32_t record_size, bool allocate_node);
bool
bt_clear(btree *tree);

enum BT_CURSOR_STATE : uint8_t
{
	BT_CURSOR_INVALID = 0,
	BT_CURSOR_VALID = 1,
};

struct bt_cursor
{
	btree		   *tree;		/* Tree being traversed */
	uint32_t		leaf_page;	/* Current leaf page */
	uint32_t		leaf_index; /* Position in leaf */
	BT_CURSOR_STATE state;		/* Cursor validity */
};
bool
bt_cursor_seek(bt_cursor *cursor, void *key, COMPARISON_OP op = EQ);
bool
bt_cursor_previous(bt_cursor *cursor);
bool
bt_cursor_next(bt_cursor *cursor);
bool
bt_cursor_last(bt_cursor *cursor);
bool
bt_cursor_first(bt_cursor *cursor);
bool
bt_cursor_update(bt_cursor *cursor, void *record);
bool
bt_cursor_insert(bt_cursor *cursor, void *key, void *record);
bool
bt_cursor_delete(bt_cursor *cursor);
void *
bt_cursor_key(bt_cursor *cursor);
void *
bt_cursor_record(bt_cursor *cursor);
bool
bt_cursoris_valid(bt_cursor *cursor);
bool
bt_cursorhas_next(bt_cursor *cursor);
bool
bt_cursorhas_previous(bt_cursor *cursor);
void
bt_validate(btree *tree);
void
bt_print(btree *tree);
/*


LEAF NODE MEMORY LAYOUT
-----------------------
┌────────────────────────────────────────────────────────────────────────┐
│ Header (24 bytes) │        Keys Area         │      Records Area       │
├───────────────────┼──────────────────────────┼─────────────────────────┤
│ index  (4)        │ key[0] │ key[1] │ key[2] │ rec[0] │ rec[1] │ rec[2]│
│ parent (4)        │        │        │        │        │        │       │
│ next   (4)        │  Keys stored             │  Records stored         │
│ prev   (4)        │  contiguously            │  contiguously           │
│ num_keys (4)      │                          │                         │
│ is_leaf (4)       │                          │                         │
└────────────────────────────────────────────────────────────────────────┘
					↑                          ↑
					data[0]                    data + (max_keys * key_size)



1. SHIFT_KEYS_RIGHT - Making space for insertion


BEFORE: (num_keys = 3, inserting at index 1)
────────────────────────────────────────────   Want to insert key 15
Keys:    [10] [20] [30] [  ] [  ]
		 ↑    ↑    ↑
		 0    1    2

Records: [A]  [B]  [C]  [ ]  [ ]
		 ↑    ↑    ↑
		 0    1    2

OPERATION: SHIFT_KEYS_RIGHT(node, from_idx=1, count=2)
──────────────────────────────────────────────────────
memcpy(GET_KEY_AT(node, 2),    // destination: key[2]
	   GET_KEY_AT(node, 1),    // source: key[1]
	   2 * key_size)           // copy key[1] and key[2]

Visual:
	   from_idx
		  ↓
Keys:    [10] [20] [30] [  ] [  ]
			  └─────┴────→ copy 2 keys
Keys:    [10] [20] [20] [30] [  ]
			  gap  └─────┴─── shifted

AFTER: (ready to insert at index 1)
─────────────────────────────────
Keys:    [10] [15] [20] [30] [  ]
			  ↑
			  ready for new key

Records: [A]  [??] [B]  [C]  [ ]
			  ↑
			  ready for new record
			  (after SHIFT_RECORDS_RIGHT)



2. SHIFT_RECORDS_RIGHT - Corresponding record shift


OPERATION: SHIFT_RECORDS_RIGHT(node, from_idx=1, count=2)
──────────────────────────────────────────────────────────
uint8_t *base = GET_RECORD_DATA(node);
memcpy(base + (2 * record_size),    // destination: rec[2]
	   base + (1 * record_size),    // source: rec[1]
	   2 * record_size)             // copy rec[1] and rec[2]

Visual:
		 from_idx
			↓
Records: [A]  [B]  [C]  [ ]  [ ]
			  └────┴─────→ copy 2 records
Records: [A]  [B]  [B]  [C]  [ ]
			  gap  └────┴─── shifted



3. SHIFT_KEYS_LEFT - Removing entry 15


BEFORE: (num_keys = 4, deleting at index 1)
───────────────────────────────────────────
Keys:    [10] [15] [20] [30] [  ]
		 ↑    ↑    ↑    ↑
		 0    1    2    3
			  DEL

Records: [A]  [X]  [B]  [C]  [ ]
		 ↑    ↑    ↑    ↑
		 0    1    2    3
			  DEL

OPERATION: SHIFT_KEYS_LEFT(node, from_idx=1, count=2)
─────────────────────────────────────────────────────
memcpy(GET_KEY_AT(node, 1),    // destination: key[1]
	   GET_KEY_AT(node, 2),    // source: key[2]
	   2 * key_size)           // copy key[2] and key[3]

Visual:
			  from_idx
				 ↓
Keys:    [10] [15] [20] [30] [  ]
			  ←────└────┴─── copy 2 keys
Keys:    [10] [20] [30] [30] [  ]
			  └────┴─── shifted
						stale (will be ignored)

AFTER: (num_keys decremented to 3)
───────────────────────────────────
Keys:    [10] [20] [30] [××] [  ]
		 ↑    ↑    ↑
		 0    1    2    (ignored)

Records: [A]  [B]  [C]  [××] [ ]
		 ↑    ↑    ↑
		 0    1    2    (ignored)



4. COMPLETE INSERT EXAMPLE


Initial state: num_keys = 3
─────────────────────────────
Keys:    [10] [20] [30]
Records: [A]  [B]  [C]

Want to insert: key=15, record=X at position 1

Step 1: Find insertion point (binary_search returns 1)
Step 2: SHIFT_KEYS_RIGHT(node, 1, 2)
		Keys:    [10] [20] [20] [30]
Step 3: SHIFT_RECORDS_RIGHT(node, 1, 2)
		Records: [A]  [B]  [B]  [C]
Step 4: COPY_KEY(GET_KEY_AT(node, 1), 15)
		Keys:    [10] [15] [20] [30]
Step 5: COPY_RECORD(GET_RECORD_AT(node, 1), X)
		Records: [A]  [X]  [B]  [C]
Step 6: node->num_keys++
		num_keys = 4

Final state:
────────────
Keys:    [10] [15] [20] [30]
Records: [A]  [X]  [B]  [C]



5. COMPLETE DELETE EXAMPLE


Initial state: num_keys = 4
─────────────────────────────
Keys:    [10] [15] [20] [30]
Records: [A]  [X]  [B]  [C]

Want to delete: key=15 at position 1

Step 1: Find deletion point (binary_search returns 1)
Step 2: Calculate entries_to_shift = 4 - 1 - 1 = 2
Step 3: SHIFT_KEYS_LEFT(node, 1, 2)
		Keys:    [10] [20] [30] [30]
Step 4: SHIFT_RECORDS_LEFT(node, 1, 2)
		Records: [A]  [B]  [C]  [C]
Step 5: node->num_keys--
		num_keys = 3

Final state:
────────────
Keys:    [10] [20] [30] [××]  (last entry ignored)
Records: [A]  [B]  [C]  [××]  (last entry ignored)
 */
