#include "parser.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>

 static bool
str_eq(const string_view &a, const char *b)
{
	if (a.empty() && !b)
		return true;
	if (a.empty() || !b)
		return false;
	return a.size() == strlen(b) && memcmp(a.data(), b, a.size()) == 0;
}

#define ASSERT_PRINT(condition, stmt)                                                                                  \
	do                                                                                                                 \
	{                                                                                                                  \
		if (!(condition))                                                                                              \
		{                                                                                                              \
			printf("\n❌ Assertion failed: %s\n", #condition);                                                         \
			printf("   at %s:%d\n", __FILE__, __LINE__);                                                               \
			if (stmt)                                                                                                  \
			{                                                                                                          \
				printf("\nAST:\n");                                                                                    \
				print_ast(stmt);                                                                                       \
			}                                                                                                          \
			assert(condition);                                                                                         \
		}                                                                                                              \
	} while (0)

 void
test_select_star()
{
	parser_result result = parse_sql("SELECT * FROM users");
	ASSERT_PRINT(result.success == true, nullptr);
	ASSERT_PRINT(result.statements.size() == 1, nullptr);

	stmt_node *stmt = result.statements[0];
	ASSERT_PRINT(stmt->type == STMT_SELECT, stmt);

	select_stmt *select = &stmt->select_stmt;
	ASSERT_PRINT(select->is_star == true, stmt);
	ASSERT_PRINT(str_eq(select->table_name, "users"), stmt);
	ASSERT_PRINT(select->where_clause == nullptr, stmt);
	ASSERT_PRINT(select->order_by_column.empty(), stmt);
}

 void
test_select_columns()
{
	parser_result result = parse_sql("SELECT id, name, email FROM users");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	select_stmt *select = &stmt->select_stmt;

	ASSERT_PRINT(select->is_star == false, stmt);
	ASSERT_PRINT(select->columns.size() == 3, stmt);
	ASSERT_PRINT(str_eq(select->columns[0], "id"), stmt);
	ASSERT_PRINT(str_eq(select->columns[1], "name"), stmt);
	ASSERT_PRINT(str_eq(select->columns[2], "email"), stmt);
	ASSERT_PRINT(str_eq(select->table_name, "users"), stmt);
}

 void
test_select_where()
{
	parser_result result = parse_sql("SELECT * FROM users WHERE id = 42");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	select_stmt *select = &stmt->select_stmt;

	ASSERT_PRINT(select->where_clause->type == EXPR_BINARY_OP, stmt);
	ASSERT_PRINT(select->where_clause->op == OP_EQ, stmt);
	ASSERT_PRINT(str_eq(select->where_clause->left->column_name, "id"), stmt);
	ASSERT_PRINT(select->where_clause->right->int_val == 42, stmt);
}

 void
test_select_where_complex()
{
	parser_result result = parse_sql("SELECT * FROM products WHERE price > 100 AND category = 'electronics'");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	select_stmt *select = &stmt->select_stmt;

	ASSERT_PRINT(select->where_clause->op == OP_AND, stmt);
	ASSERT_PRINT(select->where_clause->left->op == OP_GT, stmt);
	ASSERT_PRINT(str_eq(select->where_clause->left->left->column_name, "price"), stmt);
	ASSERT_PRINT(select->where_clause->left->right->int_val == 100, stmt);
	ASSERT_PRINT(select->where_clause->right->op == OP_EQ, stmt);
	ASSERT_PRINT(str_eq(select->where_clause->right->left->column_name, "category"), stmt);
	ASSERT_PRINT(str_eq(select->where_clause->right->right->str_val, "electronics"), stmt);
}

 void
test_select_order_by()
{
	parser_result result = parse_sql("SELECT * FROM users ORDER BY name ASC");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	select_stmt *select = &stmt->select_stmt;

	ASSERT_PRINT(str_eq(select->order_by_column, "name"), stmt);
	ASSERT_PRINT(select->order_desc == false, stmt);

	result = parse_sql("SELECT * FROM users ORDER BY created_at DESC");
	stmt = result.statements[0];
	select = &stmt->select_stmt;

	ASSERT_PRINT(str_eq(select->order_by_column, "created_at"), stmt);
	ASSERT_PRINT(select->order_desc == true, stmt);

	result = parse_sql("SELECT * FROM users ORDER BY id");
	stmt = result.statements[0];
	select = &stmt->select_stmt;

	ASSERT_PRINT(str_eq(select->order_by_column, "id"), stmt);
	ASSERT_PRINT(select->order_desc == false, stmt);
}

 void
test_select_full()
{
	parser_result result = parse_sql("SELECT name, email FROM users WHERE age > 18 ORDER BY name DESC");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	select_stmt *select = &stmt->select_stmt;

	ASSERT_PRINT(select->is_star == false, stmt);
	ASSERT_PRINT(select->columns.size() == 2, stmt);
	ASSERT_PRINT(str_eq(select->columns[0], "name"), stmt);
	ASSERT_PRINT(str_eq(select->columns[1], "email"), stmt);
	ASSERT_PRINT(select->where_clause->op == OP_GT, stmt);
	ASSERT_PRINT(str_eq(select->order_by_column, "name"), stmt);
	ASSERT_PRINT(select->order_desc == true, stmt);
}

 void
test_insert_values_only()
{
	parser_result result = parse_sql("INSERT INTO users VALUES (1, 'John', 'john@example.com')");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	insert_stmt *insert = &stmt->insert_stmt;

	ASSERT_PRINT(str_eq(insert->table_name, "users"), stmt);
	ASSERT_PRINT(insert->columns.size() == 0, stmt);
	ASSERT_PRINT(insert->values.size() == 3, stmt);
	ASSERT_PRINT(insert->values[0]->type == EXPR_LITERAL, stmt);
	ASSERT_PRINT(insert->values[0]->int_val == 1, stmt);
	ASSERT_PRINT(insert->values[1]->type == EXPR_LITERAL, stmt);
	ASSERT_PRINT(str_eq(insert->values[1]->str_val, "John"), stmt);
	ASSERT_PRINT(insert->values[2]->type == EXPR_LITERAL, stmt);
	ASSERT_PRINT(str_eq(insert->values[2]->str_val, "john@example.com"), stmt);
}

 void
test_insert_with_columns()
{
	parser_result result = parse_sql("INSERT INTO users (id, name, email) VALUES (1, 'John', 'john@example.com')");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	insert_stmt *insert = &stmt->insert_stmt;

	ASSERT_PRINT(insert->columns.size() == 3, stmt);
	ASSERT_PRINT(str_eq(insert->columns[0], "id"), stmt);
	ASSERT_PRINT(str_eq(insert->columns[1], "name"), stmt);
	ASSERT_PRINT(str_eq(insert->columns[2], "email"), stmt);
	ASSERT_PRINT(insert->values.size() == 3, stmt);
}

 void
test_update_no_where()
{
	parser_result result = parse_sql("UPDATE users SET status = 'active'");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	update_stmt *update = &stmt->update_stmt;

	ASSERT_PRINT(str_eq(update->table_name, "users"), stmt);
	ASSERT_PRINT(update->columns.size() == 1, stmt);
	ASSERT_PRINT(str_eq(update->columns[0], "status"), stmt);
	ASSERT_PRINT(update->values.size() == 1, stmt);
	ASSERT_PRINT(str_eq(update->values[0]->str_val, "active"), stmt);
	ASSERT_PRINT(update->where_clause == nullptr, stmt);
}

 void
test_update_with_where()
{
	parser_result result = parse_sql("UPDATE users SET name = 'Jane' WHERE id = 1");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	update_stmt *update = &stmt->update_stmt;

	ASSERT_PRINT(str_eq(update->table_name, "users"), stmt);
	ASSERT_PRINT(update->columns.size() == 1, stmt);
	ASSERT_PRINT(str_eq(update->columns[0], "name"), stmt);
	ASSERT_PRINT(str_eq(update->values[0]->str_val, "Jane"), stmt);
	ASSERT_PRINT(update->where_clause->op == OP_EQ, stmt);
}

 void
test_update_multiple_columns()
{
	parser_result result =
		parse_sql("UPDATE users SET name = 'Jane', age = 30, email = 'jane@example.com' WHERE id = 1");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	update_stmt *update = &stmt->update_stmt;

	ASSERT_PRINT(update->columns.size() == 3, stmt);
	ASSERT_PRINT(str_eq(update->columns[0], "name"), stmt);
	ASSERT_PRINT(str_eq(update->columns[1], "age"), stmt);
	ASSERT_PRINT(str_eq(update->columns[2], "email"), stmt);
	ASSERT_PRINT(str_eq(update->values[0]->str_val, "Jane"), stmt);
	ASSERT_PRINT(update->values[1]->int_val == 30, stmt);
	ASSERT_PRINT(str_eq(update->values[2]->str_val, "jane@example.com"), stmt);
}

 void
test_delete_all()
{
	parser_result result = parse_sql("DELETE FROM users");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	delete_stmt *del = &stmt->delete_stmt;

	ASSERT_PRINT(str_eq(del->table_name, "users"), stmt);
	ASSERT_PRINT(del->where_clause == nullptr, stmt);
}

 void
test_delete_where()
{
	parser_result result = parse_sql("DELETE FROM users WHERE id = 1");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node  *stmt = result.statements[0];
	delete_stmt *del = &stmt->delete_stmt;

	ASSERT_PRINT(str_eq(del->table_name, "users"), stmt);
	ASSERT_PRINT(del->where_clause->op == OP_EQ, stmt);
	ASSERT_PRINT(str_eq(del->where_clause->left->column_name, "id"), stmt);
	ASSERT_PRINT(del->where_clause->right->int_val == 1, stmt);
}

 void
test_create_table()
{
	parser_result result = parse_sql("CREATE TABLE users (id INT, name TEXT, age INT, email TEXT)");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node		*stmt = result.statements[0];
	create_table_stmt *create = &stmt->create_table_stmt;

	ASSERT_PRINT(str_eq(create->table_name, "users"), stmt);
	ASSERT_PRINT(create->columns.size() == 4, stmt);

	ASSERT_PRINT(str_eq(create->columns[0].name, "id"), stmt);
	ASSERT_PRINT(create->columns[0].type == TYPE_U32, stmt);
	ASSERT_PRINT(create->columns[0].sem.is_primary_key == true, stmt);

	ASSERT_PRINT(str_eq(create->columns[1].name, "name"), stmt);
	ASSERT_PRINT(create->columns[1].type == TYPE_CHAR32, stmt);
	ASSERT_PRINT(create->columns[1].sem.is_primary_key == false, stmt);

	ASSERT_PRINT(str_eq(create->columns[2].name, "age"), stmt);
	ASSERT_PRINT(create->columns[2].type == TYPE_U32, stmt);

	ASSERT_PRINT(str_eq(create->columns[3].name, "email"), stmt);
	ASSERT_PRINT(create->columns[3].type == TYPE_CHAR32, stmt);
}

 void
test_drop_table()
{
	parser_result result = parse_sql("DROP TABLE users");
	ASSERT_PRINT(result.success == true, nullptr);

	stmt_node	  *stmt = result.statements[0];
	drop_table_stmt *drop = &stmt->drop_table_stmt;

	ASSERT_PRINT(str_eq(drop->table_name, "users"), stmt);
}

 void
test_transactions()
{
	parser_result result = parse_sql("BEGIN");
	ASSERT_PRINT(result.success == true, nullptr);
	ASSERT_PRINT(result.statements[0]->type == STMT_BEGIN, result.statements[0]);

	result = parse_sql("COMMIT");
	ASSERT_PRINT(result.success == true, nullptr);
	ASSERT_PRINT(result.statements[0]->type == STMT_COMMIT, result.statements[0]);

	result = parse_sql("ROLLBACK");
	ASSERT_PRINT(result.success == true, nullptr);
	ASSERT_PRINT(result.statements[0]->type == STMT_ROLLBACK, result.statements[0]);
}

 void
test_expressions()
{
	parser_result result =
		parse_sql("SELECT * FROM t WHERE a = 1 AND b != 2 AND c < 3 AND d <= 4 AND e > 5 AND f >= 6");
	ASSERT_PRINT(result.success == true, nullptr);

	result = parse_sql("SELECT * FROM t WHERE a = 1 OR b = 2");
	stmt_node *stmt = result.statements[0];
	ASSERT_PRINT(stmt->select_stmt.where_clause->op == OP_OR, stmt);

	result = parse_sql("SELECT * FROM t WHERE NOT active = 1");
	stmt = result.statements[0];
	ASSERT_PRINT(stmt->select_stmt.where_clause->type == EXPR_UNARY_OP, stmt);
	ASSERT_PRINT(stmt->select_stmt.where_clause->unary_op == OP_NOT, stmt);

	result = parse_sql("SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3");
	stmt = result.statements[0];
	ASSERT_PRINT(stmt->select_stmt.where_clause->op == OP_AND, stmt);
	ASSERT_PRINT(stmt->select_stmt.where_clause->left->op == OP_OR, stmt);
}

 void
test_string_literal_size_limits()
{
	{
		const char	 *sql = "INSERT INTO users VALUES (1, 'This is a valid string')";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == true, nullptr);
		insert_stmt *insert = &result.statements[0]->insert_stmt;
		ASSERT_PRINT(insert->values[1]->type == EXPR_LITERAL, result.statements[0]);
		ASSERT_PRINT(insert->values[1]->lit_type == TYPE_CHAR32, result.statements[0]);
		ASSERT_PRINT(insert->values[1]->str_val.size() <= 32, result.statements[0]);
	}

	{
		const char	 *sql = "INSERT INTO users VALUES (1, '12345678901234567890123456789012')";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == true, nullptr);
		insert_stmt *insert = &result.statements[0]->insert_stmt;
		ASSERT_PRINT(insert->values[1]->str_val.size() == 32, result.statements[0]);
	}

	{
		const char *sql = "INSERT INTO users VALUES (1, 'This string is way too long and exceeds the 32 byte limit for "
						  "TEXT columns')";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		const char	 *sql = "INSERT INTO users VALUES (1, '123456789012345678901234567890123')";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		const char *sql =
			"SELECT * FROM users WHERE name = 'This extremely long string should not be allowed in a TEXT column'";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		const char *sql =
			"UPDATE users SET name = 'Another string that is definitely way too long for the TEXT type limit'";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		const char	 *sql = "INSERT INTO users VALUES (1, '')";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == true, nullptr);
	}

	{
		const char	 *sql = "INSERT INTO users VALUES (1, 'String with \\n newline')";
		parser_result result = parse_sql(sql);
	}
}

 void
