/*
 * SQL From Scratch
 *
 * B+Tree Storage
 *
 * The btree module is capable of creating, modifying, reading from, and deleting b+trees,
 * the storage backend for tables/indexes. It sits above the pager, with an array based view of the filesystem.
 *
 * Our pager abstracts the file system to give this view of the data file:
 *
 * char database_file[LARGE_NUMBER][PAGE_SIZE];
 * // or specifically:
 * base_page database_file[LARGE_NUMBER];
 *
 * Which can be reinterpreted as:
 *
 * btree_node database[LARGE_NUMBER]; // sizeof(btree_node) == PAGE_SIZE
 *
 * Multiple btrees can exist on a single file, the relationships between nodes
 * facilitated by integer indexes.
 *
 * For example, 2 btrees exist in a file, with roots at index 2 and 4. btreeA has 3 nodes in its tree,
 * with nodes at 1 and 3 being siblings, holding reference to the parent at 2.
 *
 * Because the pager reserves page 0, we can use 0 to denote null, for example, node.parent == 0 means it's the root
 *
 * btreeB is in the same file, at index 4.
 *
 * btreeA btreeA = {.root = 2};
 * btreeB btreeB = {.root = 4};
 *
 * Array view of File:
 * [pager_root, node1a{.parent = 2, .next = 3}, node2a{.parent = 0}, node3a{.parent = 2, .next = 0}, node1b{.parent = 0}];
 *
 *
 * A btree node only knows its layout at runtime: nodes have
 * a block of memory of NODE_DATA_SIZE which will fit their keys and child pointers/records
 * depending on whether they are an internal or leaf node respectively.
 *
 * For example:
 * btree tree = {.key_type = TYPE_INT};
 * btree_node node = {.data = {[NODE_DATA_SIZE]}};
 *
 * void* get_key(int x) {
 *     return node.data + type_size(tree.key_type) * x;
 * }
 *
 * Would be equivalent to:
 *
 * btree_node node = {.keys = {int keys[MAX_KEYS]}};
 * int* get_key(int x) {
 *     return &node.keys[x];
 * }
 *
 *
 * Child pointers are indexes used with the pager that represent the position in the data file
 * where index * PAGE_SIZE gives you the beginning of the node.
 *
 * The key type and size are known at runtime such that the btree can do comparisons
 * with them via dispatch aka
 *
 * type_compare(btree.node_key_type, uint8_t* key1, uint8_t* key2);
 *
 *
 * The B+tree doesn't interpret records, and only sees them as blocks of a given size aka btree.record_size.
 * The VM will give the btree a pointer to some memory that it has packed with columns, and the btree will
 * copy it into the given position based solely of the pointer and size
 *
 * A btree doesn't store its own type information, it's stored as an
 * entry in the master catalog btree, and loaded on start-up into the catalog.
 *
 *
 * For both existing and new btrees we call 'bt_create' with the key type and record_size
 * (the only salient information), and from that calculate how many max/min keys we can have
 * in a given internal and leaf node.
 *
 * To hammer home the abstraction layers at play: the btree has no concept of columns.
 *
 * tuple_format for table_x:
 * [uint32_t key, (char[16], uint32_t, int32_t) record]
 *
 * The first entry in the table_x as seen by the btree:
 * [4 byte key, type u32][opaque 24 bytes]
 *
 *
 * A simplification is that we hardcode the space a record can occupy from the types.
 * So if we have a record [u32, char16], and we populate it with: [23, "markymark"]
 * in memory/disk that will be [u32 data, "markymark\0\0\0\0\0\0\0"] such that there is wasted
 * space. A real database would have additional metadata in the header do pack and unpack rows
 * as tightly as possible.
 *
 * B+tree visualisation - https://www.cs.usfca.edu/~galles/visualization/BPlusTree.html
 *
 */

#include "btree.hpp"
#include "arena.hpp"
#include "common.hpp"
#include "types.hpp"
#include "pager.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <sys/types.h>
#include <utility>
#include <cstdio>
#include <string>

#define NODE_HEADER_SIZE 24
#define NODE_DATA_SIZE	 (PAGE_SIZE - NODE_HEADER_SIZE)
#define MIN_ENTRY_COUNT	 3

/*
 * B+Tree Node Structure
 *
 * Fixed-size structure occupying exactly 1 page. Contains metadata header
 * and flexible data area for keys and associated values.
 *
 * Internal Node Layout:
 *   [Header][Key0][Key1]...[KeyN][Child0][Child1]...[ChildN+1]
 *   Keys stored contiguously from start of data area
 *   Children stored after all keys
 *   N keys with N+1 children (Child[i] < Key[i] <= Child[i+1])
 *
 * Leaf Node Layout:
 *   [Header][Key0][Key1]...[KeyN][Record0][Record1]...[RecordN]
 *   Keys stored contiguously from start of data area
 *   Records stored after all keys
 *   Each key[i] maps to record[i] (1:1 correspondence)
 */
struct btree_node
{

	uint32_t index;	   /* This node's page index in database file */
	uint32_t parent;   /* Parent node's page index (0 if root) */
	uint32_t next;	   /* Next sibling in leaf chain (leaf only) */
	uint32_t previous; /* Previous sibling in leaf chain (leaf only) */
	uint32_t num_keys; /* Number of valid keys currently stored */
	uint32_t is_leaf;  /* Node type: 1 for leaf, 0 for internal */

	uint8_t data[NODE_DATA_SIZE];
};

static_assert(sizeof(btree_node) == PAGE_SIZE, "btree_node must be exactly PAGE_SIZE");

/*NOCOVER_START*/

/* Node type predicates */
#define IS_LEAF(node)	  ((node)->is_leaf)
#define IS_INTERNAL(node) (!(node)->is_leaf)
#define IS_ROOT(node)	  ((node)->parent == 0)

/* Node capacity accessors */
#define GET_MAX_KEYS(node)	  ((node)->is_leaf ? tree->leaf_max_keys : tree->internal_max_keys)
#define GET_MIN_KEYS(node)	  ((node)->is_leaf ? tree->leaf_min_keys : tree->internal_min_keys)
#define GET_SPLIT_INDEX(node) ((node)->is_leaf ? tree->leaf_split_index : tree->internal_split_index)

/* Node retrieval by page index (0 = null pointer) */
#define GET_NODE(index)	 (0 == index ? nullptr : reinterpret_cast<btree_node *>(pager_get(index)))
#define GET_ROOT()		 GET_NODE(tree->root_page_index)
#define GET_PARENT(node) GET_NODE((node)->parent)

/* Leaf chain navigation */
#define GET_NEXT(node) GET_NODE((node)->next)
#define GET_PREV(node) GET_NODE((node)->previous)

#define GET_KEY_AT(node, idx) ((node)->data + (idx) * tree->node_key_size)

/*
 * Children array access (internal nodes only)
 * Children stored after all keys: data + (max_keys * key_size)
 * Array contains (num_keys + 1) valid entries
 */
#define GET_CHILDREN(node)	 (reinterpret_cast<uint32_t *>((node)->data + tree->internal_max_keys * tree->node_key_size))
#define GET_CHILD(node, idx) GET_NODE(GET_CHILDREN(node)[idx])

/*
 * Record access (leaf nodes only)
 * Records stored after all keys: data + (max_keys * key_size)
 * Each key[i] corresponds to record[i]
 */
