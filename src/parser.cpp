/*
 * SQL From Scratch
 *
 * SQL Parser
 *
 * For visualizations of AST's I recommend checking out https://astexplorer.net/
 *
 * Lexer: Converts character stream → tokens (SELECT → TOKEN_KEYWORD)
 * Parser: Converts tokens → Abstract Syntax Tree (AST)
 *
 * Example flow for "SELECT * FROM users":
 *   1. Lexer produces: [SELECT keyword] [* star] [FROM keyword] [users identifier]
 *   2. Parser recognizes SELECT pattern, calls parse_select()
 *   3. parse_select() consumes tokens and builds SelectStmt AST node
 *
 * The AST nodes are arena allocated, but don't copy the original input, rather they
 * have string_view's on the buffer, so don't modify it.
 */

#include "parser.hpp"
#include "arena.hpp"
#include "common.hpp"
#include "types.hpp"
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <string_view>
#include <unordered_map>

enum TOKEN_TYPE : uint8_t
{
	TOKEN_EOF = 0,
	TOKEN_IDENTIFIER,
	TOKEN_NUMBER,
	TOKEN_STRING,
	TOKEN_KEYWORD,
	TOKEN_OPERATOR,
	TOKEN_LPAREN,
	TOKEN_RPAREN,
	TOKEN_COMMA,
	TOKEN_SEMICOLON,
	TOKEN_STAR
};

struct tok
{
	TOKEN_TYPE	type;
	string_view text;
	uint32_t	line;
	uint32_t	column;
};

struct lexer
{
	const char *input;
	const char *current;
	uint32_t	line;
	uint32_t	column;
	tok			current_token;
};

struct parser
{
	lexer		lex;
	string_view error_msg;
	int			error_line;
	int			error_column;
};

const char *
stmt_type_to_string(STMT_TYPE type)
{
	switch (type)
	{
	case STMT_SELECT:
		return "SELECT";
	case STMT_INSERT:
		return "INSERT";
	case STMT_UPDATE:
		return "UPDATE";
	case STMT_DELETE:
		return "DELETE";
	case STMT_CREATE_TABLE:
		return "CREATE_TABLE";
	case STMT_DROP_TABLE:
		return "DROP_TABLE";
	case STMT_BEGIN:
		return "BEGIN";
	case STMT_COMMIT:
		return "COMMIT";
	case STMT_ROLLBACK:
		return "ROLLBACK";
	default:
		return "UNKNOWN";
	}
}
/*
 * select and SELECT are 1, such that keywords['select'] != keywords['Anything else']
 * but keywords['SELECT'] does equal keywords['select']
 *
 */
const static std::unordered_map<string_view, int32_t> sql_keywords = {
	{"SELECT", 1},	  {"select", 1},	{"FROM", 2},   {"from", 2},	  {"WHERE", 3},	  {"where", 3},	  {"INSERT", 4},
	{"insert", 4},	  {"INTO", 5},		{"into", 5},   {"VALUES", 6}, {"values", 6},  {"UPDATE", 7},  {"update", 7},
	{"SET", 8},		  {"set", 8},		{"DELETE", 9}, {"delete", 9}, {"CREATE", 10}, {"create", 10}, {"TABLE", 11},
	{"table", 11},	  {"DROP", 12},		{"drop", 12},  {"BEGIN", 13}, {"begin", 13},  {"COMMIT", 14}, {"commit", 14},
	{"ROLLBACK", 15}, {"rollback", 15}, {"AND", 16},   {"and", 16},	  {"OR", 17},	  {"or", 17},	  {"NOT", 18},
	{"not", 18},	  {"NULL", 19},		{"null", 19},  {"ORDER", 20}, {"order", 20},  {"BY", 21},	  {"by", 21},
	{"ASC", 22},	  {"asc", 22},		{"DESC", 23},  {"desc", 23},  {"INT", 24},	  {"int", 24},	  {"TEXT", 25},
	{"text", 25}};

static string_view
format_error(parser *p, const char *fmt, ...)
{

	char   *buffer = (char *)arena<query_arena>::alloc(256);
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, 256, fmt, args);
	va_end(args);

	p->error_msg = string_view(buffer, strlen(buffer));
	p->error_line = p->lex.line;
	p->error_column = p->lex.column;

	return p->error_msg;
}