test_integer_literal_limits()
{
	{
		const char	 *sql = "INSERT INTO users VALUES (4294967295, 'name')";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == true, nullptr);
		insert_stmt *insert = &result.statements[0]->insert_stmt;
		ASSERT_PRINT(insert->values[0]->type == EXPR_LITERAL, result.statements[0]);
		ASSERT_PRINT(insert->values[0]->lit_type == TYPE_U32, result.statements[0]);
		ASSERT_PRINT(insert->values[0]->int_val == 4294967295U, result.statements[0]);
	}

	{
		const char	 *sql = "INSERT INTO users VALUES (0, 'name')";
		parser_result result = parse_sql(sql);
		ASSERT_PRINT(result.success == true, nullptr);
		insert_stmt *insert = &result.statements[0]->insert_stmt;
		ASSERT_PRINT(insert->values[0]->int_val == 0, result.statements[0]);
	}

	{
		const char	 *sql = "INSERT INTO users VALUES (4294967296, 'name')";
		parser_result result = parse_sql(sql);
	}

	{
		const char	 *sql = "INSERT INTO users VALUES (-1, 'name')";
		parser_result result = parse_sql(sql);
	}
}

 void
test_multiple_statements()
{
	parser_result result = parse_sql("SELECT * FROM users; "
									 "INSERT INTO users VALUES (1, 'John'); "
									 "UPDATE users SET name = 'Jane' WHERE id = 1; "
									 "DELETE FROM users WHERE id = 2; "
									 "CREATE TABLE test (id INT, name TEXT); "
									 "DROP TABLE old_table; "
									 "BEGIN; "
									 "COMMIT");

	ASSERT_PRINT(result.success == true, nullptr);
	ASSERT_PRINT(result.statements.size() == 8, nullptr);

	ASSERT_PRINT(result.statements[0]->type == STMT_SELECT, result.statements[0]);
	ASSERT_PRINT(result.statements[1]->type == STMT_INSERT, result.statements[1]);
	ASSERT_PRINT(result.statements[2]->type == STMT_UPDATE, result.statements[2]);
	ASSERT_PRINT(result.statements[3]->type == STMT_DELETE, result.statements[3]);
	ASSERT_PRINT(result.statements[4]->type == STMT_CREATE_TABLE, result.statements[4]);
	ASSERT_PRINT(result.statements[5]->type == STMT_DROP_TABLE, result.statements[5]);
	ASSERT_PRINT(result.statements[6]->type == STMT_BEGIN, result.statements[6]);
	ASSERT_PRINT(result.statements[7]->type == STMT_COMMIT, result.statements[7]);
}

 void
