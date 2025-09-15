/*
 * SQL From Scratch - Educational Database Engine
 *
 * Runtime Type System
 *
 *
 * All of the computation we do with the VM needs runtime info about the
 * actual types it's processing.
 *
 * We handle types dynamically because:
 * - Schema is loaded at runtime from the catalog
 * - Query plans are generated dynamically
 * - Intermediate results may have computed types (e.g., SUM(price) * 1.1)
 *
 * For example
 * 'WHERE user_id > 5;'
 *      OP_Test:
 *		typed_value *a = &VM.registers[left];   // 5
 *		typed_value *b = &VM.registers[right];  // user_id of row x
 *		int cmp_result = type_compare(a->type, a->data, b->data); // compare dispatch
 *
 * The type_compare function uses the type tag to dispatch to the correct
 * comparison function (int vs float vs string comparison).
 *
 * Real SQL engines need composite keys for multi-column operations:
 * - PRIMARY KEY (company_id, employee_id)
 * - ORDER BY age, username
 * - GROUP BY category, subcategory
 *
 * Instead of handling tuples of values, we pack multiple values into a single
 * "dual type" that maintains comparison semantics:
 *
 * ORDER BY age, username:
 * [u32][4 bytes] + [char16][16 bytes] -> [dual][20 bytes total]
 *
 * Comparison works lexicographically:
 * (20, "John") < (20, "Jane") = false  // Same age, John > Jane alphabetically
 * (20, "John") < (21, "Alice") = true  // Lower age wins regardless of name
 *
 *
 * We use a 64-bit type descriptor that encodes both type identity and size:
 *
 * Single types: [type_id:8][unused:32][size:24]
 *               ^^^^^^^^   ^^^^^^^^^^  ^^^^^^^^
 *               type tag   wasted      up to 16MB values
 *
 * Dual types:   [TYPE_ID_DUAL:8][type1:8][type2:8][size1:8][size2:8][total:24]
 *               ^^^^^^^^^^^^^^^  ^^^^^^^^ ^^^^^^^^ ^^^^^^^^ ^^^^^^^^ ^^^^^^^^^^
 *               dual marker      first    second   sizes    sizes    total size
 *
 * We heap allocate all values, including those <= 64 bits, but all on the arenas
 */

#pragma once
#include <cstdint>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "common.hpp"

typedef uint64_t data_type;

enum TYPE_ID : uint8_t
{
	TYPE_ID_U8 = 0x01,
	TYPE_ID_U16 = 0x02,
	TYPE_ID_U32 = 0x03,
	TYPE_ID_U64 = 0x04,

	TYPE_ID_I8 = 0x11,
	TYPE_ID_I16 = 0x12,
	TYPE_ID_I32 = 0x13,
	TYPE_ID_I64 = 0x14,

	TYPE_ID_F32 = 0x21,
	TYPE_ID_F64 = 0x22,

	TYPE_ID_CHAR = 0x31,	// Fixed-size string
	TYPE_ID_VARCHAR = 0x32, // Variable-size string

	TYPE_ID_DUAL = 0x40, // Dual type (pair of any two types)

	TYPE_ID_NULL = 0xFF
};

#define MAKE_TYPE(id, size) (((uint64_t)(id) << 56) | ((uint64_t)(size) & 0xFFFFFF))

#define MAKE_DUAL_TYPE(type1_id, type2_id, size1, size2)                                                               \
	(((uint64_t)(TYPE_ID_DUAL) << 56) | ((uint64_t)(type1_id) << 48) | ((uint64_t)(type2_id) << 40) |                  \
	 ((uint64_t)(size1) << 32) | ((uint64_t)(size2) << 24) | ((uint64_t)((size1) + (size2)) & 0xFFFFFF))

#define TYPE_U8	 MAKE_TYPE(TYPE_ID_U8, 1)
#define TYPE_U16 MAKE_TYPE(TYPE_ID_U16, 2)
#define TYPE_U32 MAKE_TYPE(TYPE_ID_U32, 4)
#define TYPE_U64 MAKE_TYPE(TYPE_ID_U64, 8)

#define TYPE_I8	 MAKE_TYPE(TYPE_ID_I8, 1)
#define TYPE_I16 MAKE_TYPE(TYPE_ID_I16, 2)
#define TYPE_I32 MAKE_TYPE(TYPE_ID_I32, 4)
#define TYPE_I64 MAKE_TYPE(TYPE_ID_I64, 8)

#define TYPE_F32 MAKE_TYPE(TYPE_ID_F32, 4)
#define TYPE_F64 MAKE_TYPE(TYPE_ID_F64, 8)