static bool
is_keyword(string_view text)
{
	return sql_keywords.find(text) != sql_keywords.end();
}

static bool
is_keyword(string_view text, string_view keyword)
{
	auto a = sql_keywords.find(text);
	auto b = sql_keywords.find(keyword);
	if (a == sql_keywords.end() || b == sql_keywords.end())
	{
		return false;
	}

	return a->second == b->second;
}

void
lexer_init(lexer *lex, const char *input)
{
	lex->input = input;
	lex->current = input;
	lex->line = 1;
	lex->column = 1;
	lex->current_token = {TOKEN_EOF, string_view{}, 0, 0};
}

static void
skip_whitespace(lexer *lex)
{
	while (*lex->current)
	{
		if (*lex->current == ' ' || *lex->current == '\t' || *lex->current == '\r')
		{
			lex->column++;
			lex->current++;
		}
		else if (*lex->current == '\n')
		{
			lex->line++;
			lex->column = 1;
			lex->current++;
		}
		else if (lex->current[0] == '-' && lex->current[1] == '-')
		{
			// SQL comment
			while (*lex->current && *lex->current != '\n')
			{
				lex->current++;
			}
		}
		else
		{
			break;
		}
	}
}

tok
lexer_next_token(lexer *lex)
{
	skip_whitespace(lex);

	tok token;
	token.line = lex->line;
	token.column = lex->column;

	if (*lex->current == '\0')
	{
		token.type = TOKEN_EOF;
		token.text = string_view{};
		lex->current_token = token;
		return token;
	}

	const char *start = lex->current;

	// Single character tokens
	if (*lex->current == '(')
	{
		token.type = TOKEN_LPAREN;
		token.text = string_view(lex->current, 1);
		lex->current++;
		lex->column++;
		lex->current_token = token;
		return token;
	}

	if (*lex->current == ')')
	{
		token.type = TOKEN_RPAREN;
		token.text = string_view(lex->current, 1);
		lex->current++;
		lex->column++;
		lex->current_token = token;
		return token;
	}

	if (*lex->current == ',')
	{
		token.type = TOKEN_COMMA;
		token.text = string_view(lex->current, 1);
		lex->current++;
		lex->column++;
		lex->current_token = token;
		return token;
	}

	if (*lex->current == ';')
	{
		token.type = TOKEN_SEMICOLON;
		token.text = string_view(lex->current, 1);
		lex->current++;
		lex->column++;
		lex->current_token = token;
		return token;
	}

	if (*lex->current == '*')
	{
		token.type = TOKEN_STAR;
		token.text = string_view(lex->current, 1);
		lex->current++;
		lex->column++;
		lex->current_token = token;
		return token;
	}

	// Operators
	if (*lex->current == '=' || *lex->current == '<' || *lex->current == '>' || *lex->current == '!')
	{
		start = lex->current;
		lex->current++;
		lex->column++;

		// Two-character operators
		if (*lex->current == '=' || (*start == '<' && *lex->current == '>'))
		{
			lex->current++;
			lex->column++;
		}

		token.type = TOKEN_OPERATOR;
		token.text = string_view(start, lex->current - start);
		lex->current_token = token;
		return token;
	}

	// String literals
	if (*lex->current == '\'')
	{
		lex->current++; // Skip opening quote
		start = lex->current;

		while (*lex->current && *lex->current != '\'')
		{
			lex->current++;
			lex->column++;
		}

		token.type = TOKEN_STRING;
		token.text = string_view(start, lex->current - start);

		if (*lex->current == '\'')
		{
			lex->current++; // Skip closing quote
			lex->column++;
		}

		lex->current_token = token;
		return token;
	}

	// Numbers
	if (isdigit(*lex->current))
	{
		start = lex->current;
		while (isdigit(*lex->current))
		{
			lex->current++;
			lex->column++;
		}

		token.type = TOKEN_NUMBER;
		token.text = string_view(start, lex->current - start);
		lex->current_token = token;
		return token;
	}

	// Identifiers and keywords
	if (isalpha(*lex->current) || *lex->current == '_')
	{
		start = lex->current;
		while (isalnum(*lex->current) || *lex->current == '_')
		{
			lex->current++;
			lex->column++;
		}

		token.text = string_view(start, lex->current - start);
		token.type = is_keyword(token.text) ? TOKEN_KEYWORD : TOKEN_IDENTIFIER;

		lex->current_token = token;
		return token;
	}

	// Unknown character
	token.type = TOKEN_EOF;
	token.text = string_view(lex->current, 1);
	lex->current++;
	lex->column++;
	lex->current_token = token;
	return token;
}

