#include "pager.hpp"
#include "../arena.hpp"
#include "../pager.hpp"
#include <cassert>
#include "../os_layer.hpp"
#include <cstdlib>
#include <ctime>
#include <cstdio>

#define DB "db"

struct op_log_entry_
{
	char data[32];
};

static array<op_log_entry_> op_log;

#define ASSERT_PRINT(cond, msg)                                                                                        \
	do                                                                                                                 \
	{                                                                                                                  \
		if (!(cond))                                                                                                   \
		{                                                                                                              \
			printf("Assertion failed: %s\n", msg);                                                                     \
			printf("Operation log:\n");                                                                                \
			for (uint32_t i = 0; i < op_log.size(); i++)                                                               \
			{                                                                                                          \
				printf("%s\n", op_log[i].data);                                                                        \
			}                                                                                                          \
			assert(false);                                                                                             \
		}                                                                                                              \
	} while (0)

uint64_t
hash_file(const char *filename)
{
	os_file_handle_t handle = os_file_open(filename, false, false);
	if (handle == OS_INVALID_HANDLE)
	{
		return 0;
	}

	uint64_t	   hash = 0xcbf29ce484222325ULL;
	const uint64_t prime = 0x100000001b3ULL;

	uint8_t		   buffer[PAGE_SIZE];
	os_file_size_t bytes_read;
	while ((bytes_read = os_file_read(handle, buffer, sizeof(buffer))) > 0)
	{
		for (os_file_size_t i = 0; i < bytes_read; i++)
		{
			hash ^= buffer[i];
			hash *= prime;
		}
	}

	os_file_close(handle);
	return hash;
}

uint32_t
weighted_rand_op()
{
	int r = std::rand() % 100;
	if (r < 30)
	{
		return 0;
	}
	if (r < 60)
	{
		return 1;
	}
	if (r < 90)
	{
		return 2;
	}
	if (r < 95)
	{
		return 3;
	}
	return 4;
}

