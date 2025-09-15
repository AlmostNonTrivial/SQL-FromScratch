#include "btree.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <random>
#include <thread>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <set>

#include "../common.hpp"
#include "../os_layer.hpp"
#include "../pager.hpp"
#include "../types.hpp"
#include "../btree.hpp"

#define TEST_DB "test_btree.db"

void
test_btree_sequential_ops()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	const int COUNT = 5000;

	for (int i = 0; i < COUNT; i++)
	{
		uint32_t key = i;
		uint32_t value = i * 100;

		bt_cursor_insert(&cursor, &key, (void *)&value);
		bt_validate(&tree);
	}


	for (int i = 0; i < COUNT; i++)
	{
		uint32_t key = i;
		assert(bt_cursor_seek(&cursor, &key));
		uint32_t *val = (uint32_t *)bt_cursor_record(&cursor);
		assert(*val == i * 100);
	}

	for (int i = 0; i < COUNT / 2; i++)
	{
		uint32_t key = i;
		assert(bt_cursor_seek(&cursor, &key));
		bt_cursor_delete(&cursor);
		bt_validate(&tree);
	}

	for (int i = 0; i < COUNT / 2; i++)
	{
		uint32_t key = i;
		assert(!bt_cursor_seek(&cursor, (uint8_t *)&key));
	}

	for (int i = COUNT / 2; i < COUNT; i++)
	{
		uint32_t key = i;
		assert(bt_cursor_seek(&cursor, &key));
	}

	for (int i = COUNT - 1; i >= COUNT / 2; i--)
	{
		uint32_t key = i;
		assert(bt_cursor_seek(&cursor, &key));
		bt_cursor_delete(&cursor);
		bt_validate(&tree);
	}

	assert(!bt_cursor_first(&cursor));

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_random_ops()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint64_t), true);
	bt_cursor cursor = {.tree = &tree};

	const int COUNT = 5000;

	std::vector<std::pair<uint32_t, uint64_t>> data;
	for (int i = 0; i < COUNT; i++)
	{
		data.push_back({i, (uint64_t)i * 1000});
	}

	std::mt19937 rng(42);
	std::shuffle(data.begin(), data.end(), rng);

	for (auto &[key, value] : data)
	{
		bt_cursor_insert(&cursor, &key, (void *)&value);
		bt_validate(&tree);
	}

	for (auto &[key, value] : data)
	{
		assert(bt_cursor_seek(&cursor, &key));
		uint64_t *val = (uint64_t *)bt_cursor_record(&cursor);
		assert(*val == value);
	}

	std::vector<uint32_t> keys_to_delete;
	for (auto &[key, _] : data)
	{
		keys_to_delete.push_back(key);
	}

	std::shuffle(keys_to_delete.begin(), keys_to_delete.end(), rng);
	int delete_count = keys_to_delete.size() / 2;

	std::set<uint32_t> deleted_keys;
	for (int i = 0; i < delete_count; i++)
	{
		uint32_t key = keys_to_delete[i];
		assert(bt_cursor_seek(&cursor, &key));
		bt_cursor_delete(&cursor);
		bt_validate(&tree);
		deleted_keys.insert(key);
	}

	for (auto &[key, value] : data)
	{
		if (deleted_keys.find(key) == deleted_keys.end())
		{

			assert(bt_cursor_seek(&cursor, &key));
			uint64_t *val = (uint64_t *)bt_cursor_record(&cursor);
			assert(*val == value);
		}
		else
		{

			assert(!bt_cursor_seek(&cursor, &key));
		}
	}

	for (int i = delete_count; i < keys_to_delete.size(); i++)
	{
		uint32_t key = keys_to_delete[i];
		assert(bt_cursor_seek(&cursor, &key));
		bt_cursor_delete(&cursor);
		bt_validate(&tree);
	}

	assert(!bt_cursor_first(&cursor));

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_mixed_ops()
{
	std::srand(123);

	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U64, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	std::set<uint64_t> keys_in_tree;
	const int		   ITERATIONS = 1000;
	const uint64_t	   KEY_RANGE = 1000;

	for (int i = 0; i < ITERATIONS; i++)
	{

		int op = std::rand() % 100;

		if (op < 60 || keys_in_tree.empty())
		{
			uint64_t key = std::rand() % KEY_RANGE;
			uint32_t value = key * 1000;

			bt_cursor_insert(&cursor, &key, (void *)&value);
			keys_in_tree.insert(key);
			bt_validate(&tree);
		}
		else
		{

			auto it = keys_in_tree.begin();
			std::advance(it, std::rand() % keys_in_tree.size());
			uint64_t key = *it;

			assert(bt_cursor_seek(&cursor, &key));
			bt_cursor_delete(&cursor);
			keys_in_tree.erase(key);
			bt_validate(&tree);
		}

		if (i % 50 == 0)
		{
			for (uint64_t key : keys_in_tree)
			{
				assert(bt_cursor_seek(&cursor, &key));
				uint32_t *val = (uint32_t *)bt_cursor_record(&cursor);
				assert(*val == key * 1000);
			}
		}
	}

	for (uint64_t key : keys_in_tree)
	{
		assert(bt_cursor_seek(&cursor, &key));
		bt_cursor_delete(&cursor);
		bt_validate(&tree);
	}

	assert(!bt_cursor_first(&cursor));

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_edge_cases()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	uint32_t key = 42;
	assert(!bt_cursor_seek(&cursor, &key));
	assert(!bt_cursor_delete(&cursor));
	bt_validate(&tree);

	uint32_t value = 100;
	bt_cursor_insert(&cursor, &key, (void *)&value);
	bt_validate(&tree);
	assert(bt_cursor_seek(&cursor, &key));
	bt_cursor_delete(&cursor);
	bt_validate(&tree);
	assert(!bt_cursor_first(&cursor));

	uint32_t min_key = 0;
	uint32_t max_key = UINT32_MAX;

	bt_cursor_insert(&cursor, &min_key, (void *)&value);
	bt_validate(&tree);
	bt_cursor_insert(&cursor, &max_key, (void *)&value);
	bt_validate(&tree);

	assert(bt_cursor_seek(&cursor, &min_key));
	assert(bt_cursor_seek(&cursor, &max_key));

	bt_cursor_delete(&cursor);
	bt_validate(&tree);
	assert(bt_cursor_seek(&cursor, &min_key));
	bt_cursor_delete(&cursor);
	bt_validate(&tree);

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_stress()
{

	test_btree_sequential_ops();

	test_btree_random_ops();

	test_btree_mixed_ops();

	test_btree_edge_cases();
}

void
test_btree_u32_u64()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	data_type  key_type = make_dual(TYPE_U32, TYPE_U64);
	btree	  tree = bt_create(key_type, 0, true);
	bt_cursor cursor = {.tree = &tree};

	uint8_t key_data[12];
	uint8_t empty_value = 0;

	for (uint32_t user = 1; user <= 5; user++)
	{
		for (uint64_t time = 100; time <= 103; time++)
		{
			pack_dual(key_data, TYPE_U32, &user, TYPE_U64, &time);
			assert(bt_cursor_insert(&cursor, key_data, &empty_value));
		}
	}

	uint32_t a = 3;
	uint64_t b = 0;
	pack_dual(key_data, TYPE_U32, &a, TYPE_U64, &b);

	assert(bt_cursor_seek(&cursor, key_data, GE));

	int count = 0;
	do
	{
		void	*found = bt_cursor_key(&cursor);
		uint32_t a;
		uint64_t b;
		unpack_dual(key_type, found, &a, &b);
		if (a != 3)
			break;
		count++;
	} while (bt_cursor_next(&cursor));

	assert(count == 4);

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_large_records()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	const uint32_t LARGE_RECORD = PAGE_SIZE / 4;
	btree		   tree = bt_create(TYPE_U32, LARGE_RECORD, true);
	bt_cursor	   cursor = {.tree = &tree};

	uint8_t large_data[LARGE_RECORD];

	for (uint32_t i = 0; i < 30; i++)
	{
		memset(large_data, i, LARGE_RECORD);
		assert(bt_cursor_insert(&cursor, &i, large_data));
		bt_validate(&tree);
	}

	for (uint32_t i = 0; i < 30; i++)
	{
		assert(bt_cursor_seek(&cursor, &i));
		uint8_t *data = (uint8_t *)bt_cursor_record(&cursor);
		assert(data[0] == i && data[LARGE_RECORD - 1] == i);
	}

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_multiple_cursors()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor cursor1 = {.tree = &tree};
	bt_cursor cursor2 = {.tree = &tree};
	bt_cursor cursor3 = {.tree = &tree};

	for (uint32_t i = 0; i < 100; i++)
	{
		uint32_t value = i * 100;
		assert(bt_cursor_insert(&cursor1, &i, (void *)&value));
	}

	assert(bt_cursor_first(&cursor1));

	uint32_t key = 50;
	assert(bt_cursor_seek(&cursor2, &key));

	assert(bt_cursor_last(&cursor3));

	uint32_t *key1 = (uint32_t *)bt_cursor_key(&cursor1);
	uint32_t *key2 = (uint32_t *)bt_cursor_key(&cursor2);
	uint32_t *key3 = (uint32_t *)bt_cursor_key(&cursor3);

	assert(*key1 == 0);
	assert(*key2 == 50);
	assert(*key3 == 99);

	assert(bt_cursor_next(&cursor1));
	assert(bt_cursor_previous(&cursor3));

	key1 = (uint32_t *)bt_cursor_key(&cursor1);
	key3 = (uint32_t *)bt_cursor_key(&cursor3);

	assert(*key1 == 1);
	assert(*key3 == 98);

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_page_eviction()
{
	if (MAX_CACHE_ENTRIES > 10)
	{
		return;
	}

	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	for (uint32_t i = 0; i < 1000; i++)
	{
		uint32_t value = i;
		assert(bt_cursor_insert(&cursor, &i, (void *)&value));
	}

	for (int iter = 0; iter < 3; iter++)
	{

		assert(bt_cursor_first(&cursor));
		int count = 0;
		do
		{
			count++;
		} while (bt_cursor_next(&cursor) && count < 100);

		assert(bt_cursor_last(&cursor));
		count = 0;
		do
		{
			count++;
		} while (bt_cursor_previous(&cursor) && count < 100);

		for (int i = 0; i < 50; i++)
		{
			uint32_t key = (i * 37) % 1000;
			assert(bt_cursor_seek(&cursor, &key));
		}
	}

	bt_validate(&tree);

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_varchar_collation()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_CHAR32, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	const char *test_strings[] = {
		"",
		" ",
		"  ",
		"A",
		"a",
		"AA",
		"Aa",
		"aA",
		"aa",
		"a b",
		"a  b",
		"a\tb",
		"1",
		"10",
		"2",
		"abc",
		"ABC",
		"aBc",
		"\x01",
		"\xFF",
	};

	for (int i = 0; i < sizeof(test_strings) / sizeof(test_strings[0]); i++)
	{
		char key[32] = {0};
		strncpy(key, test_strings[i], 31);
		uint32_t value = i;
		bt_cursor_insert(&cursor, key, (void *)&value);
	}

	std::vector<std::string> tree_order;
	if (bt_cursor_first(&cursor))
	{
		do
		{
			char *key = (char *)bt_cursor_key(&cursor);
			tree_order.push_back(std::string(key, strnlen(key, 32)));
		} while (bt_cursor_next(&cursor));
	}

	for (size_t i = 1; i < tree_order.size(); i++)
	{
		assert(memcmp(tree_order[i - 1].c_str(), tree_order[i].c_str(), 32) < 0);
	}

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_extended()
{
	test_btree_large_records();

	test_btree_multiple_cursors();
	test_btree_page_eviction();
	test_btree_varchar_collation();
}

void
test_update_parent_keys_condition()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	const int COUNT = tree.leaf_max_keys * 3;

	std::vector<uint32_t> keys;
	for (int i = 0; i < COUNT; i++)
	{
		keys.push_back(i);
		bt_cursor_insert(&cursor, &i, (void *)&i);
	}

	uint32_t key = 150;
	bt_cursor_seek(&cursor, &key);
	for (int i = 0; i < 182 - 150; i++)
	{
		bt_cursor_delete(&cursor);
	}

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}
void
test_merge_empty_root()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	const int COUNT = tree.leaf_max_keys + 1;

	for (int i = 0; i < COUNT; i++)
	{
		bt_cursor_insert(&cursor, &i, (void *)&i);
	}
	uint32_t key = 30;

	bt_cursor_seek(&cursor, &key);
	bt_cursor_delete(&cursor);

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_collapse_root()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor cursor = {.tree = &tree};

	for (uint32_t i = 0; i <= tree.leaf_max_keys; i++)
	{
		bt_cursor_insert(&cursor, &i, (void *)&i);
	}

	for (uint32_t i = 0; i <= tree.leaf_max_keys; i++)
	{
		assert(bt_cursor_seek(&cursor, &i));
		bt_cursor_delete(&cursor);
		bt_validate(&tree);
	}

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_deep_tree_coverage()
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	const uint32_t RECORD_SIZE = 64;
	btree		   tree = bt_create(TYPE_U32, RECORD_SIZE, true);
	bt_cursor	   cursor = {.tree = &tree};

	const int KEY_COUNT = 500;
	uint8_t	  record_data[RECORD_SIZE];

	for (int i = 0; i < KEY_COUNT; i++)
	{
		uint32_t key = i;
		memset(record_data, i % 256, RECORD_SIZE);
		assert(bt_cursor_insert(&cursor, &key, record_data));
	}

	assert(bt_cursor_first(&cursor));
	assert(bt_cursorhas_next(&cursor));
	assert(!bt_cursorhas_previous(&cursor));

	assert(bt_cursor_last(&cursor));
	assert(!bt_cursorhas_next(&cursor));
	assert(bt_cursorhas_previous(&cursor));

	uint32_t target_key = tree.leaf_max_keys;
	assert(bt_cursor_seek(&cursor, &target_key));
	assert(bt_cursor_previous(&cursor));

	target_key = tree.leaf_max_keys;
	assert(bt_cursor_seek(&cursor, &target_key));

	assert(bt_cursor_delete(&cursor));
	bt_validate(&tree);

	bt_cursor invalid_cursor = {.tree = &tree};
	invalid_cursor.state = BT_CURSOR_INVALID;

	assert(bt_cursor_key(&invalid_cursor) == nullptr);
	assert(bt_cursor_record(&invalid_cursor) == nullptr);
	assert(!bt_cursor_delete(&invalid_cursor));
	assert(!bt_cursor_update(&invalid_cursor, record_data));
	assert(!bt_cursor_next(&invalid_cursor));
	assert(!bt_cursor_previous(&invalid_cursor));

	btree	  empty_tree = bt_create(TYPE_U32, sizeof(uint32_t), false);
	bt_cursor empty_cursor = {.tree = &empty_tree};
	uint32_t  test_key = 42;
	assert(!bt_cursor_seek(&empty_cursor, &test_key));

	uint32_t cmp_key = 250;
	assert(bt_cursor_seek(&cursor, &cmp_key, GE));

	uint32_t missing_key = KEY_COUNT + 100;
	assert(bt_cursor_seek(&cursor, &missing_key, LE));


	assert(bt_cursor_first(&cursor));
	cursor.leaf_index = 999;
	assert(bt_cursor_key(&cursor) == nullptr);
	assert(bt_cursor_record(&cursor) == nullptr);

	btree	  small_tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
	bt_cursor small_cursor = {.tree = &small_tree};

	for (uint32_t i = 0; i <= small_tree.leaf_max_keys; i++)
	{
		uint32_t val = i;
		bt_cursor_insert(&small_cursor, &i, (void *)&val);
	}

	for (uint32_t i = 1; i < small_tree.leaf_min_keys; i++)
	{
		assert(bt_cursor_seek(&small_cursor, &i));
		bt_cursor_delete(&small_cursor);
	}

	assert(bt_clear(&tree));
	assert(bt_clear(&empty_tree));
	assert(bt_clear(&small_tree));

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}
void
test_btree_remaining_coverage()
{

	{
		pager_open(TEST_DB);
		pager_begin_transaction();

		btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
		bt_cursor cursor = {.tree = &tree};

		for (uint32_t i = 0; i < 200; i++)
		{
			uint32_t val = i;
			bt_cursor_insert(&cursor, &i, (void *)&val);
		}

		for (uint32_t i = 0; i < 199; i++)
		{
			if (bt_cursor_seek(&cursor, &i))
			{
				bt_cursor_delete(&cursor);
				bt_validate(&tree);
			}
		}

		pager_rollback();
		pager_close();
		os_file_delete(TEST_DB);
	}

	{

	}

	{
		pager_open(TEST_DB);
		pager_begin_transaction();

		btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
		bt_cursor cursor = {.tree = &tree};

		for (uint32_t i = 0; i < tree.leaf_max_keys * 3; i++)
		{
			uint32_t val = i;
			bt_cursor_insert(&cursor, &i, (void *)&val);
		}

		while (bt_cursor_previous(&cursor))
			;

		uint32_t key = tree.leaf_max_keys;
		assert(bt_cursor_seek(&cursor, &key));

		assert(bt_cursor_previous(&cursor));

		uint32_t *current = (uint32_t *)bt_cursor_key(&cursor);
		assert(*current == tree.leaf_max_keys - 1);

		pager_rollback();
		pager_close();
		os_file_delete(TEST_DB);
	}

	{
		pager_open(TEST_DB);
		pager_begin_transaction();

		btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
		bt_cursor cursor = {.tree = &tree};

		uint32_t keys[] = {10, 20, 30, 40, 50};
		for (int i = 0; i < 5; i++)
		{
			uint32_t val = keys[i];
			bt_cursor_insert(&cursor, &keys[i], (void *)&val);
		}

		uint32_t target = 25;

		cursor.state = BT_CURSOR_INVALID;

		assert(bt_cursor_seek(&cursor, &target, GE));

		pager_rollback();
		pager_close();
		os_file_delete(TEST_DB);
	}

	{
		pager_open(TEST_DB);
		pager_begin_transaction();

		btree tree = bt_create(TYPE_U32, sizeof(uint32_t), true);

		bt_cursor cursor = {.tree = &tree};
		uint32_t  i = 0;
		bt_cursor_seek(&cursor, &i, GE);

		pager_rollback();
		pager_close();
		os_file_delete(TEST_DB);
	}

	{
		pager_open(TEST_DB);
		pager_begin_transaction();

		btree	  tree = bt_create(TYPE_U32, sizeof(uint32_t), true);
		bt_cursor cursor = {.tree = &tree};

		for (uint32_t i = 0; i <= tree.leaf_max_keys + 1; i++)
		{
			uint32_t val = i;
			bt_cursor_insert(&cursor, &i, (void *)&val);
		}

		uint32_t target = tree.leaf_max_keys + 1;
		assert(bt_cursor_seek(&cursor, &target));

		for (uint32_t i = 1; i < tree.leaf_max_keys; i++)
		{
			bt_cursor temp_cursor = {.tree = &tree};
			if (bt_cursor_seek(&temp_cursor, &i))
			{
				bt_cursor_delete(&temp_cursor);
			}
		}

		bt_cursor_delete(&cursor);

		pager_rollback();
		pager_close();
		os_file_delete(TEST_DB);
	}
}
struct btree_test_config
{
	data_type	key_type;
	uint32_t	record_size;
	const char *name;
};