tok
lexer_peek_token(lexer *lex)
{
	const char *saved_current = lex->current;
	uint32_t	saved_line = lex->line;
	uint32_t	saved_column = lex->column;
	tok			saved_token = lex->current_token;

	tok token = lexer_next_token(lex);

	lex->current = saved_current;
	lex->line = saved_line;
	lex->column = saved_column;
	lex->current_token = saved_token;

	return token;
}

bool
consume_token(parser *parser, TOKEN_TYPE expected_type)
{
	tok token = lexer_peek_token(&parser->lex);
	if (token.type == expected_type)
	{
		lexer_next_token(&parser->lex);
		return true;
	}
	return false;
}

bool
consume_keyword(parser *parser, const char *keyword)
{
	tok token = lexer_peek_token(&parser->lex);
	if (token.type == TOKEN_KEYWORD && is_keyword(token.text, string_view(keyword)))
	{
		lexer_next_token(&parser->lex);
		return true;
	}
	return false;
}

bool
peek_keyword(parser *parser, const char *keyword)
{
	tok token = lexer_peek_token(&parser->lex);
	return token.type == TOKEN_KEYWORD && is_keyword(token.text, keyword);
}

static bool
consume_operator(parser *parser, const char *op)
{
	tok token = lexer_peek_token(&parser->lex);
	if (token.type == TOKEN_OPERATOR && token.text == string_view(op))
	{
		lexer_next_token(&parser->lex);
		return true;
	}
	return false;
}


static bool
parse_uint32(string_view sv, uint32_t *out)
{
	uint32_t val = 0;
	for (char c : sv)
	{
		if (c < '0' || c > '9')
		{
			return false;
		}

		uint32_t digit = c - '0';
		if (val > (UINT32_MAX - digit) / 10)
		{
			return false; // Overflow
		}

		val = val * 10 + digit;
	}

	*out = val;
	return true;
}

/*
 * EXPRESSION PARSING
 *
 * Precedence (lowest to highest):
 *   OR → AND → NOT → Comparisons (=, <, >, etc)
 *
 * Example: "a = 1 AND b = 2 OR c = 3" parses as:
 *   OR
 *   ├── AND
 *   │   ├── (a = 1)
 *   │   └── (b = 2)
 *   └── (c = 3)
 *
 */

data_type
parse_data_type(parser *parser);
expr_node *
parse_expression(parser *parser);
expr_node *
parse_where_clause(parser *parser);
expr_node *
parse_or_expr(parser *parser);
expr_node *
parse_and_expr(parser *parser);
expr_node *
parse_not_expr(parser *parser);
expr_node *
parse_comparison_expr(parser *parser);
expr_node *
parse_primary_expr(parser *parser);

data_type
parse_data_type(parser *parser)
{
	if (consume_keyword(parser, "INT"))
	{
		return TYPE_U32;
	}
	if (consume_keyword(parser, "TEXT"))
	{
		return TYPE_CHAR32;
	}

	format_error(parser, "Expected data type (INT or TEXT)");
	return TYPE_NULL;
}

expr_node *
parse_expression(parser *parser)
{
	return parse_or_expr(parser);
}

/*
 * OR has lowest precedence, builds tree right-to-left
 * Pattern: expr OR expr OR expr
 * Tree:    OR
 *         /  \
 *        OR   expr3
 *       /  \
 *    expr1  expr2
 */
expr_node *
parse_or_expr(parser *parser)
{
	expr_node *left = parse_and_expr(parser);
	if (!left)
	{
		return nullptr;
	}

	while (consume_keyword(parser, "OR"))
	{
		expr_node *right = parse_and_expr(parser);
		if (!right)
		{
			format_error(parser, "Expected expression after OR");
			return nullptr;
		}

		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_BINARY_OP;
		expr->op = OP_OR;
		expr->left = left;
		expr->right = right;
		left = expr;
	}

	return left;
}