#define GET_RECORD_DATA(node)	 ((node)->data + tree->leaf_max_keys * tree->node_key_size)
#define GET_RECORD_AT(node, idx) (GET_RECORD_DATA(node) + (idx) * tree->record_size)

/*
 * B+tree split invariants
 */
#define NODE_IS_FULL(node)	  ((node)->num_keys >= GET_MAX_KEYS(node))
#define NODE_IS_MINIMAL(node) ((node)->num_keys <= GET_MIN_KEYS(node))
#define IS_UNDERFLOWING(node) ((node->num_keys < GET_MIN_KEYS(node)))
#define NODE_CAN_SPARE(node)  ((node)->num_keys > GET_MIN_KEYS(node))

#define ENSURE_SAVED(node) pager_ensure_journaled((node)->index)

/* Shift keys right by 1 position to make room for insertion */
#define SHIFT_KEYS_RIGHT(node, from_idx, count)                                                                        \
	memcpy(GET_KEY_AT(node, (from_idx) + 1), GET_KEY_AT(node, from_idx), (count) * tree->node_key_size)

/* Shift keys left by 1 position to remove deleted key */
#define SHIFT_KEYS_LEFT(node, from_idx, count)                                                                         \
	memcpy(GET_KEY_AT(node, from_idx), GET_KEY_AT(node, (from_idx) + 1), (count) * tree->node_key_size)

/* Shift records right (leaf nodes only) */
#define SHIFT_RECORDS_RIGHT(node, from_idx, count)                                                                     \
	do                                                                                                                 \
	{                                                                                                                  \
		uint8_t *_base = GET_RECORD_DATA(node);                                                                        \
		memcpy(_base + ((from_idx) + 1) * tree->record_size, _base + (from_idx) * tree->record_size,                   \
			   (count) * tree->record_size);                                                                           \
	} while (0)

/* Shift records left (leaf nodes only) */
#define SHIFT_RECORDS_LEFT(node, from_idx, count)                                                                      \
	do                                                                                                                 \
	{                                                                                                                  \
		uint8_t *_base = GET_RECORD_DATA(node);                                                                        \
		memcpy(_base + (from_idx) * tree->record_size, _base + ((from_idx) + 1) * tree->record_size,                   \
			   (count) * tree->record_size);                                                                           \
	} while (0)

/* Shift children right (internal nodes only) */
#define SHIFT_CHILDREN_RIGHT(node, from_idx, count)                                                                    \
	do                                                                                                                 \
	{                                                                                                                  \
		uint32_t *_children = GET_CHILDREN(node);                                                                      \
		memcpy(&_children[(from_idx) + 1], &_children[from_idx], (count) * sizeof(uint32_t));                          \
	} while (0)

/* Shift children left (internal nodes only) */
#define SHIFT_CHILDREN_LEFT(node, from_idx, count)                                                                     \
	do                                                                                                                 \
	{                                                                                                                  \
		uint32_t *_children = GET_CHILDREN(node);                                                                      \
		memcpy(&_children[from_idx], &_children[(from_idx) + 1], (count) * sizeof(uint32_t));                          \
	} while (0)

/* Copy multiple keys between nodes */
#define COPY_KEYS(src, src_idx, dst, dst_idx, count)                                                                   \
	memcpy(GET_KEY_AT(dst, dst_idx), GET_KEY_AT(src, src_idx), (count) * tree->node_key_size)

/* Copy multiple records between nodes */
#define COPY_RECORDS(src, src_idx, dst, dst_idx, count)                                                                \
	do                                                                                                                 \
	{                                                                                                                  \
		uint8_t *_src_records = GET_RECORD_DATA(src);                                                                  \
		uint8_t *_dst_records = GET_RECORD_DATA(dst);                                                                  \
		memcpy(_dst_records + (dst_idx) * tree->record_size, _src_records + (src_idx) * tree->record_size,             \
			   (count) * tree->record_size);                                                                           \
	} while (0)

/* Single element copy operations */
#define COPY_KEY(dst, src)	  memcpy(dst, src, tree->node_key_size)
#define COPY_RECORD(dst, src) memcpy(dst, src, tree->record_size)

/*NOCOVER_END*/

/*
 * Find a child's position within its parent's children array.
 *
 * This cannot use binary search because child pointers are not sorted
 * by their page indices. The children are positionally significant:
 * child[i] contains keys less than key[i], child[i+1] contains keys
 * greater than or equal to key[i].
 *
 */
static uint32_t
find_child_index(btree *tree, btree_node *parent, btree_node *child)
{
	uint32_t *children = GET_CHILDREN(parent);
	for (uint32_t i = 0; i <= parent->num_keys; i++)
	{
		if (children[i] == child->index)
		{
			return i;
		}
	}
	assert(false && "Child node not found in parent's children array, possible tree corruption");
	return 0;
}

/*
 * Binary search within a node to find key position.
 *
 * Returns the index where the key either exists or should be inserted.
 * For leaf nodes, returns exact match position or insertion point.
 * For internal nodes, returns the child index to follow.
 *
 */
static uint32_t
binary_search(btree *tree, btree_node *node, void *key)
{
	uint32_t left = 0;
	uint32_t right = node->num_keys;

	while (left < right)
	{
		uint32_t mid = left + (right - left) / 2;
		void	*mid_key = GET_KEY_AT(node, mid);

		if (type_less_than(tree->node_key_type, mid_key, key))
		{
			left = mid + 1;
		}
		else if (type_equals(tree->node_key_type, mid_key, key))
		{
			if (IS_LEAF(node))
			{
				return mid;
			}
			return mid + 1;
		}
		else
		{
			right = mid;
		}
	}

	return left;
}

/*
 * Navigate from root to the appropriate leaf for a given key.
 *
 * Performs standard B+tree traversal, using binary search at each
 * internal node to determine which child to follow. Since all data
 * is stored in leaves, this always descends to a leaf node.
 *
 */
static btree_node *
find_leaf_for_key(btree *tree, void *key)
{
	btree_node *node = GET_ROOT();

	while (IS_INTERNAL(node))
	{
		uint32_t idx = binary_search(tree, node, key);
		node = GET_CHILD(node, idx);
	}

	return node;
}

/*
 * Update the doubly-linked chain between leaf nodes.
 */
static void
link_leaf_nodes(btree_node *left, btree_node *right)
{
	if (left)
	{
		ENSURE_SAVED(left);
		left->next = right ? right->index : 0;
	}
	if (right)
	{
		ENSURE_SAVED(right);
		right->previous = left ? left->index : 0;
	}
}

/*
 * Remove a leaf node from the doubly-linked chain.
 *
 * When deleting a node, we must maintain chain integrity by connecting
 * its previous and next neighbors directly. This is a no-op for internal
 * nodes since they don't participate in the leaf chain.
 *
 * [a]<->[b]<->[c]
 *     becomes
 *    [a]<->[c]
 *
 */
static void
unlink_leaf_node(btree_node *node)
{
	if (!IS_LEAF(node))
	{
		return;
	}

	btree_node *prev_node = nullptr;
	btree_node *next_node = nullptr;

	if (node->previous != 0)
	{
		prev_node = GET_PREV(node);
	}
	if (node->next != 0)
	{
		next_node = GET_NEXT(node);
	}

	link_leaf_nodes(prev_node, next_node);
}

/*
 * Update a child pointer and maintain parent back-reference.
 */
