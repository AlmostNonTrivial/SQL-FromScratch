/*
 * SQL From Scratch - Educational Database Engine
 *
 * Ephemeral Tree
 *
 * A Red-Black (balanced binary) tree data that can
 * be used as ephemeral storage by the VM during the execution of a program.
 * All nodes are allocated in the query_arena. The tree is 'ephemeral' because it doesn't persist between queries,
 * as like other query_arena allocations they are deallocated in bulk after the execution
 *
 * For visualizing the Red-Black tree algorithm see https://www.cs.usfca.edu/~galles/visualization/RedBlack.html
 *
 * Node structure and data are allocated in a single block: [node_struct][key_bytes][record_bytes]
 * Like the b+tree, access to a part of a record involves pointer arithmetic, but unlike the b+tree
 * it's 1 key-record pair per node.
 *
 * Having in-memory sorted storage allows to do things like:
 * - Aggregations: Store group keys with running aggregates
 * - Sorting: Insert all rows, then iterate in order
 * - DISTINCT: Store seen values, checking for duplicates
 * - Subquery results: Temporary storage for IN/EXISTS operations
 *
 * EXAMPLE 1: GROUP BY city, COUNT(*)
 * SQL: SELECT city, COUNT(*) FROM users GROUP BY city;
 *
 *     Red-Black Tree (no duplicates)           Memory Layout per Node:
 *           [London]-B                          [node_struct(24B)][key:char32][record:u32]
 *              / \
 *      [Boston]-R [Paris]-R                     e.g., [node_ptr,color,...]["London\0..."][42]
 *           /       \                                                            ↑           ↑
 *    [Austin]-B  [Tokyo]-B                                                    city       count
 *
 *     Each insertion either:
 *     - Adds new city with count=1
 *     - Finds existing city and increments count in-place
 *
 * EXAMPLE 2: ORDER BY age
 * SQL: SELECT * FROM users ORDER BY age;
 *
 *     Red-Black Tree (duplicates allowed)      Node Layout:
 *           [25]-B                              [node_struct][age:u32][username,email,...]
 *            /    \
 *     [23]-B       [28]-B                       Multiple records with age=25 would create
 *         /       /    \                        separate nodes, maintaining all data
 *   [22]-R   [25]-R   [30]-R
 *
 *     Key: age (u32)
 *     Record: [username(char16), email(char32), city(char32), ...]
 */

#include "ephemeral.hpp"
#include <cstdint>
#include "arena.hpp"
#include "common.hpp"
#include <cassert>
#include <string>
#include <cstdio>
#include <cstring>
#include "arena.hpp"

#define GET_KEY(node)		   ((node)->data)
#define GET_RECORD(node, tree) ((node)->data + (tree)->key_size)

#define IS_RED(node)   ((node) && (node)->color == RED)
#define IS_BLACK(node) (!(node) || (node)->color == BLACK)

#define IS_LEFT_CHILD(node)	 ((node)->parent && (node) == (node)->parent->left)
#define IS_RIGHT_CHILD(node) ((node)->parent && (node) == (node)->parent->right)
#define IS_ROOT(node)		 ((node)->parent == nullptr)

static inline int
node_compare_key(const ephemeral_tree *tree, void *key, ephemeral_tree_node *node)
{
	return type_compare(tree->key_type, key, GET_KEY(node));
}

static ephemeral_tree_node *
alloc_node(ephemeral_tree *tree, void *key, void *record)
{
	size_t total_size = sizeof(ephemeral_tree_node) + tree->data_size;
	auto  *node = (ephemeral_tree_node *)arena<query_arena>::alloc(total_size);
	node->data = (uint8_t *)(node + 1);

	memcpy(GET_KEY(node), key, tree->key_size);
	if (record && tree->record_size > 0)
	{
		memcpy(GET_RECORD(node, tree), record, tree->record_size);
	}
	else if (tree->record_size > 0)
	{
		memset(GET_RECORD(node, tree), 0, tree->record_size);
	}

	node->left = node->right = node->parent = nullptr;
	node->color = RED;
	tree->node_count++;
	return node;
}

static ephemeral_tree_node *
tree_minimum(ephemeral_tree_node *node)
{
	while (node && node->left)
	{
		node = node->left;
	}
	return node;
}

