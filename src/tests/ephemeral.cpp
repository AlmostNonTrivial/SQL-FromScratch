#include "ephemeral.hpp"
#include "../ephemeral.hpp"
#include "../arena.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

#define ASSERT_PRINT(tree_ptr, cond, ...)                                     \
    do {                                                                       \
        if (!(cond)) {                                                        \
            fprintf(stderr, "Assertion failed: %s\n", #cond);                 \
            fprintf(stderr, "  at %s:%d\n", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "Tree state:\n");                                 \
            et_print(tree_ptr);                                               \
            abort();                                                           \
        }                                                                      \
    } while (0)


struct simple_rng {
    uint32_t state;

    simple_rng(uint32_t seed) : state(seed) {}

    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    uint32_t next_range(uint32_t max) {
        return next() % max;
    }
};


template<typename T>
void shuffle_array(T* arr, size_t n, simple_rng& rng) {
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rng.next_range(i + 1);
        T temp = arr[i];
        arr[i] = arr[j];
        arr[j] = temp;
    }
}

 void test_ephemeral_tree_sequential_ops() {
    arena<query_arena>::init();

    et_cursor cursor = {.tree = et_create(TYPE_U32, sizeof(uint32_t), false)};
    ephemeral_tree &tree = cursor.tree;
    const int COUNT = 1000;

    for (int i = 0; i < COUNT; i++) {
        uint32_t key = i;
        uint32_t value = i * 100;
        ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)&key, (uint8_t*)&value),
                    "Failed to insert key %u\n", key);
    }

    for (int i = 0; i < COUNT; i++) {
        uint32_t key = i;
        ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key),
                    "Failed to find key %u after insertion\n", key);
        uint32_t* val = (uint32_t*)et_cursor_record(&cursor);
        ASSERT_PRINT(&tree, *val == i * 100,
                    "Value mismatch for key %u: expected %u, got %u\n", key, i * 100, *val);
    }

    for (int i = 0; i < COUNT / 2; i++) {
        uint32_t key = i;
        ASSERT_PRINT(&tree, et_delete(&tree, (uint8_t*)&key),
                    "Failed to delete key %u\n", key);
    }

    for (int i = 0; i < COUNT / 2; i++) {
        uint32_t key = i;
        ASSERT_PRINT(&tree, !et_cursor_seek(&cursor, &key),
                    "Key %u should not exist after deletion\n", key);
    }

    for (int i = COUNT / 2; i < COUNT; i++) {
        uint32_t key = i;
        ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key),
                    "Key %u should still exist\n", key);
    }

    for (int i = COUNT - 1; i >= COUNT / 2; i--) {
        uint32_t key = i;
        ASSERT_PRINT(&tree, et_delete(&tree, (uint8_t*)&key),
                    "Failed to delete key %u in backward pass\n", key);
    }

    ASSERT_PRINT(&tree, tree.node_count == 0,
                "Tree should be empty after deleting all keys\n");

    arena<query_arena>::reset();
}

 void test_ephemeral_tree_random_ops() {
    arena<query_arena>::init();

    et_cursor cursor = {.tree = et_create(TYPE_U32, sizeof(uint64_t), false)};
    ephemeral_tree &tree = cursor.tree;

    const int COUNT = 1000;

    struct kv_pair {
        uint32_t key;
        uint64_t value;
    };

    kv_pair* data = (kv_pair*)arena<query_arena>::alloc(sizeof(kv_pair) * COUNT);
    for (int i = 0; i < COUNT; i++) {
        data[i].key = i;
        data[i].value = (uint64_t)i * 1000;
    }

    simple_rng rng(42);
    shuffle_array(data, COUNT, rng);

    for (int i = 0; i < COUNT; i++) {
        ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)&data[i].key, (uint8_t*)&data[i].value),
                    "Failed to insert key %u with value %lu\n", data[i].key, data[i].value);
    }

    for (int i = 0; i < COUNT; i++) {
        uint32_t key = i;
        uint64_t expected_value = (uint64_t)i * 1000;
        ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key),
                    "Failed to find randomly inserted key %u\n", key);
        uint64_t* val = (uint64_t*)et_cursor_record(&cursor);
        ASSERT_PRINT(&tree, *val == expected_value,
                    "Value mismatch for key %u: expected %lu, got %lu\n", key, expected_value, *val);
    }

    uint32_t* keys_to_delete = (uint32_t*)arena<query_arena>::alloc(sizeof(uint32_t) * COUNT);
    for (int i = 0; i < COUNT; i++) {
        keys_to_delete[i] = i;
    }

    shuffle_array(keys_to_delete, COUNT, rng);
    int delete_count = COUNT / 2;

    bool* deleted = (bool*)arena<query_arena>::alloc(sizeof(bool) * COUNT);
    memset(deleted, 0, sizeof(bool) * COUNT);

    for (int i = 0; i < delete_count; i++) {
        uint32_t key = keys_to_delete[i];
        ASSERT_PRINT(&tree, et_delete(&tree, (uint8_t*)&key),
                    "Failed to delete key %u\n", key);
        deleted[key] = true;
        et_validate(&tree);
    }

    for (int i = 0; i < COUNT; i++) {
        uint32_t key = i;
        if (!deleted[key]) {
            ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key),
                        "Key %u should exist after partial deletion\n", key);
            uint64_t* val = (uint64_t*)et_cursor_record(&cursor);
            uint64_t expected = (uint64_t)i * 1000;
            ASSERT_PRINT(&tree, *val == expected,
                        "Value mismatch after deletion for key %u\n", key);
        } else {
            ASSERT_PRINT(&tree, !et_cursor_seek(&cursor, &key),
                        "Deleted key %u should not exist\n", key);
        }
    }



    et_validate(&tree);

    arena<query_arena>::reset();
}

 void test_ephemeral_tree_duplicates() {
    arena<query_arena>::init();

    et_cursor cursor = {.tree = et_create(TYPE_U32, sizeof(uint32_t), true)};
    ephemeral_tree &tree = cursor.tree;

    uint32_t key = 42;
    for (uint32_t i = 0; i < 10; i++) {
        uint32_t record = i * 100;
        ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)&key, (uint8_t*)&record),
                    "Failed to insert duplicate %u with record %u\n", key, record);
    }

    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key),
                "Failed to seek to duplicate key %u\n", key);

    uint32_t found_records[10];
    int found_count = 0;

    do {
        uint32_t* curr_key = (uint32_t*)et_cursor_key(&cursor);
        if (!curr_key || *curr_key != key) break;

        uint32_t* record = (uint32_t*)et_cursor_record(&cursor);
        found_records[found_count++] = *record;
    } while (et_cursor_next(&cursor));

    ASSERT_PRINT(&tree, found_count == 10,
                "Expected 10 duplicates, found %d\n", found_count);

    uint32_t target_record = 500;

    ASSERT_PRINT(&tree, et_delete(&tree, (uint8_t*)&key),
                "Failed to delete first occurrence of key %u\n", key);

    arena<query_arena>::reset();
}

 void test_ephemeral_tree_composite_keys() {
    arena<query_arena>::init();

    auto make_composite_key = [](uint32_t user_id, uint32_t timestamp) -> uint64_t {
        return ((uint64_t)user_id << 32) | timestamp;
    };

    auto extract_user_id = [](uint64_t key) -> uint32_t {
        return (uint32_t)(key >> 32);
    };

    auto extract_timestamp = [](uint64_t key) -> uint32_t {
        return (uint32_t)(key & 0xFFFFFFFF);
    };

    et_cursor cursor = {.tree = et_create(TYPE_U64, sizeof(uint64_t), false)};
    ephemeral_tree &tree = cursor.tree;

    for (uint32_t user = 1; user <= 10; user++) {
        for (uint32_t time = 100; time <= 110; time++) {
            uint64_t key = make_composite_key(user, time);
            uint64_t value = key;
            ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)&key, (uint8_t*)&value),
                        "Failed to insert composite key for user %u, time %u\n", user, time);

            et_validate(&tree);
        }
    }

    uint64_t start_key = make_composite_key(5, 0);
    uint64_t end_key = make_composite_key(6, 0);

    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &start_key, GE),
                "Failed to seek to start of range\n");
    int count = 0;
    do {
        uint64_t* found = (uint64_t*)et_cursor_key(&cursor);
        if (!found) break;

        uint32_t user_id = extract_user_id(*found);
        if (user_id >= 6) break;

        ASSERT_PRINT(&tree, user_id == 5,
                    "Expected user_id 5, got %u\n", user_id);
        count++;
    } while (et_cursor_next(&cursor));

    ASSERT_PRINT(&tree, count == 11,
                "Expected 11 timestamps for user 5, got %d\n", count);

    et_validate(&tree);
    arena<query_arena>::reset();
}

 void test_ephemeral_tree_cursor_operations() {
    arena<query_arena>::init();

    et_cursor cursor = {.tree = et_create(TYPE_U32, sizeof(uint32_t), false)};
    ephemeral_tree &tree = cursor.tree;

    for (uint32_t i = 0; i < 100; i += 10) {
        uint32_t value = i;
        ASSERT_PRINT(&tree, et_cursor_insert(&cursor, &i, (uint8_t*)&value),
                    "Failed to insert key %u\n", i);
    }

    uint32_t key = 25;
    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key, GT),
                "Failed to seek GT %u\n", key);
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 30,
                "GT seek: expected 30, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key, GE),
                "Failed to seek GE %u\n", key);
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 30,
                "GE seek: expected 30, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    key = 30;
    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key, GE),
                "Failed to seek GE %u\n", key);
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 30,
                "GE seek exact: expected 30, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    key = 35;
    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key, LT),
                "Failed to seek LT %u\n", key);
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 30,
                "LT seek: expected 30, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key, LE),
                "Failed to seek LE %u\n", key);
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 30,
                "LE seek: expected 30, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    ASSERT_PRINT(&tree, et_cursor_first(&cursor),
                "Failed to move to first\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 0,
                "First: expected 0, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    ASSERT_PRINT(&tree, et_cursor_last(&cursor),
                "Failed to move to last\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 90,
                "Last: expected 90, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    ASSERT_PRINT(&tree, et_cursor_previous(&cursor),
                "Failed to move to previous\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 80,
                "Previous: expected 80, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    ASSERT_PRINT(&tree, et_cursor_next(&cursor),
                "Failed to move to next\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 90,
                "Next: expected 90, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    key = 50;
    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key),
                "Failed to seek to %u for update\n", key);
    uint32_t new_value = 5000;
    ASSERT_PRINT(&tree, et_cursor_update(&cursor, (uint8_t*)&new_value),
                "Failed to update cursor\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_record(&cursor) == 5000,
                "Update: expected 5000, got %u\n", *(uint32_t*)et_cursor_record(&cursor));

    ASSERT_PRINT(&tree, et_cursor_seek(&cursor, &key),
                "Failed to seek to %u for delete\n", key);
    ASSERT_PRINT(&tree, et_cursor_delete(&cursor),
                "Failed to delete via cursor\n");
    ASSERT_PRINT(&tree, !et_cursor_seek(&cursor, &key),
                "Key %u should not exist after cursor delete\n", key);

    et_validate(&tree);
    arena<query_arena>::reset();
}

 void test_ephemeral_tree_edge_cases() {
    arena<query_arena>::init();

    et_cursor cursor = {.tree = et_create(TYPE_U32, sizeof(uint32_t), false)};
    ephemeral_tree &tree = cursor.tree;

    ASSERT_PRINT(&tree, tree.node_count == 0,
                "New tree should be empty\n");
    ASSERT_PRINT(&tree, !et_cursor_first(&cursor),
                "Empty tree should have no first element\n");
    ASSERT_PRINT(&tree, !et_cursor_last(&cursor),
                "Empty tree should have no last element\n");
    uint32_t key = 42;
    ASSERT_PRINT(&tree, !et_delete(&tree, (uint8_t*)&key),
                "Delete from empty tree should fail\n");

    uint32_t value = 100;
    ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)&key, (uint8_t*)&value),
                "Failed to insert single element\n");
    ASSERT_PRINT(&tree, tree.node_count != 0,
                "Tree should not be empty after insert\n");
    ASSERT_PRINT(&tree, et_cursor_first(&cursor),
                "Should find first in single-element tree\n");
    ASSERT_PRINT(&tree, et_cursor_last(&cursor),
                "Should find last in single-element tree\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 42,
                "Single element key should be 42, got %u\n", *(uint32_t*)et_cursor_key(&cursor));
    ASSERT_PRINT(&tree, et_delete(&tree, (uint8_t*)&key),
                "Failed to delete single element\n");
    ASSERT_PRINT(&tree, tree.node_count == 0,
                "Tree should be empty after deleting single element\n");

    uint32_t min_key = 0;
    uint32_t max_key = UINT32_MAX;

    ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)&min_key, (uint8_t*)&value),
                "Failed to insert min key\n");
    ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)&max_key, (uint8_t*)&value),
                "Failed to insert max key\n");

    ASSERT_PRINT(&tree, et_cursor_first(&cursor),
                "Failed to find first with boundary values\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == 0,
                "First should be 0, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    ASSERT_PRINT(&tree, et_cursor_last(&cursor),
                "Failed to find last with boundary values\n");
    ASSERT_PRINT(&tree, *(uint32_t*)et_cursor_key(&cursor) == UINT32_MAX,
                "Last should be UINT32_MAX, got %u\n", *(uint32_t*)et_cursor_key(&cursor));

    et_clear(&tree);
    ASSERT_PRINT(&tree, tree.node_count == 0,
                "Tree should be empty after clear\n");

    et_validate(&tree);
    arena<query_arena>::reset();
}

 void test_ephemeral_tree_varchar_keys() {
    arena<query_arena>::init();

    et_cursor cursor = {.tree = et_create(TYPE_CHAR32, sizeof(uint32_t), false)};
    ephemeral_tree &tree = cursor.tree;

    const char* test_strings[] = {
        "apple", "banana", "cherry", "date", "elderberry",
        "fig", "grape", "honeydew", "ice cream", "jackfruit"
    };

    for (int i = 0; i < 10; i++) {
        char key[32] = {0};
        strncpy(key, test_strings[i], 31);
        uint32_t value = i;
        ASSERT_PRINT(&tree, et_insert(&tree, (uint8_t*)key, (uint8_t*)&value),
                    "Failed to insert string key '%s'\n", test_strings[i]);
    }

    char* sorted_strings[10];
    int sorted_count = 0;

    if (et_cursor_first(&cursor)) {
        do {
            char* key = (char*)et_cursor_key(&cursor);
            sorted_strings[sorted_count] = (char*)arena<query_arena>::alloc(32);
            strncpy(sorted_strings[sorted_count], key, 31);
            sorted_count++;
        } while (et_cursor_next(&cursor) && sorted_count < 10);
    }

    for (int i = 1; i < sorted_count; i++) {
        ASSERT_PRINT(&tree, strcmp(sorted_strings[i-1], sorted_strings[i]) < 0,
                    "String ordering violated: '%s' should be < '%s'\n",
                    sorted_strings[i-1], sorted_strings[i]);
    }

    et_validate(&tree);
    arena<query_arena>::reset();
}

 void test_ephemeral() {
    test_ephemeral_tree_sequential_ops();
    test_ephemeral_tree_random_ops();
    test_ephemeral_tree_duplicates();
    test_ephemeral_tree_composite_keys();
    test_ephemeral_tree_cursor_operations();
    test_ephemeral_tree_edge_cases();
    test_ephemeral_tree_varchar_keys();

    printf("ephemeral_tests_passed\n");
}