static void
set_child(btree *tree, btree_node *node, uint32_t child_index, uint32_t node_index)
{
	assert(IS_INTERNAL(node) && "Only internal nodes can have children");

	ENSURE_SAVED(node);
	uint32_t *children = GET_CHILDREN(node);
	children[child_index] = node_index;

	if (node_index != 0)
	{
		btree_node *child_node = GET_NODE(node_index);
		if (child_node)
		{
			ENSURE_SAVED(child_node);
			child_node->parent = node->index;
		}
	}
}

static btree_node *
create_node(btree *tree, bool is_leaf)
{
	uint32_t page_index = pager_new();
	assert(PAGE_INVALID != page_index && "Failed to allocate new page from pager for btree node");
	btree_node *node = GET_NODE(page_index);

	node->index = page_index;
	node->parent = 0;
	node->next = 0;
	node->previous = 0;
	node->num_keys = 0;
	node->is_leaf = is_leaf ? 1 : 0;

	ENSURE_SAVED(node);
	return node;
}

/*
 * Swap the contents of the root node with another node.
 *
 * A b+tree grows upwards, splitting to create a new root. The problem
 * is the master catalog needs to know the root of each b+tree so it
 * can bootstrap. So either we have a mechanism that can detect the root
 * change and update master in the same transaction, or we cheat and
 * swap the contents, and children's parent pointer during a split/merge to ensure
 * that the root is always at the same position it was when the tree was created.
 */
static void
swap_with_root(btree *tree, btree_node *root, btree_node *other)
{
	ENSURE_SAVED(root);
	ENSURE_SAVED(other);
	// Verify root is actually the root
	assert(root->index == tree->root_page_index && "First parameter must be the actual root node for swap");

	// Swap everything EXCEPT the index field
	uint32_t saved_root_index = root->index;
	uint32_t saved_other_index = other->index;

	// Swap all content
	btree_node temp;
	memcpy(&temp, other, sizeof(btree_node));
	memcpy(other, root, sizeof(btree_node));
	memcpy(root, &temp, sizeof(btree_node));

	// Restore the original indices
	root->index = saved_root_index;
	other->index = saved_other_index;

	// Fix parent pointers for root's new children
	root->parent = 0; // Root has no parent
	if (IS_INTERNAL(root))
	{
		uint32_t *children = GET_CHILDREN(root);
		for (uint32_t i = 0; i <= root->num_keys; i++)
		{
			if (children[i])
			{
				btree_node *child = GET_NODE(children[i]);
				ENSURE_SAVED(child);
				child->parent = tree->root_page_index;
			}
		}
	}
	if (IS_INTERNAL(other))
	{
		uint32_t *children = GET_CHILDREN(other);
		for (uint32_t i = 0; i <= other->num_keys; i++)
		{
			if (children[i])
			{
				btree_node *child = GET_NODE(children[i]);
				ENSURE_SAVED(child);
				child->parent = other->index; // ← Update to other's index, not root
			}
		}
	}
}

/*
 * Split a full node into 2 nodes, promoting a key to the parent.
 *
 * This is the core operation for maintaining B+tree balance during insertion.
 * The split creates a new right sibling, redistributes entries between the
 * original (left) and new (right) nodes, and promotes a separator key to
 * the parent.
 *
 * Algorithm:
 *   1. Create new right sibling node
 *   2. Choose split point (typically mid-point for balance)
 *   3. For leaves: Copy right half to new node, promote copy of middle key
 *   4. For internals: Move right half to new node, promote middle key
 *   5. Insert promoted key and new child into parent
 *   6. Handle parent overflow if necessary
 *
 * Special case: Splitting the root
 *   When the root splits, we need a new root above it.
 *
 * Returns: Parent node (which may now be full and need splitting)
 */
static btree_node *
split(btree *tree, btree_node *node)
{
	// === PREPARATION ===
	uint32_t	split_point = GET_SPLIT_INDEX(node);
	btree_node *new_right = create_node(tree, IS_LEAF(node));

	// Save the key that will be promoted to parent
	// (for leaves, this is a copy; for internals, it moves up)
	assert(tree->node_key_size <= 256 && "No handling for keys 256 bytes or greater");
	uint8_t promoted_key[256];

	COPY_KEY(promoted_key, GET_KEY_AT(node, split_point));

	ENSURE_SAVED(node);
	ENSURE_SAVED(new_right);

	btree_node *parent = GET_PARENT(node);
	uint32_t	position_in_parent = 0;

	if (!parent)
	{
		// Special case: splitting root requires creating new root above it
		// 1. Create new node for the internal
		btree_node *new_node = create_node(tree, false);
		// 2. Get current root
		btree_node *root = GET_NODE(tree->root_page_index);
		// 3. Swap contents: root becomes empty, new_node gets old root data
		swap_with_root(tree, root, new_node);
		// 4. Make the old root data (now in new_node) a child of the new root
		set_child(tree, root, 0, new_node->index);

		parent = root;			// New root is the parent
		node = new_node;		// Continue with relocated node
		position_in_parent = 0; // It's child 0 of the new root
	}
	else
	{
		ENSURE_SAVED(parent);
		// Normal case: find our position in existing parent
		position_in_parent = find_child_index(tree, parent, node);

		// Make room for new key and child in parent
		SHIFT_CHILDREN_RIGHT(parent, position_in_parent + 1, parent->num_keys - position_in_parent);
		SHIFT_KEYS_RIGHT(parent, position_in_parent, parent->num_keys - position_in_parent);
	}

	// === INSERT PROMOTED KEY INTO PARENT ===

	COPY_KEY(GET_KEY_AT(parent, position_in_parent), promoted_key);
	set_child(tree, parent, position_in_parent + 1, new_right->index);
	parent->num_keys++;

	// === SPLIT NODE'S DATA ===
	if (IS_LEAF(node))
	{
		// Leaf: Split keys and records evenly
		// Right gets everything from split_point onwards
		new_right->num_keys = node->num_keys - split_point;
		COPY_KEYS(node, split_point, new_right, 0, new_right->num_keys);
		COPY_RECORDS(node, split_point, new_right, 0, new_right->num_keys);

		// Maintain leaf chain
		link_leaf_nodes(new_right, GET_NEXT(node));
		link_leaf_nodes(node, new_right);

		// Left keeps first split_point entries
		node->num_keys = split_point;
	}
	else
	{
		// Internal: The promoted key goes up, doesn't stay in either node
		// Right gets keys after the promoted key
		new_right->num_keys = node->num_keys - split_point - 1;
		COPY_KEYS(node, split_point + 1, new_right, 0, new_right->num_keys);

		// Move children [split_point+1..end] to right node
		uint32_t *left_children = GET_CHILDREN(node);
		for (uint32_t i = 0; i <= new_right->num_keys; i++)
		{
			uint32_t child = left_children[split_point + 1 + i];
			if (child)
			{
				set_child(tree, new_right, i, child);
				left_children[split_point + 1 + i] = 0; // Clear from left
			}
		}

		// Left keeps first split_point keys
		node->num_keys = split_point;
	}

	return parent; // Parent might now be full and need splitting
}

/*
 * Insert a key-record pair into the B+tree.
 *
 * Handles 3 cases:
 *   1. Empty tree: Direct insert into root
 *   2. Space available: Simple insert at correct position
 *   3. Node full: Split and retry
 *
 * After splits, we re-search for the leaf because the key's proper
 * location may have changed during the restructuring.
 */