static ephemeral_tree_node *
tree_maximum(ephemeral_tree_node *node)
{
	while (node && node->right)
	{
		node = node->right;
	}
	return node;
}

static ephemeral_tree_node *
tree_successor(ephemeral_tree_node *node)
{
	if (node->right)
	{
		return tree_minimum(node->right);
	}

	ephemeral_tree_node *parent = node->parent;
	while (parent && node == parent->right)
	{
		node = parent;
		parent = parent->parent;
	}
	return parent;
}

static ephemeral_tree_node *
tree_predecessor(ephemeral_tree_node *node)
{
	if (node->left)
	{
		return tree_maximum(node->left);
	}

	ephemeral_tree_node *parent = node->parent;
	while (parent && node == parent->left)
	{
		node = parent;
		parent = parent->parent;
	}
	return parent;
}

static void
rotate_left(ephemeral_tree *tree, ephemeral_tree_node *x)
{
	ephemeral_tree_node *y = x->right;
	x->right = y->left;
	if (y->left)
	{
		y->left->parent = x;
	}

	y->parent = x->parent;
	if (!x->parent)
	{
		tree->root = y;
	}
	else if (x == x->parent->left)
	{
		x->parent->left = y;
	}
	else
	{
		x->parent->right = y;
	}

	y->left = x;
	x->parent = y;
}

static void
rotate_right(ephemeral_tree *tree, ephemeral_tree_node *x)
{
	ephemeral_tree_node *y = x->left;
	x->left = y->right;
	if (y->right)
	{
		y->right->parent = x;
	}

	y->parent = x->parent;
	if (!x->parent)
	{
		tree->root = y;
	}
	else if (x == x->parent->right)
	{
		x->parent->right = y;
	}
	else
	{
		x->parent->left = y;
	}

	y->right = x;
	x->parent = y;
}

static void
insert_fixup(ephemeral_tree *tree, ephemeral_tree_node *z)
{

	while (z->parent && IS_RED(z->parent))
	{
		ephemeral_tree_node *grandparent = z->parent->parent;
		bool				 parent_is_left = (z->parent == grandparent->left);

		ephemeral_tree_node *uncle = parent_is_left ? grandparent->right : grandparent->left;

		if (IS_RED(uncle))
		{

			z->parent->color = BLACK;
			uncle->color = BLACK;
			grandparent->color = RED;
			z = grandparent;
		}
		else
		{

			if (parent_is_left)
			{
				if (IS_RIGHT_CHILD(z))
				{

					z = z->parent;
					rotate_left(tree, z);
				}

				z->parent->color = BLACK;
				grandparent->color = RED;
				rotate_right(tree, grandparent);
			}
			else
			{
				if (IS_LEFT_CHILD(z))
				{

					z = z->parent;
					rotate_right(tree, z);
				}

				z->parent->color = BLACK;
				grandparent->color = RED;
				rotate_left(tree, grandparent);
			}
		}
	}
	tree->root->color = BLACK;
}

static void
transplant(ephemeral_tree *tree, ephemeral_tree_node *u, ephemeral_tree_node *v)
{
	if (!u->parent)
	{
		tree->root = v;
	}
	else if (IS_LEFT_CHILD(u))
	{
		u->parent->left = v;
	}
	else
	{
		u->parent->right = v;
	}

	if (v)
	{
		v->parent = u->parent;
	}
}

static void
delete_fixup(ephemeral_tree *tree, ephemeral_tree_node *x, ephemeral_tree_node *x_parent)
{

	while (x != tree->root && IS_BLACK(x))
	{
		bool				 is_left = (x == x_parent->left);
		ephemeral_tree_node *sibling = is_left ? x_parent->right : x_parent->left;

		if (IS_RED(sibling))
		{

			sibling->color = BLACK;
			x_parent->color = RED;
			if (is_left)
			{
				rotate_left(tree, x_parent);
			}
			else
			{
				rotate_right(tree, x_parent);
			}
			sibling = is_left ? x_parent->right : x_parent->left;
		}

		bool left_black = IS_BLACK(sibling->left);
		bool right_black = IS_BLACK(sibling->right);

		if (left_black && right_black)
		{

			sibling->color = RED;
			x = x_parent;
			x_parent = x->parent;
		}
		else
		{
			if (is_left)
			{
				if (right_black)
				{

					if (sibling->left)
					{
						sibling->left->color = BLACK;
					}
					sibling->color = RED;
					rotate_right(tree, sibling);
					sibling = x_parent->right;
				}

				sibling->color = x_parent->color;
				x_parent->color = BLACK;
				if (sibling->right)
				{
					sibling->right->color = BLACK;
				}
				rotate_left(tree, x_parent);
			}
			else
			{
				if (left_black)
				{

					if (sibling->right)
					{
						sibling->right->color = BLACK;
					}
					sibling->color = RED;
					rotate_left(tree, sibling);
					sibling = x_parent->left;
				}

				sibling->color = x_parent->color;
				x_parent->color = BLACK;
				if (sibling->left)
				{
					sibling->left->color = BLACK;
				}
				rotate_right(tree, x_parent);
			}
			x = tree->root;
		}
	}
	if (x)
	{
		x->color = BLACK;
	}
}