// Fixed-size strings
#define TYPE_CHAR8	 MAKE_TYPE(TYPE_ID_CHAR, 8)
#define TYPE_CHAR16	 MAKE_TYPE(TYPE_ID_CHAR, 16)
#define TYPE_CHAR32	 MAKE_TYPE(TYPE_ID_CHAR, 32)
#define TYPE_CHAR64	 MAKE_TYPE(TYPE_ID_CHAR, 64)
#define TYPE_CHAR128 MAKE_TYPE(TYPE_ID_CHAR, 128)
#define TYPE_CHAR256 MAKE_TYPE(TYPE_ID_CHAR, 256)

#define TYPE_NULL MAKE_TYPE(TYPE_ID_NULL, 0)

// VARCHAR with runtime size
#define TYPE_VARCHAR(len) MAKE_TYPE(TYPE_ID_VARCHAR, (len))

data_type
make_char(uint32_t size);
data_type
make_varchar(uint32_t size);

uint32_t
type_size(data_type type);
uint8_t
type_id(data_type type);

uint8_t
dual_type_id_1(data_type type);
uint8_t
dual_type_id_2(data_type type);

data_type
type_from_id_and_size(uint8_t id, uint32_t size);
data_type
make_dual(data_type type1, data_type type2);
data_type
dual_component_type(data_type type, uint32_t index);
uint32_t
dual_component_offset(data_type type, uint32_t index);

bool
type_is_string(data_type type);
bool
type_is_numeric(data_type type);
bool
type_is_null(data_type type);
bool
type_is_dual(data_type type);

int
type_compare(data_type type, const void *a, const void *b);
bool
type_compare_op(COMPARISON_OP op, data_type type, const void *a, const void *b);

inline bool
type_greater_than(data_type type, const void *a, const void *b)
{
	return type_compare(type, a, b) > 0;
}

inline bool
type_greater_equal(data_type type, const void *a, const void *b)
{
	return type_compare(type, a, b) >= 0;
}

inline bool
type_less_than(data_type type, const void *a, const void *b)
{
	return type_compare(type, a, b) < 0;
}

inline bool
type_less_equal(data_type type, const void *a, const void *b)
{
	return type_compare(type, a, b) <= 0;
}

inline bool
type_equals(data_type type, const void *a, const void *b)
{
	return type_compare(type, a, b) == 0;
}

inline bool
type_not_equals(data_type type, const void *a, const void *b)
{
	return type_compare(type, a, b) != 0;
}

#define DECLARE_ARITHMETIC_OP(name) void type_##name(data_type type, void *dst, const void *a, const void *b);

DECLARE_ARITHMETIC_OP(add)
DECLARE_ARITHMETIC_OP(sub)
DECLARE_ARITHMETIC_OP(mul)
DECLARE_ARITHMETIC_OP(div)

void
type_copy(data_type type, void *dst, const void *src);
void
type_zero(data_type type, void *dst);

void
type_print(data_type type, const void *data);
void
type_increment(data_type type, void *dst, const void *src);
const char *
type_name(data_type type);

void
pack_dual(void *dest, data_type type1, const void *data1, data_type type2, const void *data2);
void
unpack_dual(data_type dual_type, const void *src, void *data1, void *data2);

struct typed_value
{
	void	 *data;
	data_type type;

	uint8_t
	get_type_id() const;
	uint32_t
	get_size() const;
	bool
	is_dual() const;

	bool
	is_numeric() const;
	bool
	is_string() const;
	bool
	is_null() const;

	void
	set_varchar(const char *str, uint32_t len = 0);

	int
	compare(const typed_value &other) const;
	bool
	operator>(const typed_value &other) const;
	bool
	operator>=(const typed_value &other) const;
	bool
	operator<(const typed_value &other) const;
	bool
	operator<=(const typed_value &other) const;
	bool
	operator==(const typed_value &other) const;
	bool
	operator!=(const typed_value &other) const;

	void
	copy_to(typed_value &dst) const;
	void
	print() const;
	uint16_t
	size() const;
	const char *
	name() const;

	static typed_value
	make(data_type type, void *data = nullptr);

	uint8_t
	as_u8() const;
	uint16_t
	as_u16() const;
	uint32_t
	as_u32() const;
	uint64_t
	as_u64() const;

	int8_t
	as_i8() const;
	int16_t
	as_i16() const;
	int32_t
	as_i32() const;
	int64_t
	as_i64() const;

	float
	as_f32() const;
	double
	as_f64() const;

	const char *
	as_char() const;
	const char *
	as_varchar() const;
};