static void
insert_element(btree *tree, void *key, void *data)
{
	btree_node *root = GET_ROOT();

	if (root->num_keys == 0)
	{
		// Direct insert into empty root
		ENSURE_SAVED(root);
		COPY_KEY(GET_KEY_AT(root, 0), (void *)key);
		COPY_RECORD(GET_RECORD_AT(root, 0), (void *)data);
		root->num_keys = 1;
		return;
	}

	// Find leaf
	btree_node *leaf = find_leaf_for_key(tree, (void *)key);

	// Make room if needed
	if (NODE_IS_FULL(leaf))
	{

		btree_node *node = leaf;
		while (node && NODE_IS_FULL(node))
		{
			// split might propagate to parent,
			// stop when split doesn't return new parent of split node
			// and research

			node = split(tree, node);
		}

		// Re-find because splits may have moved our key
		leaf = find_leaf_for_key(tree, (void *)key);
	}

	// Now insert
	uint32_t pos = binary_search(tree, leaf, (void *)key);
	ENSURE_SAVED(leaf);

	SHIFT_KEYS_RIGHT(leaf, pos, leaf->num_keys - pos);
	SHIFT_RECORDS_RIGHT(leaf, pos, leaf->num_keys - pos);

	COPY_KEY(GET_KEY_AT(leaf, pos), (void *)key);
	COPY_RECORD(GET_RECORD_AT(leaf, pos), (void *)data);
	leaf->num_keys++;
}

static void
destroy_node(btree_node *node)
{

	unlink_leaf_node(node); // no-op if internal
	pager_delete(node->index);
}
/*
 * Handle the special case where the root becomes empty after deletion.
 *
 * This only occurs for internal roots with a single child remaining.
 * Rather than having an empty root pointing to 1 child, we promote
 * the child to become the new root, reducing tree height by one.
 */
static void
collapse_empty_root(btree *tree, btree_node *root)
{
	assert(root->num_keys == 0 && "collapse_empty_root called on non-empty root");
	assert(IS_INTERNAL(root) && "collapse_empty_root requires internal node (leaf roots cannot be collapsed)");

	// The root has only 1 child - make it the new root
	btree_node *only_child = GET_CHILD(root, 0);

	// Swap contents: only_child becomes root, root gets child's data
	swap_with_root(tree, root, only_child);

	// Delete the old root (now at only_child's position)
	destroy_node(only_child);
}

/*
 * Borrow an entry from the left sibling to fix underflow.
 *
 * For leaf nodes:
 *    Move rightmost entry from left sibling to leftmost of current node
 *    Update parent separator to reflect new boundary
 *
 * For internal nodes:
 *    Rotate through parent: parent separator moves down, left's last key moves up
 *    Transfer corresponding child pointer
 *
 */
static void
borrow_from_left_sibling(btree *tree, btree_node *node, btree_node *left_sibling, uint32_t separator_index)
{
	btree_node *parent = GET_PARENT(node);

	ENSURE_SAVED(node);
	ENSURE_SAVED(left_sibling);
	ENSURE_SAVED(parent);

	// Make room at the beginning of node
	SHIFT_KEYS_RIGHT(node, 0, node->num_keys);

	if (IS_LEAF(node))
	{
		// For leaves: move last entry from left to first of node
		SHIFT_RECORDS_RIGHT(node, 0, node->num_keys);

		// Copy entry from left's end to node's beginning
		COPY_KEY(GET_KEY_AT(node, 0), GET_KEY_AT(left_sibling, left_sibling->num_keys - 1));
		COPY_RECORD(GET_RECORD_AT(node, 0), GET_RECORD_AT(left_sibling, left_sibling->num_keys - 1));

		// Update parent separator to be the new first key of node
		COPY_KEY(GET_KEY_AT(parent, separator_index), GET_KEY_AT(node, 0));
	}
	else
	{
		// For internals: rotate through parent
		// Parent separator moves down to node
		COPY_KEY(GET_KEY_AT(node, 0), GET_KEY_AT(parent, separator_index));

		// Left's last key moves up to parent
		COPY_KEY(GET_KEY_AT(parent, separator_index), GET_KEY_AT(left_sibling, left_sibling->num_keys - 1));

		// Move corresponding child pointer
		uint32_t *node_children = GET_CHILDREN(node);
		uint32_t *left_children = GET_CHILDREN(left_sibling);

		// Shift node's children right and add left's last child as first
		for (uint32_t i = node->num_keys + 1; i > 0; i--)
		{
			set_child(tree, node, i, node_children[i - 1]);
		}
		set_child(tree, node, 0, left_children[left_sibling->num_keys]);
	}

	node->num_keys++;
	left_sibling->num_keys--;
}

/*
 * Borrow an entry from the right sibling to fix underflow.
 *
 * Mirrors borrow_from_left_sibling, moving entries
 * in the opposite direction.
 */
static void
borrow_from_right_sibling(btree *tree, btree_node *node, btree_node *right_sibling, uint32_t separator_index)
{
	btree_node *parent = GET_PARENT(node);

	ENSURE_SAVED(node);
	ENSURE_SAVED(right_sibling);
	ENSURE_SAVED(parent);

	if (IS_LEAF(node))
	{
		// For leaves: move first entry from right to end of node
		COPY_KEY(GET_KEY_AT(node, node->num_keys), GET_KEY_AT(right_sibling, 0));
		COPY_RECORD(GET_RECORD_AT(node, node->num_keys), GET_RECORD_AT(right_sibling, 0));

		// Shift right sibling's entries left
		SHIFT_KEYS_LEFT(right_sibling, 0, right_sibling->num_keys - 1);
		SHIFT_RECORDS_LEFT(right_sibling, 0, right_sibling->num_keys - 1);

		// Update parent separator to be the new first key of right
		COPY_KEY(GET_KEY_AT(parent, separator_index), GET_KEY_AT(right_sibling, 0));
	}
	else
	{
		// For internals: rotate through parent
		// Parent separator moves down to node
		COPY_KEY(GET_KEY_AT(node, node->num_keys), GET_KEY_AT(parent, separator_index));

		// Right's first key moves up to parent
		COPY_KEY(GET_KEY_AT(parent, separator_index), GET_KEY_AT(right_sibling, 0));

		// Move corresponding child pointer
		uint32_t *right_children = GET_CHILDREN(right_sibling);
		set_child(tree, node, node->num_keys + 1, right_children[0]);

		// Shift right's keys and children left
		SHIFT_KEYS_LEFT(right_sibling, 0, right_sibling->num_keys - 1);
		for (uint32_t i = 0; i < right_sibling->num_keys; i++)
		{
			set_child(tree, right_sibling, i, right_children[i + 1]);
		}
	}

	node->num_keys++;
	right_sibling->num_keys--;
}

/*
 * Attempt to borrow from either sibling to fix underflow.
 *
 * Tries left sibling first for consistency. Borrowing is preferred
 * over merging because it's non-destructive and maintains the same
 * number of nodes in the tree.
 *
 * Returns: true if borrowing succeeded, false if both siblings are minimal leading to a merge
 */