void
test_btree_sequential_ops_parameterized(const btree_test_config &config)
{
	pager_open(TEST_DB);
	pager_begin_transaction();

	btree	  tree = bt_create(config.key_type, config.record_size, true);
	bt_cursor cursor = {.tree = &tree};

	const int COUNT = 5000;
	uint32_t  key_size = type_size(config.key_type);

	std::vector<uint8_t> key_storage(COUNT * key_size);
	std::vector<uint8_t> record_storage(COUNT * config.record_size);

	for (int i = 0; i < COUNT; i++)
	{
		uint8_t *key_ptr = key_storage.data() + (i * key_size);
		uint8_t *record_ptr = record_storage.data() + (i * config.record_size);

		if (type_is_dual(config.key_type))
		{

			data_type first_type = dual_component_type(config.key_type, 0);
			data_type second_type = dual_component_type(config.key_type, 1);

			if (first_type == TYPE_U32 && second_type == TYPE_U64)
			{
				uint32_t first = i;
				uint64_t second = i * 100;
				pack_dual(key_ptr, TYPE_U32, &first, TYPE_U64, &second);
			}
			else if (first_type == TYPE_U16 && second_type == TYPE_U16)
			{
				uint16_t first = i % 65536;
				uint16_t second = (i * 10) % 65536;
				pack_dual(key_ptr, TYPE_U16, &first, TYPE_U16, &second);
			}
			else if (first_type == TYPE_U8 && second_type == TYPE_U8)
			{
				uint8_t first = i % 256;
				uint8_t second = (i * 10) % 256;
				pack_dual(key_ptr, TYPE_U8, &first, TYPE_U8, &second);
			}
		}
		else if (config.key_type == TYPE_CHAR32)
		{

			char str[32] = {0};
			snprintf(str, 31, "key_%010d", i);
			memcpy(key_ptr, str, 32);
		}
		else
		{

			switch (config.key_type)
			{
			case TYPE_U8:
				*(uint8_t *)key_ptr = i % 256;
				break;
			case TYPE_U16:
				*(uint16_t *)key_ptr = i % 65536;
				break;
			case TYPE_U32:
				*(uint32_t *)key_ptr = i;
				break;
			case TYPE_U64:
				*(uint64_t *)key_ptr = i;
				break;
			case TYPE_F64:
				*(float *)key_ptr = (float)i;
				break;
			default:
				assert(false && "Unsupported key type");
			}
		}

		if (config.record_size > 0)
		{

			for (uint32_t j = 0; j < config.record_size; j++)
			{
				record_ptr[j] = (i + j) % 256;
			}
		}
	}

	for (int i = 0; i < COUNT; i++)
	{
		uint8_t *key = key_storage.data() + (i * key_size);
		uint8_t *record = record_storage.data() + (i * config.record_size);

		bool inserted = bt_cursor_insert(&cursor, key, record);
		assert(inserted);
		bt_validate(&tree);
	}

	for (int i = 0; i < COUNT; i++)
	{
		uint8_t *key = key_storage.data() + (i * key_size);
		assert(bt_cursor_seek(&cursor, key));

		if (config.record_size > 0)
		{
			uint8_t *expected_record = record_storage.data() + (i * config.record_size);
			uint8_t *actual_record = (uint8_t *)bt_cursor_record(&cursor);
			assert(memcmp(actual_record, expected_record, config.record_size) == 0);
		}
	}

	for (int i = 0; i < COUNT / 2; i++)
	{
		uint8_t *key = key_storage.data() + (i * key_size);
		assert(bt_cursor_seek(&cursor, key));
		assert(bt_cursor_delete(&cursor));
		bt_validate(&tree);
	}

	for (int i = 0; i < COUNT / 2; i++)
	{
		uint8_t *key = key_storage.data() + (i * key_size);
		assert(!bt_cursor_seek(&cursor, key));
	}

	for (int i = COUNT / 2; i < COUNT; i++)
	{
		uint8_t *key = key_storage.data() + (i * key_size);
		assert(bt_cursor_seek(&cursor, key));
	}

	for (int i = COUNT - 1; i >= COUNT / 2; i--)
	{
		uint8_t *key = key_storage.data() + (i * key_size);
		assert(bt_cursor_seek(&cursor, key));
		assert(bt_cursor_delete(&cursor));
		bt_validate(&tree);
	}

	assert(!bt_cursor_first(&cursor));

	pager_rollback();
	pager_close();
	os_file_delete(TEST_DB);
}

void
test_btree_sequential_all_types()
{
	const btree_test_config configs[] = {
		{TYPE_U32, sizeof(uint32_t), "U32 key, U32 record"},
		{TYPE_U32, sizeof(uint64_t), "U32 key, U64 record"},
		{TYPE_U64, sizeof(uint32_t), "U64 key, U32 record"},
		{TYPE_CHAR32, sizeof(uint32_t), "VARCHAR key, U32 record"},
		{make_dual(TYPE_U32, TYPE_U64), sizeof(uint16_t), "U32+U64 key, U16 record"},
		{make_dual(TYPE_U16, TYPE_U16), 0, "U16+U16 key, no record"},
	};

	for (const auto &config : configs)
	{
		test_btree_sequential_ops_parameterized(config);
	}
}



void
test_btree()
{
    // do apply coverage.py to btree and run these to see that it's fully covered.
    // if any fail try bumping up the pager cache size
	test_btree_sequential_all_types();
	test_btree_stress();
	test_merge_empty_root();
	test_btree_extended();

	test_update_parent_keys_condition();
	test_btree_collapse_root();
	test_btree_deep_tree_coverage();
	test_btree_remaining_coverage();
	test_btree_u32_u64();
	printf("btree tests passed\n");
}