void
test_pager_stress()
{
	std::srand(42);
	os_file_delete(DB);
	pager_open(DB);

	op_log.clear();
	array<uint32_t> committed_pages;
	array<uint32_t> transaction_pages;
	const int		iterations = 100;
	const char		chars[] = "abcdefghijklmnopqrstuvwxyz";
	const int		char_count = sizeof(chars) - 1;
	bool			in_transaction = false;
	bool			made_changes = false;
	pager_meta		stats = pager_get_stats();
	uint64_t		before_hash = hash_file(DB);

	op_log_entry_ entry;
	snprintf(entry.data, sizeof(entry.data), "Init: f=%d t=%d", stats.free_pages, stats.total_pages);
	op_log.push(entry);

	for (int i = 0; i < iterations; ++i)
	{
		uint32_t operation;
		if (committed_pages.size() + transaction_pages.size() == 0)
		{
			operation = 0;
		}
		else
		{
			operation = weighted_rand_op();
		}

		if (operation == 0)
		{
			if (!in_transaction)
			{
				stats = pager_get_stats();
				before_hash = hash_file(DB);
				pager_begin_transaction();
				in_transaction = true;
				snprintf(entry.data, sizeof(entry.data), "BeginTx");
				op_log.push(entry);
			}
			uint32_t page_id = pager_new();
			ASSERT_PRINT(page_id != 0, "Failed to create new page");
			transaction_pages.push(page_id);
			made_changes = true;
			snprintf(entry.data, sizeof(entry.data), "Create p=%d", page_id);
			op_log.push(entry);
		}
		else if (operation == 1 && (committed_pages.size() + transaction_pages.size() > 0))
		{
			if (!in_transaction)
			{
				stats = pager_get_stats();
				before_hash = hash_file(DB);
				pager_begin_transaction();
				in_transaction = true;
				snprintf(entry.data, sizeof(entry.data), "BeginTx");
				op_log.push(entry);
			}
			uint32_t   total_size = committed_pages.size() + transaction_pages.size();
			uint32_t   index = std::rand() % total_size;
			uint32_t   page_id = index < committed_pages.size() ? committed_pages[index]
																: transaction_pages[index - committed_pages.size()];
			base_page *page = pager_get(page_id);
			ASSERT_PRINT(page != nullptr, "Failed to get page for writing");
			char random_char = chars[std::rand() % char_count];
			pager_ensure_journaled(page_id);
			page->data[0] = random_char;
			made_changes = true;
			snprintf(entry.data, sizeof(entry.data), "Write p=%d c=%c", page_id, random_char);
			op_log.push(entry);
			base_page *verify_page = pager_get(page_id);
			ASSERT_PRINT(verify_page->data[0] == random_char, "Write verification failed");
		}
		else if (operation == 2 && (committed_pages.size() + transaction_pages.size() > 0))
		{
			if (!in_transaction)
			{
				stats = pager_get_stats();
				before_hash = hash_file(DB);
				pager_begin_transaction();
				in_transaction = true;
				snprintf(entry.data, sizeof(entry.data), "BeginTx");
				op_log.push(entry);
			}
			uint32_t total_size = committed_pages.size() + transaction_pages.size();
			uint32_t index = std::rand() % total_size;
			uint32_t page_id = index < committed_pages.size() ? committed_pages[index]
															  : transaction_pages[index - committed_pages.size()];
			pager_delete(page_id);
			made_changes = true;
			snprintf(entry.data, sizeof(entry.data), "Delete p=%d", page_id);
			op_log.push(entry);
			if (index < committed_pages.size())
			{
				committed_pages[index] = committed_pages[committed_pages.size() - 1];
				committed_pages.pop_back();
			}
			else
			{
				index -= committed_pages.size();
				transaction_pages[index] = transaction_pages[transaction_pages.size() - 1];
				transaction_pages.pop_back();
			}
		}
		else if (operation == 3 && in_transaction)
		{
			pager_rollback();
			in_transaction = false;
			made_changes = false;
			snprintf(entry.data, sizeof(entry.data), "Rollback");
			op_log.push(entry);

			transaction_pages.clear();
			pager_meta new_stats = pager_get_stats();
			uint64_t   after_hash = hash_file(DB);
			ASSERT_PRINT(before_hash == after_hash, "File hash changed after rollback");
			ASSERT_PRINT(new_stats.free_pages == stats.free_pages && new_stats.total_pages == stats.total_pages,
						 "Stats not restored after rollback");
		}
		else if (operation == 4 && in_transaction && made_changes)
		{
			pager_commit();
			in_transaction = false;
			made_changes = false;
			snprintf(entry.data, sizeof(entry.data), "Commit");
			op_log.push(entry);
			for (uint32_t j = 0; j < transaction_pages.size(); ++j)
			{
				committed_pages.push(transaction_pages[j]);
			}
			transaction_pages.clear();
			pager_meta new_stats = pager_get_stats();
			uint64_t   after_hash = hash_file(DB);
			ASSERT_PRINT(before_hash != after_hash, "File hash unchanged after commit");
			stats = new_stats;
			before_hash = after_hash;
		}
	}

	if (in_transaction)
	{
		if (made_changes)
		{
			pager_commit();
			snprintf(entry.data, sizeof(entry.data), "FinalCommit");
			op_log.push(entry);
			for (uint32_t j = 0; j < transaction_pages.size(); ++j)
			{
				committed_pages.push(transaction_pages[j]);
			}
			transaction_pages.clear();
		}
		else
		{
			pager_rollback();
			snprintf(entry.data, sizeof(entry.data), "FinalRollback");
			op_log.push(entry);
			transaction_pages.clear();
		}
	}

	pager_close();
	os_file_delete(DB);

	printf("Stress test passed\n");
}

void
test_pager()
{
	test_pager_stress();
}