static bool
try_borrow_from_siblings(btree *tree, btree_node *node)
{
	btree_node *parent = GET_PARENT(node);
	uint32_t	child_index = find_child_index(tree, parent, node);

	// Try left sibling first (consistent strategy)
	if (child_index > 0)
	{
		btree_node *left = GET_CHILD(parent, child_index - 1);
		if (NODE_CAN_SPARE(left))
		{
			borrow_from_left_sibling(tree, node, left, child_index - 1);
			return true;
		}
	}

	// Try right sibling
	if (child_index < parent->num_keys)
	{
		btree_node *right = GET_CHILD(parent, child_index + 1);
		if (NODE_CAN_SPARE(right))
		{
			borrow_from_right_sibling(tree, node, right, child_index);
			return true;
		}
	}

	return false;
}

/*
 * Merge an underflowing node with a sibling.
 *
 * When borrowing isn't possible (both siblings are minimal), we must
 * merge the underflowing node with a sibling. This reduces the number
 * of nodes and may cause the parent to underflow.
 *
 * For leaves: Concatenate all entries into the left node
 * For internals: Pull down separator from parent and concatenate
 *
 * Returns: Parent node (which may now underflow and need repair)
 */
static btree_node *
perform_merge_with_sibling(btree *tree, btree_node *node)
{
	btree_node *parent = GET_PARENT(node);
	uint32_t	child_index = find_child_index(tree, parent, node);

	btree_node *left, *right;
	uint32_t	separator_index;

	// Decide which sibling to merge with
	// Prefer merging with right sibling (consistent strategy)
	if (child_index < parent->num_keys)
	{
		// Merge with right sibling
		left = node;
		right = GET_CHILD(parent, child_index + 1);
		separator_index = child_index;
	}
	else
	{
		assert(child_index > 0 && "Rightmost child must have a left sibling to merge with");
		// We're the rightmost child, merge with left sibling
		left = GET_CHILD(parent, child_index - 1);
		right = node;
		separator_index = child_index - 1;
	}

	assert(left->index == GET_CHILDREN(parent)[separator_index] &&
		   "Left node must be at separator_index position in parent");
	assert(right->index == GET_CHILDREN(parent)[separator_index + 1] &&
		   "Right node must be at separator_index+1 position in parent");

	ENSURE_SAVED(left);
	ENSURE_SAVED(parent);

	if (IS_LEAF(left))
	{
		// For leaves: concatenate all entries
		COPY_KEYS(right, 0, left, left->num_keys, right->num_keys);
		COPY_RECORDS(right, 0, left, left->num_keys, right->num_keys);
		left->num_keys += right->num_keys;

		// Update leaf chain
		link_leaf_nodes(left, GET_NEXT(right));
	}
	else
	{
		// For internals: bring down separator and concatenate
		// Copy separator from parent into left
		COPY_KEY(GET_KEY_AT(left, left->num_keys), GET_KEY_AT(parent, separator_index));

		// Copy all keys from right
		COPY_KEYS(right, 0, left, left->num_keys + 1, right->num_keys);

		// Move all children from right
		uint32_t *right_children = GET_CHILDREN(right);
		for (uint32_t i = 0; i <= right->num_keys; i++)
		{
			set_child(tree, left, left->num_keys + 1 + i, right_children[i]);
		}

		left->num_keys += 1 + right->num_keys;
	}

	// Remove separator and right child from parent
	SHIFT_KEYS_LEFT(parent, separator_index, parent->num_keys - separator_index - 1);
	SHIFT_CHILDREN_LEFT(parent, separator_index + 1, parent->num_keys - separator_index - 1);
	parent->num_keys--;

	// Delete the now-empty right node
	destroy_node(right);

	return parent;
}

/*
 * Fix an underflowing node after deletion.
 *
 * Try borrowing from siblings (non-destructive)
 * If that fails, merge with a sibling (destructive)
 *
 * The repair may cascade up the tree if merging causes the parent
 * to underflow. The root is special-cased: it can have fewer than
 * the minimum keys, but if it becomes completely empty (internal
 * with no keys), we collapse it.
 */
static void
repair_underflow(btree *tree, btree_node *node)
{
	// Step 1: Check if repair is needed
	if (!IS_UNDERFLOWING(node))
	{
		return;
	}

	// Root is allowed to have fewer keys
	if (IS_ROOT(node))
	{
		return;
	}

	// Step 2: Try non-destructive fix (borrow from sibling)
	if (try_borrow_from_siblings(tree, node))
	{
		return;
	}

	// Step 3: Destructive fix (merge with sibling)
	btree_node *parent = perform_merge_with_sibling(tree, node);

	// Step 4: Check if parent needs repair (cascade)
	if (parent && IS_UNDERFLOWING(parent))
	{
		if (IS_ROOT(parent) && parent->num_keys == 0)
		{
			collapse_empty_root(tree, parent);
		}
		else
		{
			repair_underflow(tree, parent); // Recursive
		}
	}
}

/*
 * Delete an entry from a leaf node.
 *
 * Removes the key-record pair at the specified index and handles
 * any resulting underflow. The deletion only occurs in leaf nodes
 * since B+trees store all data in leaves.
 *
 * B+trees can leave deleted keys in internal nodes as they just act as guides to
 * the leaves.
 */
static void
delete_element(btree *tree, btree_node *node, void *key, uint32_t index)
{
	assert(IS_LEAF(node) && "B+tree deletion only occurs in leaf nodes where data is stored");

	if (IS_ROOT(node) && node->num_keys == 1)
	{
		ENSURE_SAVED(node);
		node->num_keys = 0;
		return;
	}

	ENSURE_SAVED(node);

	// Remove the entry by shifting remaining entries left
	uint32_t entries_to_shift = node->num_keys - index - 1;
	SHIFT_KEYS_LEFT(node, index, entries_to_shift);
	SHIFT_RECORDS_LEFT(node, index, entries_to_shift);
	node->num_keys--;

	// Fix underflow if necessary
	repair_underflow(tree, node);
}
/*
 * Recursively delete all nodes in the tree.
 */
void
clear_recurse(btree *tree, btree_node *node)
{
	if (IS_LEAF(node))
	{
		pager_delete(node->index);
		return;
	}

	uint32_t	i = 0;
	btree_node *child = GET_CHILD(node, i);
	while (child != nullptr)
	{
		clear_recurse(tree, child);
		child = GET_CHILD(node, ++i);
	}

	pager_delete(node->index);
}

/*
 * Post order dfs calling pager_delete to wipe entire tree
 */
bool
bt_clear(btree *tree)
{
	if (0 == tree->root_page_index)
	{
		// uninitialised table
		return true;
	}

	clear_recurse(tree, GET_NODE(tree->root_page_index));
	return true;
}

/*
 * Calculates node capacities based on key and record sizes
 *
 * The calculations account for:
 *    Node header overhead (24 bytes)
 *    Different layouts for leaf vs internal nodes
 *    Minimum entry count for proper splits/merges
 *
 *
 * A simplification of the implementation is that we hardcode the space a record can occupy from the types.
 * So if we have a record [u32, char16], and we populate it with: [23, "markymark"]
 * in memory/disk that will be [u32 data, "markymark0000000"] such that there is wasted
 * space. A real database would have additional metadata in the header do pack and unpack rows
 * as tightly as possible.
 */
