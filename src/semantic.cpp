/*
 * SQL From Scratch - Educational Database Engine
 *
 * Semantic Analyzer
 *
 * After parsing the SQL into an abstract syntax tree, we know that there are
 * no syntactic and lexical errors, so we then check for semantic errors.
 *
 * For example, while "SELECT username FROM table_that_doesnt_exist;" is
 * valid SQL, it's not semantically valid unless said table actually exists
 * in our database. Similarly with the columns we select, and our where expressions.
 *
 * As we validate a statement, we also want to resolve information
 * about the data types and schema information we're operating upon to make the
 * ast easier to compile. For example, our expression node:
 *
 * struct expr_node
 *{
 *	EXPR_TYPE type;
 *
 *	struct
 *	{
 *		data_type resolved_type = TYPE_NULL;
 *		int32_t	  column_index = -1;
 *	} sem
 *
 *	...
 *
 * has a 'sem(antic)' context. 'SELECT * FROM users WHERE age > 50;' will
 * resolve to expr_node.sem.resolved_type = TYPE_U32, .column_index = 2
 *
 */

#include "semantic.hpp"
#include "arena.hpp"
#include "catalog.hpp"
#include "common.hpp"
#include "parser.hpp"
#include "types.hpp"
#include <cstdlib>
#include <cstring>
#include <cstdio>

struct semantic_context
{
	string_view error;
	string_view context;
	bool		modify; // for testing, don't actually modify the catalog
	char		error_buffer[256];
	char		context_buffer[256];
};

char *
format_error(semantic_context *ctx, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(ctx->error_buffer, 256, fmt, args);
	va_end(args);
	return ctx->error_buffer;
}

static void
set_error(semantic_context *ctx, string_view error, string_view context = {})
{
	ctx->error = error;
	ctx->context = context;
}

static relation *
lookup_table(semantic_context *ctx, string_view name)
{
	return catalog.get(name);
}

static int32_t
find_column_index(relation *table, string_view column_name)
{
	for (uint32_t i = 0; i < table->columns.size(); i++)
	{
		if (column_name.compare(table->columns[i].name) == 0)
		{
			return i;
		}
	}
	return -1;
}

static bool
resolve_column_list(semantic_context *ctx, relation *table, array<string_view, query_arena> &column_names,
					array<int32_t, query_arena> &out_indices)
{
	out_indices.clear();

	for (auto column_name : column_names)
	{
		int32_t idx = find_column_index(table, column_name);

		if (idx < 0)
		{
			set_error(ctx, "Column does not exist in table", column_name);
			return false;
		}

		out_indices.push(idx);
	}
	return true;
}

static const char *
sql_type_name(data_type type)
{
	return (type == TYPE_U32) ? "INT" : "TEXT";
}

static relation *
require_table(semantic_context *ctx, string_view table_name)
{
	relation *table = lookup_table(ctx, table_name);
	if (!table)
	{
		set_error(ctx, "Table does not exist", table_name);
		return nullptr;
	}

	return table;
}

static bool
validate_literal_value(semantic_context *ctx, expr_node *expr, data_type expected_type, const char *column_name,
					   const char *operation)
{
	if (expr->type != EXPR_LITERAL)
	{
		set_error(ctx, format_error(ctx, "Only literal values allowed in %s", operation), column_name);
		return false;
	}

	if (expr->lit_type != expected_type)
	{
		set_error(ctx,
				  format_error(ctx, "Type mismatch for column '%s': expected %s, got %s", column_name,
							   sql_type_name(expected_type), sql_type_name(expr->lit_type)),
				  column_name);
		return false;
	}

	expr->sem.resolved_type = expected_type;
	return true;
}

static bool
semantic_resolve_expr(semantic_context *ctx, expr_node *expr, relation *table)
{
	if (!expr)
	{
		return true;
	}

	switch (expr->type)
	{
	case EXPR_LITERAL:
		expr->sem.resolved_type = expr->lit_type;
		return true;

	case EXPR_COLUMN: {
		int32_t idx = find_column_index(table, expr->column_name);
		if (idx < 0)
		{
			set_error(ctx, "Column not found", expr->column_name);
			return false;
		}

		expr->sem.column_index = idx;
		expr->sem.resolved_type = table->columns[idx].type;
		return true;
	}

	case EXPR_BINARY_OP: {
		if (!semantic_resolve_expr(ctx, expr->left, table))
		{
			return false;
		}
		if (!semantic_resolve_expr(ctx, expr->right, table))
		{
			return false;
		}

		data_type left_type = expr->left->sem.resolved_type;
		data_type right_type = expr->right->sem.resolved_type;

		if (left_type != right_type)
		{
			set_error(ctx, "Types need to match");
			return false;
		}

		switch (expr->op)
		{
		case OP_EQ:
		case OP_NE:
		case OP_LT:
		case OP_LE:
		case OP_GT:
		case OP_GE:
		case OP_AND:
		case OP_OR:
			expr->sem.resolved_type = TYPE_U32;
			break;
		}

		return true;
	}
	case EXPR_UNARY_OP: {
		if (!semantic_resolve_expr(ctx, expr->operand, table))
		{
			return false;
		}

		if (expr->unary_op == OP_NOT)
		{
			expr->sem.resolved_type = TYPE_U32;
		}
		else if (expr->unary_op == OP_NEG)
		{
			expr->sem.resolved_type = expr->operand->sem.resolved_type;
		}

		return true;
	}

	default:
		set_error(ctx, "Unknown expression type");
		return false;
	}
}