static bool
delete_node(ephemeral_tree *tree, ephemeral_tree_node *z)
{
	if (!z)
	{
		return false;
	}

	ephemeral_tree_node *y = z;
	ephemeral_tree_node *x = nullptr;
	ephemeral_tree_node *x_parent = nullptr;
	TREE_COLOR			 y_original_color = y->color;

	if (!z->left)
	{

		x = z->right;
		x_parent = z->parent;
		transplant(tree, z, z->right);
	}
	else if (!z->right)
	{

		x = z->left;
		x_parent = z->parent;
		transplant(tree, z, z->left);
	}
	else
	{

		y = tree_minimum(z->right);
		y_original_color = y->color;
		x = y->right;

		if (y->parent == z)
		{
			x_parent = y;
		}
		else
		{
			x_parent = y->parent;
			transplant(tree, y, y->right);
			y->right = z->right;
			y->right->parent = y;
		}

		transplant(tree, z, y);
		y->left = z->left;
		y->left->parent = y;
		y->color = z->color;
	}

	tree->node_count--;

	if (y_original_color == BLACK)
	{
		delete_fixup(tree, x, x_parent);
	}

	return true;
}

static ephemeral_tree_node *
seek_eq(ephemeral_tree *tree, void *key)
{
	ephemeral_tree_node *current = tree->root;
	ephemeral_tree_node *found = nullptr;

	while (current)
	{
		int cmp = node_compare_key(tree, key, current);

		if (cmp == 0)
		{
			found = current;

			if (tree->allow_duplicates)
			{
				current = current->left;
			}
			else
			{
				return found;
			}
		}
		else
		{
			current = (cmp < 0) ? current->left : current->right;
		}
	}

	return found;
}

static ephemeral_tree_node *
seek_ge(ephemeral_tree *tree, void *key)
{
	ephemeral_tree_node *current = tree->root;
	ephemeral_tree_node *best = nullptr;

	while (current)
	{
		int cmp = node_compare_key(tree, key, current);

		if (cmp <= 0)
		{

			best = current;
			current = current->left;
		}
		else
		{
			current = current->right;
		}
	}

	return best;
}

static ephemeral_tree_node *
seek_gt(ephemeral_tree *tree, void *key)
{
	ephemeral_tree_node *current = tree->root;
	ephemeral_tree_node *best = nullptr;

	while (current)
	{
		int cmp = node_compare_key(tree, key, current);

		if (cmp < 0)
		{

			best = current;
			current = current->left;
		}
		else
		{
			current = current->right;
		}
	}

	return best;
}

static ephemeral_tree_node *
seek_le(ephemeral_tree *tree, void *key)
{
	ephemeral_tree_node *current = tree->root;
	ephemeral_tree_node *best = nullptr;

	while (current)
	{
		int cmp = node_compare_key(tree, key, current);

		if (cmp >= 0)
		{

			best = current;
			current = current->right;
		}
		else
		{
			current = current->left;
		}
	}

	return best;
}

static ephemeral_tree_node *
seek_lt(ephemeral_tree *tree, void *key)
{
	ephemeral_tree_node *current = tree->root;
	ephemeral_tree_node *best = nullptr;

	while (current)
	{
		int cmp = node_compare_key(tree, key, current);

		if (cmp > 0)
		{

			best = current;
			current = current->right;
		}
		else
		{
			current = current->left;
		}
	}

	return best;
}