btree
bt_create(data_type key, uint32_t record_size, bool allocate_node = false)
{
	btree tree = {0};

	tree.node_key_type = key;
	tree.node_key_size = type_size(key);

	tree.record_size = record_size;

	constexpr uint32_t USABLE_SPACE = PAGE_SIZE - NODE_HEADER_SIZE;

	assert(record_size * MIN_ENTRY_COUNT <= USABLE_SPACE && "Record to large, btree node must be able to fit at least "
															"3 records for merge and split operations to function");

	uint32_t leaf_entry_size = tree.node_key_size + record_size;
	uint32_t leaf_max_entries = USABLE_SPACE / leaf_entry_size;

	tree.leaf_max_keys = MIN_ENTRY_COUNT > leaf_max_entries ? MIN_ENTRY_COUNT : leaf_max_entries;
	tree.leaf_min_keys = tree.leaf_max_keys / 2;
	tree.leaf_split_index = tree.leaf_max_keys / 2;

	uint32_t child_ptr_size = sizeof(uint32_t);

	uint32_t internal_max_entries = (USABLE_SPACE - child_ptr_size) / (tree.node_key_size + child_ptr_size);

	// in split, there is one rising key, we need to ensure an even split
	if (internal_max_entries % 2 == 0)
	{
		tree.internal_min_keys = (internal_max_entries / 2) - 1;
	}
	else
	{
		tree.internal_min_keys = (internal_max_entries) / 2;
	}

	tree.internal_max_keys = MIN_ENTRY_COUNT > internal_max_entries ? MIN_ENTRY_COUNT : internal_max_entries;
	tree.internal_split_index = tree.internal_max_keys / 2;

	if (allocate_node)
	{
		btree_node *root = create_node(&tree, true);
		tree.root_page_index = root->index;
	}
	return tree;
}

/*
 * B+tree Cursor
 *
 * The cursor provides a stateful position within the B+tree, enabling
 * iteration and positioned updates. Cursors operate exclusively
 * at the leaf level since all data resides in leaf nodes.
 *
 *
 * Cursor state consists of:
 *    leaf_page: Current leaf node's page index
 *    leaf_index: Position within that leaf (0 to num_keys-1)
 *    state: Valid or invalid flag
 *
 * In the same way that the text cursor can 'traverse' a text file moving
 * forward, back, to the beginning/end (if you use vim ...) and can alter a character
 * that it is positioned over, so to can we have a cursor that moves over a more complex
 * data structure.
 *
 */

static void
cursor_clear(bt_cursor *cursor)
{
	cursor->leaf_page = 0;
	cursor->leaf_index = 0;
	cursor->state = BT_CURSOR_INVALID;
}

static bool
cursor_move_end(bt_cursor *cursor, bool left)
{

	btree *tree = cursor->tree;
	cursor_clear(cursor);

	btree_node *root = GET_ROOT();
	if (!root || root->num_keys == 0)
	{
		cursor->state = BT_CURSOR_INVALID;
		return false;
	}

	btree_node *current = root;

	while (!IS_LEAF(current))
	{
		if (left)
		{
			current = GET_CHILD(current, 0);
		}
		else
		{
			uint32_t child_pos = current->num_keys;

			current = GET_CHILD(current, child_pos);
			assert(current);
		}
	}

	if (left)
	{
		cursor->leaf_page = current->index;
		cursor->leaf_index = 0;
	}
	else
	{
		cursor->leaf_page = current->index;
		cursor->leaf_index = current->num_keys - 1;
	}
	cursor->state = BT_CURSOR_VALID;
	return true;
}

/*
 * Position cursor based on key and comparison operator.
 *
 * For range operations (non-EQ), the cursor iterates from the initial
 * position until finding a key satisfying the condition.
 *
 * Returns: true if a matching key was found, false otherwise
 */
bool
bt_cursor_seek(bt_cursor *cursor, void *key, COMPARISON_OP op)
{
	btree *tree = cursor->tree;
	cursor_clear(cursor);

	if (!cursor->tree->root_page_index)
	{
		cursor->state = BT_CURSOR_INVALID;
		return false;
	}

	btree_node *leaf = find_leaf_for_key(cursor->tree, key);
	uint32_t	index = binary_search(cursor->tree, leaf, key);

	cursor->leaf_page = leaf->index;

	bool exact = index < leaf->num_keys && type_equals(cursor->tree->node_key_type, GET_KEY_AT(leaf, index), key);

	// Handle EQ case immediately
	if (op == EQ)
	{
		if (exact)
		{
			cursor->leaf_index = index;
			cursor->state = BT_CURSOR_VALID;
			return true;
		}
		cursor->state = BT_CURSOR_INVALID;
		return false;
	}

	if (index >= leaf->num_keys)
	{
		cursor->leaf_index = leaf->num_keys - 1;
	}
	else
	{
		cursor->leaf_index = index;
	}
	cursor->state = BT_CURSOR_VALID;

	bool exact_match_ok = (op == GE || op == LE);
	if (exact && exact_match_ok)
	{
		return true;
	}

	bool forward = (op == GE || op == GT);

	do
	{
		void *current_key = bt_cursor_key(cursor);
		if (!current_key)
		{
			continue;
		}

		bool satisfied = (op == GE && type_greater_equal(cursor->tree->node_key_type, current_key, key)) ||
						 (op == GT && type_greater_than(cursor->tree->node_key_type, current_key, key)) ||
						 (op == LE && type_less_equal(cursor->tree->node_key_type, current_key, key)) ||
						 (op == LT && type_less_than(cursor->tree->node_key_type, current_key, key));

		if (satisfied)
		{
			return true;
		}

	} while (forward ? bt_cursor_next(cursor) : bt_cursor_previous(cursor));

	cursor->state = BT_CURSOR_INVALID;
	return false;
}

bool
bt_cursoris_valid(bt_cursor *cursor)
{
	return cursor->state == BT_CURSOR_VALID;
}

void *
bt_cursor_key(bt_cursor *cursor)
{

	btree *tree = cursor->tree;
	if (cursor->state != BT_CURSOR_VALID)
	{
		return nullptr;
	}

	btree_node *node = GET_NODE(cursor->leaf_page);
	if (!node || cursor->leaf_index >= node->num_keys)
	{
		return nullptr;
	}

	return GET_KEY_AT(node, cursor->leaf_index);
}

void *
bt_cursor_record(bt_cursor *cursor)
{

	btree *tree = cursor->tree;
	if (cursor->state != BT_CURSOR_VALID)
	{
		return nullptr;
	}

	btree_node *node = GET_NODE(cursor->leaf_page);
	if (!node || cursor->leaf_index >= node->num_keys)
	{
		return nullptr;
	}

	return GET_RECORD_AT(node, cursor->leaf_index);
}

/*
 * Delete entry at current cursor position.
 *
 * After deletion, cursor moves to the previous entry in the node
 * if at the last position, or stays at the same index (which now
 * contains the next entry due to shifting).
 */
bool
bt_cursor_delete(bt_cursor *cursor)
{
	if (cursor->state != BT_CURSOR_VALID)
	{
		return false;
	}

	void *key = bt_cursor_key(cursor);
	if (!key)
	{
		return false;
	}

	auto	 node = GET_NODE(cursor->leaf_page);
	uint32_t index = node->index;

	delete_element(cursor->tree, node, key, cursor->leaf_index);

	node = GET_NODE(cursor->leaf_page);
	if (!node)
	{
		return true;
	}

	if (cursor->leaf_index >= node->num_keys)
	{
		if (node->num_keys > 0)
		{
			cursor->leaf_index = node->num_keys - 1;
		}
		else
		{
			cursor->state = BT_CURSOR_INVALID;
		}
	}

	return true;
}

/*
 * Insert a new key-record pair.
 *
 * Returns false if key already exists (no duplicates allowed).
 * Cursor position becomes undefined after insertion.
 */