/*
 * AND binds tighter than OR
 * Pattern: expr AND expr AND expr
 * Tree:    AND
 *         /   \
 *       AND    expr3
 *      /   \
 *   expr1  expr2
 */
expr_node *
parse_and_expr(parser *parser)
{
	expr_node *left = parse_not_expr(parser);
	if (!left)
	{
		return nullptr;
	}

	while (consume_keyword(parser, "AND"))
	{
		expr_node *right = parse_not_expr(parser);
		if (!right)
		{
			format_error(parser, "Expected expression after AND");
			return nullptr;
		}

		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_BINARY_OP;
		expr->op = OP_AND;
		expr->left = left;
		expr->right = right;
		left = expr;
	}

	return left;
}

/*
 * NOT is unary prefix operator
 * Pattern: NOT expr
 * Tree:    NOT
 *           |
 *         expr
 * Note: NOT NOT expr is valid and creates nested nodes
 */
expr_node *
parse_not_expr(parser *parser)
{
	if (consume_keyword(parser, "NOT"))
	{
		expr_node *operand = parse_not_expr(parser);
		if (!operand)
		{
			format_error(parser, "Expected expression after NOT");
			return nullptr;
		}

		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_UNARY_OP;
		expr->unary_op = OP_NOT;
		expr->operand = operand;
		return expr;
	}

	return parse_comparison_expr(parser);
}

expr_node *
parse_comparison_expr(parser *parser)
{
	expr_node *left = parse_primary_expr(parser);
	if (!left)
	{
		return nullptr;
	}

	tok token = lexer_peek_token(&parser->lex);
	if (token.type == TOKEN_OPERATOR)
	{
		BINARY_OP op;

		if (consume_operator(parser, "="))
		{
			op = OP_EQ;
		}
		else if (consume_operator(parser, "!=") || consume_operator(parser, "<>"))
		{
			op = OP_NE;
		}
		else if (consume_operator(parser, "<="))
		{
			op = OP_LE;
		}
		else if (consume_operator(parser, ">="))
		{
			op = OP_GE;
		}
		else if (consume_operator(parser, "<"))
		{
			op = OP_LT;
		}
		else if (consume_operator(parser, ">"))
		{
			op = OP_GT;
		}
		else
		{
			return left;
		}

		expr_node *right = parse_primary_expr(parser);
		if (!right)
		{
			format_error(parser, "Expected expression after comparison operator");
			return nullptr;
		}

		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_BINARY_OP;
		expr->op = op;
		expr->left = left;
		expr->right = right;
		return expr;
	}

	return left;
}

expr_node *
parse_primary_expr(parser *parser)
{
	tok token = lexer_peek_token(&parser->lex);

	// Parenthesized expression
	if (token.type == TOKEN_LPAREN)
	{
		lexer_next_token(&parser->lex);
		expr_node *expr = parse_expression(parser);
		if (!expr)
		{
			return nullptr;
		}
		if (!consume_token(parser, TOKEN_RPAREN))
		{
			format_error(parser, "Expected ')' after expression");
			return nullptr;
		}
		return expr;
	}

	// NULL literal
	if (token.type == TOKEN_KEYWORD && is_keyword(token.text, "NULL"))
	{
		lexer_next_token(&parser->lex);
		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_LITERAL;
		expr->lit_type = TYPE_NULL;
		return expr;
	}

	token = lexer_next_token(&parser->lex);

	// Number literal
	if (token.type == TOKEN_NUMBER)
	{
		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_LITERAL;
		expr->lit_type = TYPE_U32;

		if (!parse_uint32(token.text, &expr->int_val))
		{
			format_error(parser, "Invalid number");
			return nullptr;
		}

		return expr;
	}

	// String literal
	if (token.type == TOKEN_STRING)
	{

		if (token.text.size() > type_size(TYPE_CHAR32))
		{
			format_error(parser, "Literal 32 byte limit for TEXT columns");
			return nullptr;
		}

		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_LITERAL;
		expr->lit_type = TYPE_CHAR32;
		expr->str_val = token.text;
		return expr;
	}

	// Column reference
	if (token.type == TOKEN_IDENTIFIER)
	{
		expr_node *expr = (expr_node *)arena<query_arena>::alloc(sizeof(expr_node));
		expr->type = EXPR_COLUMN;
		expr->column_name = token.text;
		return expr;
	}

	format_error(parser, "Unexpected token '%.*s'", (int)token.text.size(), token.text.data());
	return nullptr;
}