static bool
resolve_where_clause(semantic_context *ctx, expr_node *where_clause, relation *table)
{
	if (!where_clause)
	{
		return true;
	}

	if (!semantic_resolve_expr(ctx, where_clause, table))
	{
		if (ctx->error.empty())
		{
			set_error(ctx, "Invalid expression in WHERE clause");
		}
		return false;
	}

	if (where_clause->sem.resolved_type != TYPE_U32)
	{
		set_error(ctx, "WHERE clause must evaluate to boolean");
		return false;
	}

	return true;
}

static bool
resolve_insert_columns(semantic_context *ctx, insert_stmt *stmt, relation *table)
{
	if (stmt->columns.size() > 0)
	{
		return resolve_column_list(ctx, table, stmt->columns, stmt->sem.column_indices);
	}

	stmt->sem.column_indices.clear();
	for (uint32_t i = 0; i < table->columns.size(); i++)
	{
		stmt->sem.column_indices.push(i);
	}
	return true;
}

static bool
semantic_resolve_select(semantic_context *ctx, select_stmt *stmt)
{
	relation *table = require_table(ctx, stmt->table_name);
	if (!table)
	{
		return false;
	}

	stmt->sem.column_indices.clear();
	stmt->sem.column_types.clear();

	if (stmt->is_star)
	{
		for (uint32_t i = 0; i < table->columns.size(); i++)
		{
			stmt->sem.column_indices.push(i);
			stmt->sem.column_types.push(table->columns[i].type);
		}
	}
	else
	{
		if (!resolve_column_list(ctx, table, stmt->columns, stmt->sem.column_indices))
		{
			return false;
		}

		for (uint32_t i = 0; i < stmt->sem.column_indices.size(); i++)
		{
			stmt->sem.column_types.push(table->columns[stmt->sem.column_indices[i]].type);
		}
	}

	if (!resolve_where_clause(ctx, stmt->where_clause, table))
	{
		return false;
	}

	if (stmt->order_by_column.size() > 0)
	{
		stmt->sem.order_by_index = find_column_index(table, stmt->order_by_column);
		if (stmt->sem.order_by_index < 0)
		{
			set_error(ctx, "ORDER BY column does not exist in table", stmt->order_by_column);
			return false;
		}

		array<data_type, query_arena> rb_types;
		rb_types.push(table->columns[stmt->sem.order_by_index].type);
		for (auto &type : stmt->sem.column_types)
		{
			rb_types.push(type);
		}

		stmt->sem.rb_format = tuple_format_from_types(rb_types);
	}

	return true;
}

static bool
semantic_resolve_insert(semantic_context *ctx, insert_stmt *stmt)
{
	relation *table = require_table(ctx, stmt->table_name);
	if (!table)
	{
		return false;
	}

	if (!resolve_insert_columns(ctx, stmt, table))
	{
		return false;
	}

	if (stmt->values.size() != stmt->sem.column_indices.size())
	{
		set_error(ctx,
				  format_error(ctx, "Value count mismatch: expected %u, got %u", stmt->sem.column_indices.size(),
							   stmt->values.size()),
				  stmt->table_name);
		return false;
	}

	for (uint32_t i = 0; i < stmt->values.size(); i++)
	{
		expr_node *expr = stmt->values[i];
		uint32_t   col_idx = stmt->sem.column_indices[i];
		data_type  expected_type = table->columns[col_idx].type;

		if (!validate_literal_value(ctx, expr, expected_type, table->columns[col_idx].name, "INSERT"))
		{
			return false;
		}
	}

	return true;
}