ephemeral_tree
et_create(data_type key_type, uint32_t record_size, uint8_t flags)
{
	ephemeral_tree tree = {};
	tree.key_type = key_type;
	tree.key_size = type_size(key_type);
	tree.record_size = record_size;
	tree.data_size = tree.key_size + record_size;
	tree.allow_duplicates = flags & 0x01;
	return tree;
}

void
et_clear(ephemeral_tree *tree)
{
	tree->root = nullptr;
	tree->node_count = 0;
}

bool
et_insert(ephemeral_tree *tree, void *key, void *record)
{

	ephemeral_tree_node *parent = nullptr, *current = tree->root;

	while (current)
	{
		parent = current;
		int cmp = node_compare_key(tree, key, current);

		if (cmp == 0 && !tree->allow_duplicates)
		{

			if (record && tree->record_size > 0)
			{
				memcpy(GET_RECORD(current, tree), record, tree->record_size);
			}
			return true;
		}

		if (cmp == 0 && tree->allow_duplicates && tree->record_size > 0 && record)
		{
			int rec_cmp = memcmp(record, GET_RECORD(current, tree), tree->record_size);
			if (rec_cmp == 0)
			{

				memcpy(GET_RECORD(current, tree), record, tree->record_size);
				return true;
			}
			current = (rec_cmp < 0) ? current->left : current->right;
		}
		else
		{
			current = (cmp < 0) ? current->left : current->right;
		}
	}

	ephemeral_tree_node *node = alloc_node(tree, key, record);
	node->parent = parent;

	if (!parent)
	{
		tree->root = node;
	}
	else
	{
		int cmp = node_compare_key(tree, key, parent);
		if (cmp == 0 && tree->allow_duplicates && tree->record_size > 0 && record)
		{

			int rec_cmp = memcmp(record, GET_RECORD(parent, tree), tree->record_size);
			if (rec_cmp < 0)
			{
				parent->left = node;
			}
			else
			{
				parent->right = node;
			}
		}
		else
		{
			if (cmp < 0)
			{
				parent->left = node;
			}
			else
			{
				parent->right = node;
			}
		}
	}

	insert_fixup(tree, node);
	return true;
}

bool
et_delete(ephemeral_tree *tree, void *key)
{
	ephemeral_tree_node *current = tree->root;

	while (current)
	{
		int cmp = node_compare_key(tree, key, current);
		if (cmp == 0)
		{
			return delete_node(tree, current);
		}
		current = (cmp < 0) ? current->left : current->right;
	}

	return false;
}

bool
et_cursor_first(et_cursor *cursor)
{
	if (!cursor->tree.root)
	{
		cursor->state = et_cursor::AT_END;
		return false;
	}

	cursor->current = tree_minimum(cursor->tree.root);
	cursor->state = cursor->current ? et_cursor::VALID : et_cursor::AT_END;
	return cursor->current != nullptr;
}

bool
et_cursor_last(et_cursor *cursor)
{
	if (!cursor->tree.root)
	{
		cursor->state = et_cursor::AT_END;
		return false;
	}

	cursor->current = tree_maximum(cursor->tree.root);
	cursor->state = cursor->current ? et_cursor::VALID : et_cursor::AT_END;
	return cursor->current != nullptr;
}

bool
et_cursor_next(et_cursor *cursor)
{
	if (cursor->state != et_cursor::VALID)
	{
		return false;
	}

	cursor->current = tree_successor(cursor->current);
	if (cursor->current)
	{
		return true;
	}

	cursor->state = et_cursor::AT_END;
	return false;
}

bool
et_cursor_previous(et_cursor *cursor)
{
	if (cursor->state != et_cursor::VALID)
	{
		return false;
	}

	cursor->current = tree_predecessor(cursor->current);
	if (cursor->current)
	{
		return true;
	}

	cursor->state = et_cursor::AT_END;
	return false;
}

bool
et_cursor_has_next(et_cursor *cursor)
{
	if (cursor->state != et_cursor::VALID)
	{
		return false;
	}
	return tree_successor(cursor->current) != nullptr;
}

bool
et_cursor_has_previous(et_cursor *cursor)
{
	if (cursor->state != et_cursor::VALID)
	{
		return false;
	}
	return tree_predecessor(cursor->current) != nullptr;
}