expr_node *
parse_where_clause(parser *parser)
{
	if (!consume_keyword(parser, "WHERE"))
	{
		return nullptr;
	}

	expr_node *expr = parse_expression(parser);
	if (!expr)
	{
		format_error(parser, "Expected expression after WHERE");
	}

	return expr;
}

void
parse_select(parser *parser, select_stmt *stmt)
{
	if (!consume_keyword(parser, "SELECT"))
	{
		format_error(parser, "Expected SELECT");
		return;
	}

	if (consume_token(parser, TOKEN_STAR))
	{
		stmt->is_star = true;
	}
	else
	{
		stmt->is_star = false;

		do
		{
			tok token = lexer_next_token(&parser->lex);
			if (token.type != TOKEN_IDENTIFIER)
			{
				format_error(parser, "Expected column name in SELECT list");
				return;
			}

			stmt->columns.push(token.text);
		} while (consume_token(parser, TOKEN_COMMA));
	}

	if (!consume_keyword(parser, "FROM"))
	{
		format_error(parser, "Expected FROM after SELECT list");
		return;
	}

	tok token = lexer_next_token(&parser->lex);
	if (token.type != TOKEN_IDENTIFIER)
	{
		format_error(parser, "Expected table name after FROM");
		return;
	}

	stmt->table_name = token.text;

	stmt->where_clause = parse_where_clause(parser);

	if (consume_keyword(parser, "ORDER"))
	{
		if (!consume_keyword(parser, "BY"))
		{
			format_error(parser, "Expected BY after ORDER");
			return;
		}

		token = lexer_next_token(&parser->lex);
		if (token.type != TOKEN_IDENTIFIER)
		{
			format_error(parser, "Expected column name after ORDER BY");
			return;
		}

		stmt->order_by_column = token.text;

		if (consume_keyword(parser, "DESC"))
		{
			stmt->order_desc = true;
		}
		else
		{
			consume_keyword(parser, "ASC");
			stmt->order_desc = false;
		}
	}
}

void
parse_insert(parser *parser, insert_stmt *stmt)
{
	if (!consume_keyword(parser, "INSERT"))
	{
		format_error(parser, "Expected INSERT");
		return;
	}

	if (!consume_keyword(parser, "INTO"))
	{
		format_error(parser, "Expected INTO after INSERT");
		return;
	}

	tok token = lexer_next_token(&parser->lex);
	if (token.type != TOKEN_IDENTIFIER)
	{
		format_error(parser, "Expected table name after INSERT INTO");
		return;
	}

	stmt->table_name = token.text;

	if (consume_token(parser, TOKEN_LPAREN))
	{
		do
		{
			token = lexer_next_token(&parser->lex);
			if (token.type != TOKEN_IDENTIFIER)
			{
				format_error(parser, "Expected column name in INSERT column list");
				return;
			}

			stmt->columns.push(token.text);
		} while (consume_token(parser, TOKEN_COMMA));

		if (!consume_token(parser, TOKEN_RPAREN))
		{
			format_error(parser, "Expected ')' after column list");
			return;
		}
	}

	if (!consume_keyword(parser, "VALUES"))
	{
		format_error(parser, "Expected VALUES after table name");
		return;
	}

	if (!consume_token(parser, TOKEN_LPAREN))
	{
		format_error(parser, "Expected '(' after VALUES");
		return;
	}

	do
	{
		expr_node *expr = parse_expression(parser);
		if (!expr)
		{
			format_error(parser, "Expected value expression in VALUES list");
			return;
		}
		stmt->values.push(expr);
	} while (consume_token(parser, TOKEN_COMMA));

	if (!consume_token(parser, TOKEN_RPAREN))
	{
		format_error(parser, "Expected ')' after VALUES list");
		return;
	}
}