static bool
semantic_resolve_update(semantic_context *ctx, update_stmt *stmt)
{
	relation *table = require_table(ctx, stmt->table_name);
	if (!table)
	{
		return false;
	}

	if (!resolve_column_list(ctx, table, stmt->columns, stmt->sem.column_indices))
	{
		return false;
	}

	for (uint32_t i = 0; i < stmt->values.size(); i++)
	{
		expr_node *expr = stmt->values[i];
		uint32_t   col_idx = stmt->sem.column_indices[i];
		data_type  expected_type = table->columns[col_idx].type;

		if (!validate_literal_value(ctx, expr, expected_type, table->columns[col_idx].name, "UPDATE SET"))
		{
			return false;
		}
	}

	if (!resolve_where_clause(ctx, stmt->where_clause, table))
	{
		return false;
	}

	return true;
}

static bool
semantic_resolve_delete(semantic_context *ctx, delete_stmt *stmt)
{
	relation *table = require_table(ctx, stmt->table_name);
	if (!table)
	{
		return false;
	}

	if (!resolve_where_clause(ctx, stmt->where_clause, table))
	{
		return false;
	}

	return true;
}

static bool
semantic_resolve_create_table(semantic_context *ctx, create_table_stmt *stmt)
{
	if (stmt->table_name.size() > RELATION_NAME_MAX_SIZE)
	{
		set_error(ctx, format_error(ctx, "Table name max size is %u, got %u", RELATION_NAME_MAX_SIZE,
									stmt->table_name.size()));
		return false;
	}

	relation *existing = lookup_table(ctx, stmt->table_name);
	if (existing)
	{
		set_error(ctx, "Table already exists", stmt->table_name);
		return false;
	}

	if (stmt->columns.size() == 0)
	{
		set_error(ctx, "Table must have at least one column", stmt->table_name);
		return false;
	}

	for (uint32_t i = 0; i < stmt->columns.size(); i++)
	{
		if (stmt->columns[i].name.size() > ATTRIBUTE_NAME_MAX_SIZE)
		{
			set_error(ctx,
					  format_error(ctx, "Column name max size is %u, got %u", ATTRIBUTE_NAME_MAX_SIZE,
								   stmt->columns[i].name.size()),
					  stmt->columns[i].name);
			return false;
		}

		for (uint32_t j = i + 1; j < stmt->columns.size(); j++)
		{
			if (stmt->columns[i].name.compare(stmt->columns[j].name) == 0)
			{
				set_error(ctx, "Duplicate column name", stmt->columns[i].name);
				return false;
			}
		}
	}

	array<attribute, query_arena> cols;
	for (attribute_node &def : stmt->columns)
	{
		if (def.type != TYPE_U32 && def.type != TYPE_CHAR32)
		{
			set_error(ctx, "Invalid column type", def.name);
			return false;
		}

		attribute attr;
		sv_to_cstr(def.name, attr.name, ATTRIBUTE_NAME_MAX_SIZE);
		attr.type = def.type;

		cols.push(attr);
	}

	relation new_relation = create_relation(stmt->table_name, cols);
	if (ctx->modify)
	{
		catalog.insert(new_relation.name, new_relation);
	}
	stmt->sem.created_structure = stmt->table_name;
	return true;
}

static bool
semantic_resolve_drop_table(semantic_context *ctx, drop_table_stmt *stmt)
{
	relation *table = lookup_table(ctx, stmt->table_name);
	if (!table)
	{
		set_error(ctx, "Table does not exist", stmt->table_name);
		return false;
	}

	/*
	 * Remove the table from the catalog in
	 * OP_Function of the program
	 */

	return true;
}

static bool
semantic_resolve_statement(semantic_context *ctx, stmt_node *stmt)
{
	switch (stmt->type)
	{
	case STMT_SELECT:
		return semantic_resolve_select(ctx, &stmt->select_stmt);

	case STMT_INSERT:
		return semantic_resolve_insert(ctx, &stmt->insert_stmt);

	case STMT_UPDATE:
		return semantic_resolve_update(ctx, &stmt->update_stmt);

	case STMT_DELETE:
		return semantic_resolve_delete(ctx, &stmt->delete_stmt);

	case STMT_CREATE_TABLE:
		return semantic_resolve_create_table(ctx, &stmt->create_table_stmt);
	case STMT_DROP_TABLE:
		return semantic_resolve_drop_table(ctx, &stmt->drop_table_stmt);

	case STMT_BEGIN:
	case STMT_COMMIT:
	case STMT_ROLLBACK:
		return true;

	default:
		set_error(ctx, "Unknown statement type");
		return false;
	}
}

semantic_result
semantic_analyze(stmt_node *statement, bool modify_catalog)
{
	semantic_result result;
	result.success = true;
	result.error = {};
	result.error_context = {};

	semantic_context ctx;
	ctx.modify = modify_catalog;

	if (!semantic_resolve_statement(&ctx, statement))
	{
		result.success = false;
		result.error = ctx.error;
		result.error_context = ctx.context;
		return result;
	}

	return result;
}