bool
et_cursor_seek(et_cursor *cursor, const void *key, COMPARISON_OP op)
{
	void				*key_bytes = (void *)key;
	ephemeral_tree_node *result = nullptr;

	switch (op)
	{
	case EQ:
		result = seek_eq(&cursor->tree, key_bytes);
		break;
	case GE:
		result = seek_ge(&cursor->tree, key_bytes);
		break;
	case GT:
		result = seek_gt(&cursor->tree, key_bytes);
		break;
	case LE:
		result = seek_le(&cursor->tree, key_bytes);
		break;
	case LT:
		result = seek_lt(&cursor->tree, key_bytes);
		break;
	default:
		cursor->state = et_cursor::INVALID;
		return false;
	}

	if (result)
	{
		cursor->current = result;
		cursor->state = et_cursor::VALID;
		return true;
	}

	cursor->state = (op == EQ) ? et_cursor::INVALID : et_cursor::AT_END;
	return false;
}

void *
et_cursor_key(et_cursor *cursor)
{
	if (cursor->state != et_cursor::VALID)
	{
		return nullptr;
	}
	return GET_KEY(cursor->current);
}

void *
et_cursor_record(et_cursor *cursor)
{
	if (cursor->state != et_cursor::VALID)
	{
		return nullptr;
	}
	return GET_RECORD(cursor->current, &cursor->tree);
}

bool
et_cursor_is_valid(et_cursor *cursor)
{
	return cursor->state == et_cursor::VALID;
}

bool
et_cursor_insert(et_cursor *cursor, void *key, void *record)
{
	return et_insert(&cursor->tree, key, record);
}

bool
et_cursor_delete(et_cursor *cursor)
{
	if (cursor->state != et_cursor::VALID)
	{
		return false;
	}

	ephemeral_tree_node *next = tree_successor(cursor->current);

	bool result = delete_node(&cursor->tree, cursor->current);

	if (next)
	{
		cursor->current = next;
		cursor->state = et_cursor::VALID;
	}
	else
	{
		cursor->state = et_cursor::AT_END;
	}

	return result;
}

bool
et_cursor_update(et_cursor *cursor, void *record)
{
	if (cursor->state != et_cursor::VALID || cursor->tree.record_size == 0)
	{
		return false;
	}

	memcpy(GET_RECORD(cursor->current, &cursor->tree), record, cursor->tree.record_size);
	return true;
}

static int
validate_node_recursive(const ephemeral_tree *tree, ephemeral_tree_node *node, ephemeral_tree_node *expected_parent,
						void *min_bound, void *max_bound, hash_set<ephemeral_tree_node *, query_arena> &visited)
{
	if (!node)
	{
		return 0;
	}

	assert(!visited.contains(node) && "Cycle detected in tree");
	visited.insert(node, 1);

	assert(node->parent == expected_parent && "Parent pointer mismatch");

	void *key = GET_KEY(node);

	if (min_bound)
	{
		assert(node_compare_key(tree, min_bound, node) < 0 && "BST violation: node smaller than min bound");
	}
	if (max_bound)
	{
		assert(node_compare_key(tree, max_bound, node) > 0 && "BST violation: node larger than max bound");
	}

	if (IS_RED(node))
	{
		assert(IS_BLACK(node->left) && "Red node has red left child");
		assert(IS_BLACK(node->right) && "Red node has red right child");
		assert(node->parent != nullptr && "Red root node");
		assert(IS_BLACK(node->parent) && "Red node has red parent");
	}

	int left_black_height = validate_node_recursive(tree, node->left, node, min_bound, key, visited);
	int right_black_height = validate_node_recursive(tree, node->right, node, key, max_bound, visited);

	assert(left_black_height == right_black_height && "Black height mismatch");

	return left_black_height + (IS_BLACK(node) ? 1 : 0);
}

void
et_validate(const ephemeral_tree *tree)
{

	if (!tree->root)
	{
		assert(tree->node_count == 0);
		return;
	}

	assert(IS_BLACK(tree->root) && "Root is not black");
	assert(IS_ROOT(tree->root) && "Root has parent");

	hash_set<ephemeral_tree_node *, query_arena> visited;

	int black_height = validate_node_recursive(tree, tree->root, nullptr, nullptr, nullptr, visited);

	assert(visited.size() == tree->node_count && "Node count mismatch");

	(void)black_height;
}