void
parse_update(parser *parser, update_stmt *stmt)
{
	if (!consume_keyword(parser, "UPDATE"))
	{
		format_error(parser, "Expected UPDATE");
		return;
	}

	tok token = lexer_next_token(&parser->lex);
	if (token.type != TOKEN_IDENTIFIER)
	{
		format_error(parser, "Expected table name after UPDATE");
		return;
	}

	stmt->table_name = token.text;

	if (!consume_keyword(parser, "SET"))
	{
		format_error(parser, "Expected SET after table name");
		return;
	}

	do
	{
		token = lexer_next_token(&parser->lex);
		if (token.type != TOKEN_IDENTIFIER)
		{
			format_error(parser, "Expected column name in SET clause");
			return;
		}

		stmt->columns.push(token.text);

		if (!consume_operator(parser, "="))
		{
			format_error(parser, "Expected '=' after column name");
			return;
		}

		expr_node *expr = parse_expression(parser);
		if (!expr)
		{
			format_error(parser, "Expected value expression after '='");
			return;
		}
		stmt->values.push(expr);
	} while (consume_token(parser, TOKEN_COMMA));

	stmt->where_clause = parse_where_clause(parser);
}

void
parse_delete(parser *parser, delete_stmt *stmt)
{
	if (!consume_keyword(parser, "DELETE"))
	{
		format_error(parser, "Expected DELETE");
		return;
	}

	if (!consume_keyword(parser, "FROM"))
	{
		format_error(parser, "Expected FROM after DELETE");
		return;
	}

	tok token = lexer_next_token(&parser->lex);
	if (token.type != TOKEN_IDENTIFIER)
	{
		format_error(parser, "Expected table name after DELETE FROM");
		return;
	}

	stmt->table_name = token.text;

	stmt->where_clause = parse_where_clause(parser);
}

void
parse_create_table(parser *parser, create_table_stmt *stmt)
{
	if (!consume_keyword(parser, "CREATE"))
	{
		format_error(parser, "Expected CREATE");
		return;
	}

	if (!consume_keyword(parser, "TABLE"))
	{
		format_error(parser, "Expected TABLE after CREATE");
		return;
	}

	tok token = lexer_next_token(&parser->lex);
	if (token.type != TOKEN_IDENTIFIER)
	{
		format_error(parser, "Expected table name after CREATE TABLE");
		return;
	}

	stmt->table_name = token.text;

	if (!consume_token(parser, TOKEN_LPAREN))
	{
		format_error(parser, "Expected '(' after table name");
		return;
	}

	do
	{
		token = lexer_next_token(&parser->lex);
		if (token.type != TOKEN_IDENTIFIER)
		{
			format_error(parser, "Expected column name in CREATE TABLE");
			return;
		}

		attribute_node col = {};
		col.name = token.text;

		col.type = parse_data_type(parser);
		if (col.type == TYPE_NULL)
		{
			return;
		}

		// First column is implicitly primary key
		if (stmt->columns.size() == 0)
		{
			col.sem.is_primary_key = true;
		}

		stmt->columns.push(col);
	} while (consume_token(parser, TOKEN_COMMA));

	if (!consume_token(parser, TOKEN_RPAREN))
	{
		format_error(parser, "Expected ')' after column definitions");
		return;
	}

	if (stmt->columns.size() == 0)
	{
		format_error(parser, "Table must have at least one column");
		return;
	}
}

void
parse_drop_table(parser *parser, drop_table_stmt *stmt)
{
	if (!consume_keyword(parser, "DROP"))
	{
		format_error(parser, "Expected DROP");
		return;
	}

	if (!consume_keyword(parser, "TABLE"))
	{
		format_error(parser, "Expected TABLE after DROP");
		return;
	}

	tok token = lexer_next_token(&parser->lex);
	if (token.type != TOKEN_IDENTIFIER)
	{
		format_error(parser, "Expected table name after DROP TABLE");
		return;
	}

	stmt->table_name = token.text;
}

void
parse_begin(parser *parser, begin_stmt *stmt)
{
	if (!consume_keyword(parser, "BEGIN"))
	{
		format_error(parser, "Expected BEGIN");
		return;
	}
}

void
parse_commit(parser *parser, commit_stmt *stmt)
{
	if (!consume_keyword(parser, "COMMIT"))
	{
		format_error(parser, "Expected COMMIT");
		return;
	}
}

void
parse_rollback(parser *parser, rollback_stmt *stmt)
{
	if (!consume_keyword(parser, "ROLLBACK"))
	{
		format_error(parser, "Expected ROLLBACK");
		return;
	}
}