bool
bt_cursor_insert(bt_cursor *cursor, void *key, void *record)
{

	if (bt_cursor_seek(cursor, key))
	{
		return false;
	}

	insert_element(cursor->tree, key, record);
	return true;
}

/*
 * Update record at current cursor position.
 *
 * Only modifies the record data, not the key. The cursor
 * position remains valid after update.
 */
bool
bt_cursor_update(bt_cursor *cursor, void *record)
{
	if (cursor->state != BT_CURSOR_VALID)
	{
		return false;
	}
	pager_ensure_journaled(cursor->leaf_page);
	void *data = bt_cursor_record(cursor);
	memcpy(data, record, cursor->tree->record_size);
	return true;
}

/*
 * Move cursor to first entry in tree.
 */
bool
bt_cursor_first(bt_cursor *cursor)
{
	return cursor_move_end(cursor, true);
}

/*
 * Move cursor to last entry in tree.
 */
bool
bt_cursor_last(bt_cursor *cursor)
{
	return cursor_move_end(cursor, false);
}

/*
 * Advance cursor to next entry.
 */
bool
bt_cursor_next(bt_cursor *cursor)
{
	if (cursor->state != BT_CURSOR_VALID)
	{
		return false;
	}

	btree_node *node = GET_NODE(cursor->leaf_page);
	if (!node)
	{
		cursor->state = BT_CURSOR_INVALID;
		return false;
	}

	cursor->leaf_index++;

	if (cursor->leaf_index >= node->num_keys)
	{
		if (node->next != 0)
		{
			btree_node *next_node = GET_NEXT(node);
			if (next_node && next_node->num_keys > 0)
			{
				cursor->leaf_page = next_node->index;
				cursor->leaf_index = 0;
				return true;
			}
		}

		return false;
	}
	return true;
}

/*
 * Move cursor to previous entry.
 */
bool
bt_cursor_previous(bt_cursor *cursor)
{
	if (cursor->state != BT_CURSOR_VALID)
	{
		return false;
	}

	btree_node *node = GET_NODE(cursor->leaf_page);
	if (!node)
	{
		cursor->state = BT_CURSOR_INVALID;
		return false;
	}

	if (cursor->leaf_index > 0)
	{
		cursor->leaf_index--;
		return true;
	}

	if (node->previous != 0)
	{
		btree_node *prev_node = GET_PREV(node);
		if (prev_node && prev_node->num_keys > 0)
		{
			cursor->leaf_page = prev_node->index;
			cursor->leaf_index = prev_node->num_keys - 1;
			return true;
		}
	}

	return false;
}

bool
bt_cursorhas_next(bt_cursor *cursor)
{
	if (bt_cursor_next(cursor))
	{
		bt_cursor_previous(cursor);
		return true;
	}
	return false;
}

bool
bt_cursorhas_previous(bt_cursor *cursor)
{
	if (bt_cursor_previous(cursor))
	{
		bt_cursor_next(cursor);
		return true;
	}
	return false;
}

/*NOCOVER_START*/

#define ASSERT_PRINT(condition, tree)                                                                                  \
	if (!(condition))                                                                                                  \
	{                                                                                                                  \
		bt_print(tree);                                                                                                \
		assert(condition);                                                                                             \
	}

struct validation_result
{
	uint32_t	depth;
	uint8_t	   *min_key;
	uint8_t	   *max_key;
	btree_node *leftmost_leaf;
	btree_node *rightmost_leaf;
};

static validation_result
validate_node_recursive(btree *tree, btree_node *node, uint32_t expected_parent, void *parent_min_bound,
						void *parent_max_bound, hash_set<uint32_t> &visited);

void
bt_validate(btree *tree)
{

	ASSERT_PRINT(tree != nullptr, tree);

	if (tree->root_page_index == 0)
	{
		return;
	}

	btree_node *root = GET_ROOT();
	ASSERT_PRINT(root != nullptr, tree);

	ASSERT_PRINT(IS_ROOT(root), tree);
	ASSERT_PRINT(root->index == tree->root_page_index, tree);

	hash_set<uint32_t> visited;

	validation_result result = validate_node_recursive(tree, root, 0, nullptr, nullptr, visited);

	if (IS_LEAF(root) && root->num_keys > 0)
	{

		ASSERT_PRINT(root->next == 0, tree);
		ASSERT_PRINT(root->previous == 0, tree);
	}
	else if (IS_INTERNAL(root))
	{

		btree_node		  *first_leaf = result.leftmost_leaf;
		btree_node		  *current = first_leaf;
		hash_set<uint32_t> leaf_visited;

		ASSERT_PRINT(current->previous == 0, tree);

		while (current)
		{
			ASSERT_PRINT(IS_LEAF(current), tree);
			ASSERT_PRINT(!leaf_visited.contains(current->index), tree);
			leaf_visited.insert(current->index, 1);

			if (current->next != 0)
			{
				btree_node *next = GET_NEXT(current);
				ASSERT_PRINT(next != nullptr, tree);
				ASSERT_PRINT(next->previous == current->index, tree);
				current = next;
			}
			else
			{
				ASSERT_PRINT(current == result.rightmost_leaf, tree);
				break;
			}
		}
	}
}
static validation_result
validate_node_recursive(btree *tree, btree_node *node, uint32_t expected_parent, void *parent_min_bound,
						void *parent_max_bound, hash_set<uint32_t> &visited)
{
	ASSERT_PRINT(node != nullptr, tree);

	ASSERT_PRINT(!visited.contains(node->index), tree);
	visited.insert(node->index, 1);

	ASSERT_PRINT(node->parent == expected_parent, tree);

	uint32_t max_keys = GET_MAX_KEYS(node);
	uint32_t min_keys = GET_MIN_KEYS(node);

	ASSERT_PRINT(node->num_keys <= max_keys, tree);

	if (expected_parent != 0)
	{
		ASSERT_PRINT(node->num_keys >= min_keys, tree);
	}
	else
	{

		if (node->num_keys == 0)
		{
			ASSERT_PRINT(IS_LEAF(node), tree);
		}
	}

	void *prev_key = nullptr;
	void *first_key = nullptr;
	void *last_key = nullptr;

	for (uint32_t i = 0; i < node->num_keys; i++)
	{
		void *current_key = GET_KEY_AT(node, i);

		if (i == 0)
		{
			first_key = current_key;
		}
		if (i == node->num_keys - 1)
		{
			last_key = current_key;
		}

		if (prev_key)
		{
			ASSERT_PRINT(type_less_than(tree->node_key_type, prev_key, current_key), tree);
		}

		if (parent_min_bound)
		{
			ASSERT_PRINT(type_greater_equal(tree->node_key_type, current_key, parent_min_bound), tree);
		}
		if (parent_max_bound)
		{
			ASSERT_PRINT(type_less_than(tree->node_key_type, current_key, parent_max_bound), tree);
		}
		prev_key = current_key;
	}

	validation_result result;
	result.min_key = (uint8_t *)first_key;
	result.max_key = (uint8_t *)last_key;

	if (IS_LEAF(node))
	{
		result.depth = 0;
		result.leftmost_leaf = node;
		result.rightmost_leaf = node;

		void *records = GET_RECORD_DATA(node);
		ASSERT_PRINT(records != nullptr, tree);

		if (node->next != 0)
		{
			ASSERT_PRINT(node->next != node->index, tree);
			btree_node *next = GET_NEXT(node);
			ASSERT_PRINT(next != nullptr, tree);
			ASSERT_PRINT(IS_LEAF(next), tree);
		}
		if (node->previous != 0)
		{
			ASSERT_PRINT(node->previous != node->index, tree);
			btree_node *prev = GET_PREV(node);
			ASSERT_PRINT(prev != nullptr, tree);
			ASSERT_PRINT(IS_LEAF(prev), tree);
		}
	}
	else
	{

		uint32_t *children = GET_CHILDREN(node);
		ASSERT_PRINT(children != nullptr, tree);

		uint32_t	child_depth = UINT32_MAX;
		btree_node *leftmost_leaf = nullptr;
		btree_node *rightmost_leaf = nullptr;

		for (uint32_t i = 0; i <= node->num_keys; i++)
		{
			ASSERT_PRINT(children[i] != 0, tree);
			ASSERT_PRINT(children[i] != node->index, tree);

			btree_node *child = GET_CHILD(node, i);
			ASSERT_PRINT(child != nullptr, tree);

			void *child_min = (i == 0) ? parent_min_bound : GET_KEY_AT(node, i - 1);
			void *child_max = (i == node->num_keys) ? parent_max_bound : GET_KEY_AT(node, i);

			validation_result child_result =
				validate_node_recursive(tree, child, node->index, child_min, child_max, visited);

			if (child_depth == UINT32_MAX)
			{
				child_depth = child_result.depth;
				leftmost_leaf = child_result.leftmost_leaf;
			}
			else
			{
				ASSERT_PRINT(child_depth == child_result.depth, tree);
			}

			rightmost_leaf = child_result.rightmost_leaf;

			if (child_result.min_key && i > 0)
			{

				void *separator = GET_KEY_AT(node, i - 1);
				ASSERT_PRINT(type_greater_equal(tree->node_key_type, child_result.min_key, separator), tree);
			}
			if (child_result.max_key && i < node->num_keys)
			{

				void *separator = GET_KEY_AT(node, i);
				ASSERT_PRINT(type_less_equal(tree->node_key_type, child_result.max_key, separator), tree);
			}
		}

		result.depth = child_depth + 1;
		result.leftmost_leaf = leftmost_leaf;
		result.rightmost_leaf = rightmost_leaf;

		ASSERT_PRINT(node->next == 0, tree);
		ASSERT_PRINT(node->previous == 0, tree);
	}

	return result;
}
static void
print_key(btree *tree, void *key)
{
	if (!key)
	{
		printf("NULL");
		return;
	}
	// Use columns[0] type if schema provided, otherwise use tree's key type
	data_type key_type =

		 tree->node_key_type;
	type_print(key_type, key);
}

