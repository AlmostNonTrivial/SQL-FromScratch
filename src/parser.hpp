/*
 * SQL From Scratch - Educational Database Engine
 *
 * SQL Parser
 *
 * This parser implements the following subset of SQL
 *
 * Data Definition Language (DDL):
 *   CREATE TABLE table_name (column_name INT|TEXT, ...)
 *   DROP TABLE table_name
 *
 * Data Manipulation Language (DML):
 *   SELECT * FROM table_name [WHERE expr] [ORDER BY column [ASC|DESC]]
 *   SELECT col1, col2, ... FROM table_name [WHERE expr] [ORDER BY column [ASC|DESC]]
 *   INSERT INTO table_name VALUES (val1, val2, ...)
 *   INSERT INTO table_name (col1, col2, ...) VALUES (val1, val2, ...)
 *   UPDATE table_name SET col1 = val1, col2 = val2, ... [WHERE expr]
 *   DELETE FROM table_name [WHERE expr]
 *
 * Transaction Control:
 *   BEGIN
 *   COMMIT
 *   ROLLBACK
 *
 *
 * AST nodes have 'sem(antic)' properties that are populated in the semantic pass
 *
 * The parser can catch lexical and syntactic errors e.g., 'SELECT @ FROM', and 'SELECT WHERE' respectively
 * But semantic errors, such as 'SELECT * table_that_doesnt_exist' are caught in the semantic pass
 *
 * Have a play around my parsing a complex statement and then call 'parse_ast' do see the output
 *
 */

#pragma once

#include "catalog.hpp"
#include "common.hpp"
#include "arena.hpp"
#include "types.hpp"

enum EXPR_TYPE : uint8_t
{
	EXPR_LITERAL = 0, // Number or string literal
	EXPR_COLUMN,	  // Column reference
	EXPR_BINARY_OP,	  // Binary operation (comparison or logical)
	EXPR_UNARY_OP,	  // Unary operation (NOT, -)
	EXPR_NULL		  // NULL literal
};

enum BINARY_OP : uint8_t
{

	OP_EQ = 0,
	OP_NE,
	OP_LT,
	OP_LE,
	OP_GT,
	OP_GE,
	OP_AND,
	OP_OR
};

enum UNARY_OP : uint8_t
{
	OP_NOT = 0,
	OP_NEG
};

struct expr_node
{
	EXPR_TYPE type;

	struct
	{
		data_type resolved_type = TYPE_NULL;
		int32_t	  column_index = -1;
	} sem;

	union {

		struct
		{
			data_type lit_type;
			union {
				uint32_t	int_val;
				string_view str_val;
			};
		};

		struct
		{
			string_view column_name;
		};

		struct
		{
			BINARY_OP  op;
			expr_node *left;
			expr_node *right;
		};

		struct
		{
			UNARY_OP   unary_op;
			expr_node *operand;
		};
	};
};

enum STMT_TYPE : uint8_t
{
	STMT_SELECT = 0,
	STMT_INSERT,
	STMT_UPDATE,
	STMT_DELETE,
	STMT_CREATE_TABLE,
	STMT_DROP_TABLE,
	STMT_BEGIN,
	STMT_COMMIT,
	STMT_ROLLBACK
};

struct attribute_node
{
	string_view name;
	data_type	type;

	struct
	{
		bool is_primary_key = false; // First column is implicitly PK
	} sem;
};

struct select_stmt
{
	bool							is_star;		 // SELECT *
	array<string_view, query_arena> columns;		 // Column names (if not *)
	string_view						table_name;		 // FROM table
	expr_node					   *where_clause;	 // Optional WHERE
	string_view						order_by_column; // Optional ORDER BY column
	bool							order_desc;		 // DESC if true, ASC if false

	struct
	{
		array<int32_t, query_arena>	  column_indices;
		array<data_type, query_arena> column_types;
		int32_t						  order_by_index = -1;

		tuple_format rb_format; // For ORDER BY temp table
	} sem;
};

struct insert_stmt
{
	string_view						table_name;
	array<string_view, query_arena> columns;
	array<expr_node *, query_arena> values;

	struct
	{
		array<int32_t, query_arena> column_indices;
	} sem;
};

struct update_stmt
{
	string_view						table_name;
	array<string_view, query_arena> columns;
	array<expr_node *, query_arena> values;
	expr_node					   *where_clause;

	struct
	{
		array<int32_t, query_arena> column_indices;
	} sem;
};

struct delete_stmt
{
	string_view table_name;
	expr_node  *where_clause;
};

struct create_table_stmt
{
	string_view						   table_name;
	array<attribute_node, query_arena> columns;

	struct
	{
		string_view created_structure;

	} sem;
};

struct drop_table_stmt
{
	string_view table_name;
};

struct begin_stmt
{
};
struct commit_stmt
{
};
struct rollback_stmt
{
};

struct stmt_node
{
	STMT_TYPE	type;
	string_view sql_stmt;

	struct
	{
		bool has_errors = false;
	} sem;

	union {
		select_stmt		  select_stmt;
		insert_stmt		  insert_stmt;
		update_stmt		  update_stmt;
		delete_stmt		  delete_stmt;
		create_table_stmt create_table_stmt;
		drop_table_stmt	  drop_table_stmt;
		begin_stmt		  begin_stmt;
		commit_stmt		  commit_stmt;
		rollback_stmt	  rollback_stmt;
	};
};

struct parser_result
{
	bool							success;
	string_view						error;					// Error message (nullptr if success)
	array<stmt_node *, query_arena> statements;				// Array by value, not pointer
	int								error_line;				// -1 if no error
	int								error_column;			// -1 if no error
	int								failed_statement_index; // Which statement failed (-1 if none)
};

parser_result
parse_sql(const char *sql);

void
print_ast(stmt_node *stmt);