stmt_node *
parse_statement(parser *parser)
{
	stmt_node *stmt = (stmt_node *)arena<query_arena>::alloc(sizeof(stmt_node));

	tok token = lexer_peek_token(&parser->lex);
	const char *stmt_start = parser->lex.current;

	if (peek_keyword(parser, "SELECT"))
	{
		stmt->type = STMT_SELECT;
		parse_select(parser, &stmt->select_stmt);
	}
	else if (peek_keyword(parser, "UPDATE"))
	{
		stmt->type = STMT_UPDATE;
		parse_update(parser, &stmt->update_stmt);
	}
	else if (peek_keyword(parser, "DELETE"))
	{
		stmt->type = STMT_DELETE;
		parse_delete(parser, &stmt->delete_stmt);
	}
	else if (peek_keyword(parser, "CREATE"))
	{
		stmt->type = STMT_CREATE_TABLE;
		parse_create_table(parser, &stmt->create_table_stmt);
	}
	else if (peek_keyword(parser, "INSERT"))
	{
		stmt->type = STMT_INSERT;
		parse_insert(parser, &stmt->insert_stmt);
	}
	else if (peek_keyword(parser, "DROP"))
	{
		stmt->type = STMT_DROP_TABLE;
		parse_drop_table(parser, &stmt->drop_table_stmt);
	}
	else if (peek_keyword(parser, "BEGIN"))
	{
		stmt->type = STMT_BEGIN;
		parse_begin(parser, &stmt->begin_stmt);
	}
	else if (peek_keyword(parser, "COMMIT"))
	{
		stmt->type = STMT_COMMIT;
		parse_commit(parser, &stmt->commit_stmt);
	}
	else if (peek_keyword(parser, "ROLLBACK"))
	{
		stmt->type = STMT_ROLLBACK;
		parse_rollback(parser, &stmt->rollback_stmt);
	}
	else
	{
		if (token.type == TOKEN_EOF)
		{
			format_error(parser, "Unexpected end of input");
		}
		else
		{
			format_error(parser, "Unexpected token '%.*s' - expected SQL statement", (int)token.text.size(),
						 token.text.data());
		}
		return nullptr;
	}

	if (!parser->error_msg.empty())
	{
		return nullptr;
	}

    stmt->sql_stmt = string_view(stmt_start, parser->lex.current - stmt_start);

	consume_token(parser, TOKEN_SEMICOLON);

	return stmt;
}

array<stmt_node *, query_arena>
parse_statements(parser *parser)
{
	array<stmt_node *, query_arena> statements;
	statements.clear();

	while (true)
	{
		if (lexer_peek_token(&parser->lex).type == TOKEN_EOF)
		{
			break;
		}

		stmt_node *stmt = parse_statement(parser);
		if (!stmt)
		{
			return statements;
		}

		statements.push(stmt);
	}

	return statements;
}

parser_result
parse_sql(const char *sql)
{
	parser_result result;
	parser		  parser;
	lexer_init(&parser.lex, sql);

	parser.error_msg = string_view{};
	parser.error_line = -1;
	parser.error_column = -1;

	result.statements = parse_statements(&parser);

	if (!parser.error_msg.empty())
	{
		result.success = false;
		result.error = parser.error_msg;
		result.error_line = parser.error_line;
		result.error_column = parser.error_column;
		result.failed_statement_index = result.statements.size();
	}
	else
	{
		result.success = true;
		result.error = {};
		result.error_line = -1;
		result.error_column = -1;
		result.failed_statement_index = -1;
	}

	return result;
}

// Debug printing functions
static void
print_expr(expr_node *expr, int indent)
{
	if (!expr)
	{
		printf("%*s<null>\n", indent, "");
		return;
	}

	switch (expr->type)
	{
	case EXPR_LITERAL:
		if (expr->lit_type == TYPE_U32)
		{
			printf("%*sLiteral(INT): %u\n", indent, "", expr->int_val);
		}
		else
		{
			printf("%*sLiteral(TEXT): '%.*s'\n", indent, "", (int)expr->str_val.size(), expr->str_val.data());
		}
		break;

	case EXPR_COLUMN:
		printf("%*sColumn: %.*s\n", indent, "", (int)expr->column_name.size(), expr->column_name.data());
		break;

	case EXPR_BINARY_OP: {
		const char *op_str[] = {"=", "!=", "<", "<=", ">", ">=", "AND", "OR"};
		printf("%*sBinaryOp: %s\n", indent, "", op_str[expr->op]);
		print_expr(expr->left, indent + 2);
		print_expr(expr->right, indent + 2);
		break;
	}

	case EXPR_UNARY_OP: {
		const char *op_str[] = {"NOT", "NEG"};
		printf("%*sUnaryOp: %s\n", indent, "", op_str[expr->unary_op]);
		print_expr(expr->operand, indent + 2);
		break;
	}
	}
}