static void
print_record_columns(const uint8_t *record_data,
                    array<data_type, query_arena> *columns,
                    int start_col)
{
	printf("{");
	uint32_t offset = 0;

	for (int i = start_col; i < columns->size(); i++) {
		if (i > start_col) printf(", ");

		data_type col_type = (*columns)[i];
		type_print(col_type, record_data + offset);
		offset += type_size(col_type);
	}
	printf("}");
}

void
bt_print(btree *tree, array<data_type, query_arena> *columns)
{
	if (!tree || tree->root_page_index == 0)
	{
		printf("B+Tree: EMPTY\n");
		return;
	}
	printf("====================================\n");
	printf("B+Tree Structure\n");
	printf("====================================\n");
	printf("Root: page_%u\n", tree->root_page_index);
	printf("Key type: %s, Record size: %u bytes\n", type_name(tree->node_key_type), tree->record_size);

	// Show schema if provided
	if (columns && columns->size() > 0) {
		printf("Schema: [");
		for (int i = 0; i < columns->size(); i++) {
			if (i > 0) printf(", ");
			printf("%s", type_name((*columns)[i]));
		}
		printf("]\n");
	}

	printf("Internal: max_keys=%u, min_keys=%u\n", tree->internal_max_keys, tree->internal_min_keys);
	printf("Leaf: max_keys=%u, min_keys=%u\n", tree->leaf_max_keys, tree->leaf_min_keys);
	printf("------------------------------------\n\n");

	queue<uint32_t, query_arena> current_level;
	queue<uint32_t, query_arena> next_level;
	current_level.push(tree->root_page_index);
	uint32_t depth = 0;

	while (!current_level.empty())
	{
		printf("LEVEL %u:\n", depth);
		printf("--------\n");
		while (!current_level.empty())
		{
			uint32_t page_index = *current_level.front();
			current_level.pop();
			btree_node *node = GET_NODE(page_index);
			if (!node)
			{
				printf("  ERROR: Cannot read page %u\n", page_index);
				continue;
			}
			printf("  Node[page_%u]:\n", node->index);
			printf("    Type: %s\n", IS_LEAF(node) ? "LEAF" : "INTERNAL");
			printf("    Parent: %s\n", IS_ROOT(node) ? "ROOT" : ("page_" + std::to_string(node->parent)).c_str());
			printf("    Keys(%u): [", node->num_keys);

			for (uint32_t i = 0; i < node->num_keys; i++)
			{
				if (i > 0)
					printf(", ");
				print_key(tree, GET_KEY_AT(node, i));
			}
			printf("]\n");

			// For leaf nodes with schema, show records
			if (IS_LEAF(node) && tree->record_size > 0 && columns && columns->size() > 1)
			{
				printf("    Records:\n");
				for (uint32_t i = 0; i < node->num_keys; i++)
				{
					printf("      ");
					print_key(tree, GET_KEY_AT(node, i));
					printf(" => ");
					print_record_columns(GET_RECORD_AT(node, i), columns, 1);
					printf("\n");
				}
			}

			if (IS_INTERNAL(node))
			{
				uint32_t *children = GET_CHILDREN(node);
				printf("    Children(%u): [", node->num_keys + 1);
				for (uint32_t i = 0; i <= node->num_keys; i++)
				{
					if (i > 0)
						printf(", ");
					printf("page_%u", children[i]);
					next_level.push(children[i]);
				}
				printf("]\n");
			}
			else
			{
				printf("    Leaf chain: ");
				if (node->previous != 0)
				{
					printf("prev=page_%u", node->previous);
				}
				else
				{
					printf("prev=NULL");
				}
				printf(", ");
				if (node->next != 0)
				{
					printf("next=page_%u", node->next);
				}
				else
				{
					printf("next=NULL");
				}
				printf("\n");
			}
			printf("\n");
		}
		if (!next_level.empty())
		{
			std::swap(current_level, next_level);
			depth++;
		}
	}

	printf("====================================\n");
	printf("Leaf Chain Traversal:\n");
	printf("------------------------------------\n");

	btree_node *current = GET_ROOT();
	while (IS_INTERNAL(current))
	{
		current = GET_CHILD(current, 0);
		if (!current)
		{
			printf("ERROR: Cannot find leftmost leaf\n");
			return;
		}
	}

	printf("  ");
	uint32_t leaf_count = 0;
	while (current)
	{
		if (leaf_count > 0)
			printf(" -> ");
		printf("page_%u", current->index);
		if (++leaf_count > 1000)
		{
			printf("\n  ERROR: Possible cycle detected in leaf chain!\n");
			break;
		}
		current = GET_NEXT(current);
	}
	printf("\n");
	printf("  Total leaves: %u\n", leaf_count);
	printf("====================================\n\n");
}