test_statements_without_semicolons()
{
	parser_result result = parse_sql("SELECT * FROM users "
									 "INSERT INTO users VALUES (1, 'Bob') "
									 "COMMIT");

	ASSERT_PRINT(result.success == true, nullptr);
	ASSERT_PRINT(result.statements.size() == 3, nullptr);
	ASSERT_PRINT(result.statements[0]->type == STMT_SELECT, result.statements[0]);
	ASSERT_PRINT(result.statements[1]->type == STMT_INSERT, result.statements[1]);
	ASSERT_PRINT(result.statements[2]->type == STMT_COMMIT, result.statements[2]);
}

 void
test_error_handling()
{
	{
		parser_result result = parse_sql("");
		ASSERT_PRINT(result.success == true, nullptr);
		ASSERT_PRINT(result.statements.size() == 0, nullptr);
	}

	{
		parser_result result = parse_sql("INVALID SQL HERE");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
		ASSERT_PRINT(result.error_line == 1, nullptr);
		ASSERT_PRINT(result.error_column == 1, nullptr);
	}

	{
		parser_result result = parse_sql("SELECT *");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM users WHERE");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM users ORDER");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM users ORDER BY");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		parser_result result = parse_sql("INSERT users VALUES (1)");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("INSERT INTO users");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("INSERT INTO users VALUES 1, 2, 3");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("INSERT INTO users (id, name VALUES (1, 'test')");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("INSERT INTO users VALUES ()");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		parser_result result = parse_sql("UPDATE users WHERE id = 1");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("UPDATE users SET = 'value'");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("UPDATE users SET name 'value'");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("UPDATE users SET name =");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		parser_result result = parse_sql("DELETE users WHERE id = 1");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("DELETE FROM");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		parser_result result = parse_sql("CREATE users (id INT)");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("CREATE TABLE users id INT");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("CREATE TABLE users ()");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("CREATE TABLE users (id INVALID_TYPE)");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("CREATE TABLE users (id)");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		parser_result result = parse_sql("DROP users");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("DROP TABLE");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		parser_result result = parse_sql("SELECT * FROM users WHERE id =");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM users WHERE id = 1 AND");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM users WHERE id = 1 OR");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM users WHERE NOT");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);

		result = parse_sql("SELECT * FROM users WHERE (id = 1");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(!result.error.empty(), nullptr);
	}

	{
		parser_result result = parse_sql("SELECT * FROM users; INVALID SQL");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(result.statements.size() == 1, nullptr);
		ASSERT_PRINT(result.failed_statement_index == 1, nullptr);

		result = parse_sql("INVALID SQL; SELECT * FROM users");
		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(result.statements.size() == 0, nullptr);
		ASSERT_PRINT(result.failed_statement_index == 0, nullptr);
	}

	{
		const char	 *sql = "SELECT *\n"
							"FROM users\n"
							"WHERE";
		parser_result result = parse_sql(sql);

		ASSERT_PRINT(result.success == false, nullptr);
		ASSERT_PRINT(result.error_line == 3, nullptr);
	}
}

 void
test_parser()
{
	arena<query_arena>::init();

	test_select_star();
	test_select_columns();
	test_select_where();
	test_select_where_complex();
	test_select_order_by();
	test_select_full();

	test_insert_values_only();
	test_insert_with_columns();

	test_update_no_where();
	test_update_with_where();
	test_update_multiple_columns();

	test_delete_all();
	test_delete_where();

	test_create_table();
	test_drop_table();

	test_transactions();

	test_expressions();

	test_multiple_statements();
	test_statements_without_semicolons();

	test_error_handling();
	test_string_literal_size_limits();
	test_integer_literal_limits();

	printf("parser tests passed\n");
}