static void
print_inorder_recursive(const ephemeral_tree *tree, ephemeral_tree_node *node, bool &first)
{
	if (!node)
	{
		return;
	}

	print_inorder_recursive(tree, node->left, first);

	if (!first)
	{
		printf(", ");
	}
	first = false;
	printf("[");
	type_print(tree->key_type, GET_KEY(node));
	if (tree->record_size > 0)
	{
		printf(":");

		uint8_t *rec = (uint8_t *)GET_RECORD(node, tree);
		for (uint32_t i = 0; i < std::min(tree->record_size, 4u); i++)
		{
			printf("%02x", rec[i]);
		}
		if (tree->record_size > 4)
		{
			printf("...");
		}
	}
	printf("]");

	print_inorder_recursive(tree, node->right, first);
}

static void
print_tree_visual_helper(const ephemeral_tree *tree, ephemeral_tree_node *node, const std::string &prefix, bool is_tail)
{
	if (!node)
	{
		return;
	}

	printf("%s", prefix.c_str());
	printf("%s", is_tail ? "└── " : "├── ");

	type_print(tree->key_type, GET_KEY(node));
	printf(" %s", IS_RED(node) ? "(R)" : "(B)");

	if (tree->record_size > 0)
	{
		printf(" rec:");
		auto *rec = (uint8_t *)GET_RECORD(node, tree);
		for (uint32_t i = 0; i < std::min(tree->record_size, 4u); i++)
		{
			printf("%02x", rec[i]);
		}
		if (tree->record_size > 4)
		{
			printf("...");
		}
	}
	printf("\n");

	std::string child_prefix = prefix + (is_tail ? "    " : "│   ");

	if (node->left || node->right)
	{
		if (node->right)
		{
			print_tree_visual_helper(tree, node->right, child_prefix, false);
		}
		if (node->left)
		{
			print_tree_visual_helper(tree, node->left, child_prefix, true);
		}
	}
}

void
et_print(const ephemeral_tree *tree)
{
	if (!tree || !tree->root)
	{
		printf("Ephemeral Tree: EMPTY\n");
		return;
	}

	printf("====================================\n");
	printf("Ephemeral Tree Structure (Red-Black Tree)\n");
	printf("====================================\n");
	printf("Key type: %s, Key size: %u bytes\n", type_name(tree->key_type), tree->key_size);
	printf("Record size: %u bytes\n", tree->record_size);
	printf("Allow duplicates: %s\n", tree->allow_duplicates ? "YES" : "NO");
	printf("Node count: %u\n", tree->node_count);
	printf("------------------------------------\n\n");

	struct node_level
	{
		ephemeral_tree_node *node;
		int					 level;
		bool				 is_left;
	};

	queue<node_level, query_arena> queue;
	queue.push({tree->root, 0, false});

	int current_level = -1;
	int nodes_in_level = 0;

	printf("Level-Order Traversal:\n");
	while (!queue.empty())
	{
		node_level &nl = *queue.front();
		queue.pop();

		if (nl.level != current_level)
		{
			if (current_level >= 0)
			{
				printf(" (%d nodes)\n", nodes_in_level);
			}
			printf("Level %d: ", nl.level);
			current_level = nl.level;
			nodes_in_level = 0;
		}

		if (nodes_in_level > 0)
		{
			printf("  ");
		}
		printf("[");
		type_print(tree->key_type, GET_KEY(nl.node));
		printf("]");

		printf("-%c", IS_RED(nl.node) ? 'R' : 'B');

		nodes_in_level++;

		if (nl.node->left)
		{
			queue.push({nl.node->left, nl.level + 1, true});
		}
		if (nl.node->right)
		{
			queue.push({nl.node->right, nl.level + 1, false});
		}
	}
	if (nodes_in_level > 0)
	{
		printf(" (%d nodes)\n", nodes_in_level);
	}

	printf("\n------------------------------------\n");
	printf("In-order traversal: ");
	bool first = true;
	print_inorder_recursive(tree, tree->root, first);
	printf("\n");

	printf("\nVisual Structure:\n");
	print_tree_visual_helper(tree, tree->root, "", true);
	printf("====================================\n\n");
}
