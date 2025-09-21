/*
 * SQL From Scratch
 *
 * Read Execute Print Loop
 */


#include "arena.hpp"
#include "repl.hpp"
#include <cstdio>
#include <cstring>

#include "tests/parser.hpp"
#include "tests/types.hpp"
#include "tests/blob.hpp"
#include "tests/btree.hpp"
#include "tests/ephemeral.hpp"
#include "tests/pager.hpp"


void
print_usage(const char *program_name)
{
	printf("Usage: %s [database_file]\n", program_name);
	printf("  database_file: Path to the database file (default: relational_test.db)\n");
	printf("\nExamples:\n");
	printf("  %s                    # Use default database\n", program_name);
	printf("  %s mydata.db          # Use custom database\n", program_name);
	printf("  %s /path/to/data.db   # Use database at specific path\n", program_name);
	printf("  %s test               # Run the tests\n", program_name);
}

int
main(int argc, char **argv)
{
	arena<global_arena>::init();
	const char *database_path = "relational_test.db";

	if (argc > 2)
	{
		print_usage(argv[0]);
		return 1;
	}
	else if (argc == 2)
	{

		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
		{
			print_usage(argv[0]);
			return 0;
		}

		if (strcmp(argv[1], "test") == 0)
		{
			test_btree();
			test_pager();
			test_blob();
			test_ephemeral();
			test_parser();
			test_types();
			printf("All tests passed\n");
			exit(0);
		}

		database_path = argv[1];
	}

	return run_repl(database_path);
}