void
print_ast(stmt_node *stmt)
{
	if (!stmt)
	{
		printf("NULL statement\n");
		return;
	}

	printf("Statement Type: %s\n", stmt_type_to_string(stmt->type));

	switch (stmt->type)
	{
	case STMT_SELECT: {
		select_stmt *s = &stmt->select_stmt;
		printf("  Table: %.*s\n", (int)s->table_name.size(), s->table_name.data());

		if (s->is_star)
		{
			printf("  Columns: *\n");
		}
		else
		{
			printf("  Columns: ");
			for (uint32_t i = 0; i < s->columns.size(); i++)
			{
				if (i > 0)
					printf(", ");
				printf("%.*s", (int)s->columns[i].size(), s->columns[i].data());
			}
			printf("\n");
		}

		if (s->where_clause)
		{
			printf("  WHERE:\n");
			print_expr(s->where_clause, 4);
		}

		if (!s->order_by_column.empty())
		{
			printf("  ORDER BY: %.*s %s\n", (int)s->order_by_column.size(), s->order_by_column.data(),
				   s->order_desc ? "DESC" : "ASC");
		}
		break;
	}

	case STMT_INSERT: {
		insert_stmt *s = &stmt->insert_stmt;
		printf("  Table: %.*s\n", (int)s->table_name.size(), s->table_name.data());

		if (s->columns.size() > 0)
		{
			printf("  Columns: ");
			for (uint32_t i = 0; i < s->columns.size(); i++)
			{
				if (i > 0)
					printf(", ");
				printf("%.*s", (int)s->columns[i].size(), s->columns[i].data());
			}
			printf("\n");
		}

		printf("  Values:\n");
		for (uint32_t i = 0; i < s->values.size(); i++)
		{
			print_expr(s->values[i], 4);
		}
		break;
	}

	case STMT_UPDATE: {
		update_stmt *s = &stmt->update_stmt;
		printf("  Table: %.*s\n", (int)s->table_name.size(), s->table_name.data());
		printf("  SET:\n");
		for (uint32_t i = 0; i < s->columns.size(); i++)
		{
			printf("    %.*s = ", (int)s->columns[i].size(), s->columns[i].data());
			print_expr(s->values[i], 0);
		}
		if (s->where_clause)
		{
			printf("  WHERE:\n");
			print_expr(s->where_clause, 4);
		}
		break;
	}

	case STMT_DELETE: {
		delete_stmt *s = &stmt->delete_stmt;
		printf("  Table: %.*s\n", (int)s->table_name.size(), s->table_name.data());
		if (s->where_clause)
		{
			printf("  WHERE:\n");
			print_expr(s->where_clause, 4);
		}
		break;
	}

	case STMT_CREATE_TABLE: {
		create_table_stmt *s = &stmt->create_table_stmt;
		printf("  Table: %.*s\n", (int)s->table_name.size(), s->table_name.data());
		printf("  Columns:\n");
		for (uint32_t i = 0; i < s->columns.size(); i++)
		{
			attribute_node *col = &s->columns[i];
			printf("    %.*s %s%s\n", (int)col->name.size(), col->name.data(), col->type == TYPE_U32 ? "INT" : "TEXT",
				   col->sem.is_primary_key ? " (PRIMARY KEY)" : "");
		}
		break;
	}

	case STMT_DROP_TABLE: {
		drop_table_stmt *s = &stmt->drop_table_stmt;
		printf("  Table: %.*s\n", (int)s->table_name.size(), s->table_name.data());
		break;
	}

	case STMT_BEGIN:
	case STMT_COMMIT:
	case STMT_ROLLBACK:
		// No additional info
		break;
	}
}
