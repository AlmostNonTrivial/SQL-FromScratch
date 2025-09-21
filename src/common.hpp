/*
 * SQL From Scratch
 *
 */

#pragma once

#pragma once

#include <stdint.h>
#include <string_view>

#define PAGE_SIZE 4096 /* Keep this lower to see more btree splits when printing */
using std::string_view;

/*
 *  The 'per-query, or per user-input (multi-statement query)' arena
 *  that is reset after execution. All VM memory, including register allocation
 *  ephemeral tree nodes, as well as AST nodes and compiled programs are allocated
 *  to this arena.
 */
struct query_arena
{
};

enum ARITH_OP : uint8_t
{
	ARITH_ADD = 0,
	ARITH_SUB = 1,
	ARITH_MUL = 2,
	ARITH_DIV = 3
};

enum LOGIC_OP : uint8_t
{
	LOGIC_AND = 0,
	LOGIC_OR = 1,
};

enum COMPARISON_OP : uint8_t
{
	EQ = 0,
	NE = 1,
	LT = 2,
	LE = 3,
	GT = 4,
	GE = 5,
};

inline const char *
debug_compare_op_name(COMPARISON_OP op)
{
	const char *names[] = {"==", "!=", "<", "<=", ">", ">="};
	return (op >= EQ && op <= GE) ? names[op] : "UNKNOWN";
}

inline const char *
debug_arith_op_name(ARITH_OP op)
{
	const static char *names[] = {"+", "-", "*", "/"};
	return (op >= ARITH_ADD && op <= ARITH_DIV) ? names[op] : "UNKNOWN";
}

inline const char *
debug_logic_op_name(LOGIC_OP op)
{
	const static char *names[] = {"AND", "OR"};
	return (op >= LOGIC_AND && op <= LOGIC_OR) ? names[op] : "UNKNOWN";
}

inline void
sv_to_cstr(std::string_view sv, char *dst, int size)
{
	size_t len = sv.size();
	memcpy(dst, sv.data(), len);
	dst[len] = '\0';
}
