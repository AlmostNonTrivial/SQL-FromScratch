
/*
 * SQL From Scratch - Educational Database Engine
 *
 * Ephemeral Tree
 */

#pragma once
#include "common.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstring>

enum TREE_COLOR : uint8_t
{
	RED = 0,
	BLACK = 1
};

struct ephemeral_tree_node
{
	uint8_t				*data;	 // Points to key, record follows at offset key_size
	ephemeral_tree_node *left;	 // Left child (smaller keys)
	ephemeral_tree_node *right;	 // Right child (larger keys)
	ephemeral_tree_node *parent; // Parent pointer for tree operations
	TREE_COLOR			 color;	 // RED or BLACK for balancing
};

struct ephemeral_tree
{
	ephemeral_tree_node *root;			   // Root node (null for empty tree)
	data_type			 key_type;		   // Type of keys stored
	uint32_t			 key_size;		   // Size of each key in bytes
	uint32_t			 record_size;	   // Size of each record in bytes
	uint32_t			 node_count;	   // Number of nodes in tree
	uint32_t			 data_size;		   // Total size: key_size + record_size
	bool				 allow_duplicates; // Whether duplicate keys are permitted
};

struct et_cursor
{
	ephemeral_tree		 tree;	  // Tree being traversed
	ephemeral_tree_node *current; // Current position in tree

	enum ET_CURSOR_STATE
	{
		INVALID,
		VALID,
		AT_END
	} state;
};

// Flags: bit 0 = allow_duplicates
ephemeral_tree
et_create(data_type key_type, uint32_t record_size, uint8_t flags = 0x01);

void
et_clear(ephemeral_tree *tree);

bool
et_insert(ephemeral_tree *tree, void *key, void *record);

bool
et_delete(ephemeral_tree *tree, void *key);

bool
et_cursor_seek(et_cursor *cursor, const void *key, COMPARISON_OP op = EQ);
bool
et_cursor_first(et_cursor *cursor);
bool
et_cursor_last(et_cursor *cursor);

bool
et_cursor_next(et_cursor *cursor);
bool
et_cursor_previous(et_cursor *cursor);
bool
et_cursor_has_next(et_cursor *cursor);
bool
et_cursor_has_previous(et_cursor *cursor);

void *
et_cursor_key(et_cursor *cursor);
void *
et_cursor_record(et_cursor *cursor);
bool
et_cursor_is_valid(et_cursor *cursor);

bool
et_cursor_insert(et_cursor *cursor, void *key, void *record);
bool
et_cursor_update(et_cursor *cursor, void *record);
bool
et_cursor_delete(et_cursor *cursor);

void
et_validate(const ephemeral_tree *tree);
void
et_print(const ephemeral_tree *tree);
