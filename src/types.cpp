/*
 * SQL From Scratch - Educational Database Engine
 *
 * Runtime Type System
 */

#include "types.hpp"
#include <cassert>
#include <cstring>
#include <string.h>
#include <inttypes.h>

data_type
make_char(uint32_t size)
{
	return MAKE_TYPE(TYPE_ID_CHAR, size);
}

data_type
make_varchar(uint32_t size)
{
	return MAKE_TYPE(TYPE_ID_VARCHAR, size);
}

uint32_t
type_size(data_type type)
{
	return type & 0xFFFFFF;
}

uint8_t
type_id(data_type type)
{
	return type >> 56;
}

uint8_t
dual_type_id_1(data_type type)
{
	return (type >> 48) & 0xFF;
}

uint8_t
dual_type_id_2(data_type type)
{
	return (type >> 40) & 0xFF;
}

static uint8_t
dual_size_1(data_type type)
{
	return (type >> 32) & 0xFF;
}

static uint8_t
dual_size_2(data_type type)
{
	return (type >> 24) & 0xFF;
}

data_type
type_from_id_and_size(uint8_t id, uint32_t size)
{
	switch (id)
	{
	case TYPE_ID_U8:
		return TYPE_U8;
	case TYPE_ID_U16:
		return TYPE_U16;
	case TYPE_ID_U32:
		return TYPE_U32;
	case TYPE_ID_U64:
		return TYPE_U64;

	case TYPE_ID_I8:
		return TYPE_I8;
	case TYPE_ID_I16:
		return TYPE_I16;
	case TYPE_ID_I32:
		return TYPE_I32;
	case TYPE_ID_I64:
		return TYPE_I64;

	case TYPE_ID_F32:
		return TYPE_F32;
	case TYPE_ID_F64:
		return TYPE_F64;

	case TYPE_ID_CHAR:
		return make_char(size);
	case TYPE_ID_VARCHAR:
		return make_varchar(size);

	default:
		return TYPE_NULL;
	}
}

data_type
make_dual(data_type type1, data_type type2)
{
	uint8_t	 id1 = type_id(type1);
	uint8_t	 id2 = type_id(type2);
	uint32_t size1 = type_size(type1);
	uint32_t size2 = type_size(type2);
	return MAKE_DUAL_TYPE(id1, id2, size1, size2);
}

data_type
dual_component_type(data_type type, uint32_t index)
{
	assert(type_id(type) == TYPE_ID_DUAL);
	if (index == 0)
	{
		return type_from_id_and_size(dual_type_id_1(type), dual_size_1(type));
	}
	else if (index == 1)
	{
		return type_from_id_and_size(dual_type_id_2(type), dual_size_2(type));
	}
	return TYPE_NULL;
}

uint32_t
dual_component_offset(data_type type, uint32_t index)
{
	if (index == 0)
	{
		return 0;
	}

	if (index == 1)
	{
		return type_size(dual_component_type(type, 0));
	}

	return 0;
}

bool
type_is_string(data_type type)
{
	uint8_t id = type_id(type);
	return id == TYPE_ID_CHAR || id == TYPE_ID_VARCHAR;
}
bool
type_is_dual(data_type type)
{
	return type_id(type) == TYPE_ID_DUAL;
}
bool
type_is_null(data_type type)
{
	return type_id(type) == TYPE_ID_NULL;
}

bool
type_is_numeric(data_type type)
{
	uint8_t id = type_id(type);
	return id <= TYPE_ID_F64;
}

int
type_compare(data_type type, const void *a, const void *b)
{
	uint8_t tid = type_id(type);

	switch (tid)
	{
	case TYPE_ID_U8: {
		uint8_t av = *(uint8_t *)a, bv = *(uint8_t *)b;
		return (av > bv) - (av < bv);
	}
	case TYPE_ID_U16: {
		uint16_t av = *(uint16_t *)a, bv = *(uint16_t *)b;
		return (av > bv) - (av < bv);
	}
	case TYPE_ID_U32: {
		uint32_t av = *(uint32_t *)a, bv = *(uint32_t *)b;
		return (av > bv) - (av < bv);
	}
	case TYPE_ID_U64: {
		uint64_t av = *(uint64_t *)a, bv = *(uint64_t *)b;
		return (av > bv) - (av < bv);
	}

	case TYPE_ID_I8: {
		int8_t av = *(int8_t *)a, bv = *(int8_t *)b;
		return (av > bv) - (av < bv);
	}
	case TYPE_ID_I16: {
		int16_t av = *(int16_t *)a, bv = *(int16_t *)b;
		return (av > bv) - (av < bv);
	}
	case TYPE_ID_I32: {
		int32_t av = *(int32_t *)a, bv = *(int32_t *)b;
		return (av > bv) - (av < bv);
	}
	case TYPE_ID_I64: {
		int64_t av = *(int64_t *)a, bv = *(int64_t *)b;
		return (av > bv) - (av < bv);
	}

	case TYPE_ID_F32: {
		float av = *(float *)a, bv = *(float *)b;
		return (av > bv) - (av < bv);
	}
	case TYPE_ID_F64: {
		double av = *(double *)a, bv = *(double *)b;
		return (av > bv) - (av < bv);
	}

	case TYPE_ID_CHAR:
	case TYPE_ID_VARCHAR:
		return strcmp((char *)a, (char *)b);

	case TYPE_ID_DUAL: {
		// Compare first component
		data_type type1 = dual_component_type(type, 0);
		int		  cmp1 = type_compare(type1, a, b);
		if (cmp1 != 0)
			return cmp1;

		// Compare second component if first is equal
		data_type type2 = dual_component_type(type, 1);
		uint32_t  offset = type_size(type1);
		return type_compare(type2, (char *)a + offset, (char *)b + offset);
	}

	default:
		return 0;
	}
}

bool
type_compare_op(COMPARISON_OP op, data_type type, const void *a, const void *b)
{
	switch (op)
	{
	case EQ:
		return type_equals(type, a, b);
	case NE:
		return type_not_equals(type, a, b);
	case LT:
		return type_less_than(type, a, b);
	case LE:
		return type_less_equal(type, a, b);
	case GT:
		return type_greater_than(type, a, b);
	case GE:
		return type_greater_equal(type, a, b);
	}
	return false;
}

#define DEFINE_ARITHMETIC_OP(name, op)                                                                                 \
	void type_##name(data_type type, void *dst, const void *a, const void *b)                                          \
	{                                                                                                                  \
		switch (type_id(type))                                                                                         \
		{                                                                                                              \
		case TYPE_ID_U8:                                                                                               \
			*(uint8_t *)dst = *(uint8_t *)a op * (uint8_t *)b;                                                         \
			break;                                                                                                     \
		case TYPE_ID_U16:                                                                                              \
			*(uint16_t *)dst = *(uint16_t *)a op * (uint16_t *)b;                                                      \
			break;                                                                                                     \
		case TYPE_ID_U32:                                                                                              \
			*(uint32_t *)dst = *(uint32_t *)a op * (uint32_t *)b;                                                      \
			break;                                                                                                     \
		case TYPE_ID_U64:                                                                                              \
			*(uint64_t *)dst = *(uint64_t *)a op * (uint64_t *)b;                                                      \
			break;                                                                                                     \
		case TYPE_ID_I8:                                                                                               \
			*(int8_t *)dst = *(int8_t *)a op * (int8_t *)b;                                                            \
			break;                                                                                                     \
		case TYPE_ID_I16:                                                                                              \
			*(int16_t *)dst = *(int16_t *)a op * (int16_t *)b;                                                         \
			break;                                                                                                     \
		case TYPE_ID_I32:                                                                                              \
			*(int32_t *)dst = *(int32_t *)a op * (int32_t *)b;                                                         \
			break;                                                                                                     \
		case TYPE_ID_I64:                                                                                              \
			*(int64_t *)dst = *(int64_t *)a op * (int64_t *)b;                                                         \
			break;                                                                                                     \
		case TYPE_ID_F32:                                                                                              \
			*(float *)dst = *(float *)a op * (float *)b;                                                               \
			break;                                                                                                     \
		case TYPE_ID_F64:                                                                                              \
			*(double *)dst = *(double *)a op * (double *)b;                                                            \
			break;                                                                                                     \
		}                                                                                                              \
	}

DEFINE_ARITHMETIC_OP(add, +)
DEFINE_ARITHMETIC_OP(sub, -)
DEFINE_ARITHMETIC_OP(mul, *)
DEFINE_ARITHMETIC_OP(div, /)

void
type_copy(data_type type, void *dst, const void *src)
{
	if (type_is_string(type))
	{
		strcpy((char *)dst, (char *)src);
	}
	else
	{
		memcpy(dst, src, type_size(type));
	}
}

void
type_zero(data_type type, void *dst)
{
	memset(dst, 0, type_size(type));
}

void
type_print(data_type type, const void *data)
{
	switch (type_id(type))
	{
	case TYPE_ID_NULL:
		printf("NULL");
		break;

	case TYPE_ID_U8:
		printf("%" PRIu8, *(uint8_t *)data);
		break;
	case TYPE_ID_U16:
		printf("%" PRIu16, *(uint16_t *)data);
		break;
	case TYPE_ID_U32:
		printf("%" PRIu32, *(uint32_t *)data);
		break;
	case TYPE_ID_U64:
		printf("%" PRIu64, *(uint64_t *)data);
		break;

	case TYPE_ID_I8:
		printf("%" PRId8, *(int8_t *)data);
		break;
	case TYPE_ID_I16:
		printf("%" PRId16, *(int16_t *)data);
		break;
	case TYPE_ID_I32:
		printf("%" PRId32, *(int32_t *)data);
		break;
	case TYPE_ID_I64:
		printf("%" PRId64, *(int64_t *)data);
		break;

	case TYPE_ID_F32:
		printf("%g", *(float *)data);
		break;
	case TYPE_ID_F64:
		printf("%g", *(double *)data);
		break;

	case TYPE_ID_CHAR: {
		const char *str = (const char *)data;
		printf("%s", str ? str : "NULL");
		break;
	}
	case TYPE_ID_VARCHAR:
		printf("%s", (const char *)data);
		break;

	case TYPE_ID_DUAL: {
		printf("(");

		// Print first component
		data_type type1 = dual_component_type(type, 0);
		type_print(type1, data);

		printf(", ");

		// Print second component
		data_type type2 = dual_component_type(type, 1);
		uint32_t  offset = type_size(type1);
		type_print(type2, (char *)data + offset);

		printf(")");
		break;
	}
	}
}

// for auto incrementing primary keys
void
type_increment(data_type type, void *dst, const void *src)
{
	uint8_t tid = type_id(type);

	switch (tid)
	{
	case TYPE_ID_U8:
		*(uint8_t *)dst = *(uint8_t *)src + 1;
		break;
	case TYPE_ID_U16:
		*(uint16_t *)dst = *(uint16_t *)src + 1;
		break;
	case TYPE_ID_U32:
		*(uint32_t *)dst = *(uint32_t *)src + 1;
		break;
	case TYPE_ID_U64:
		*(uint64_t *)dst = *(uint64_t *)src + 1;
		break;

	case TYPE_ID_I8:
		*(int8_t *)dst = *(int8_t *)src + 1;
		break;
	case TYPE_ID_I16:
		*(int16_t *)dst = *(int16_t *)src + 1;
		break;
	case TYPE_ID_I32:
		*(int32_t *)dst = *(int32_t *)src + 1;
		break;
	case TYPE_ID_I64:
		*(int64_t *)dst = *(int64_t *)src + 1;
		break;

	case TYPE_ID_F32:
		*(float *)dst = *(float *)src + 1.0f;
		break;
	case TYPE_ID_F64:
		*(double *)dst = *(double *)src + 1.0;
		break;

	case TYPE_ID_CHAR:
	case TYPE_ID_VARCHAR:
		// unimplemented
		assert(false);
	case TYPE_ID_DUAL: {
		data_type type1 = dual_component_type(type, 0);
		type_increment(type1, dst, src);

		data_type type2 = dual_component_type(type, 1);
		uint32_t  offset = type_size(type1);
		type_increment(type2, (char *)dst + offset, (char *)src + offset);
		break;
	}

	default:
		assert(false);
	}
}

const char *
type_name(data_type type)
{
	static char buf[64];

	switch (type_id(type))
	{
	case TYPE_ID_U8:
		return "U8";
	case TYPE_ID_U16:
		return "U16";
	case TYPE_ID_U32:
		return "U32";
	case TYPE_ID_U64:
		return "U64";

	case TYPE_ID_I8:
		return "I8";
	case TYPE_ID_I16:
		return "I16";
	case TYPE_ID_I32:
		return "I32";
	case TYPE_ID_I64:
		return "I64";

	case TYPE_ID_F32:
		return "F32";
	case TYPE_ID_F64:
		return "F64";

	case TYPE_ID_CHAR: {
		uint32_t size = type_size(type);
		snprintf(buf, sizeof(buf), "CHAR%u", size);
		return buf;
	}

	case TYPE_ID_VARCHAR: {
		uint32_t size = type_size(type);
		snprintf(buf, sizeof(buf), "VARCHAR(%u)", size);
		return buf;
	}

	case TYPE_ID_DUAL: {
		data_type type1 = dual_component_type(type, 0);
		data_type type2 = dual_component_type(type, 1);
		snprintf(buf, sizeof(buf), "DUAL(%s,%s)", type_name(type1), type_name(type2));
		return buf;
	}

	case TYPE_ID_NULL:
		return "NULL";
	default:
		return "UNKNOWN";
	}
}

void
pack_dual(void *dest, data_type type1, const void *data1, data_type type2, const void *data2)
{
	type_copy(type1, dest, data1);
	type_copy(type2, (char *)dest + type_size(type1), data2);
}

void
unpack_dual(data_type dual_type, const void *src, void *data1, void *data2)
{
	data_type type1 = dual_component_type(dual_type, 0);
	data_type type2 = dual_component_type(dual_type, 1);

	type_copy(type1, data1, src);
	type_copy(type2, data2, (char *)src + type_size(type1));
}

uint8_t
typed_value::get_type_id() const
{
	return type_id(type);
}

uint32_t
typed_value::get_size() const
{
	return type_size(type);
}

bool
typed_value::is_dual() const
{
	return type_is_dual(type);
}

bool
typed_value::is_numeric() const
{
	return type_is_numeric(type);
}

bool
typed_value::is_string() const
{
	return type_is_string(type);
}
bool
typed_value::is_null() const
{
	return type_is_null(type);
}

void
typed_value::set_varchar(const char *str, uint32_t len)
{
	len = len ? len : strlen(str);
	type = TYPE_VARCHAR(len);
	data = (void *)str;
}

int
typed_value::compare(const typed_value &other) const
{
	return type_compare(type, data, other.data);
}

bool
typed_value::operator>(const typed_value &other) const
{
	return compare(other) > 0;
}

bool
typed_value::operator>=(const typed_value &other) const
{
	return compare(other) >= 0;
}

bool
typed_value::operator<(const typed_value &other) const
{
	return compare(other) < 0;
}

bool
typed_value::operator<=(const typed_value &other) const
{
	return compare(other) <= 0;
}

bool
typed_value::operator==(const typed_value &other) const
{
	return type_equals(type, data, other.data);
}

bool
typed_value::operator!=(const typed_value &other) const
{
	return !type_equals(type, data, other.data);
}

void
typed_value::copy_to(typed_value &dst) const
{
	dst.type = type;
	type_copy(type, dst.data, data);
}

void
typed_value::print() const
{
	type_print(type, data);
}

uint16_t
typed_value::size() const
{
	return type_size(this->type);
}

const char *
typed_value::name() const
{
	return type_name(type);
}

typed_value
typed_value::make(data_type type, void *data)
{
	return {data, type};
}

uint8_t
typed_value::as_u8() const
{
	return *reinterpret_cast<uint8_t *>(data);
}

uint16_t
typed_value::as_u16() const
{
	return *reinterpret_cast<uint16_t *>(data);
}

uint32_t
typed_value::as_u32() const
{
	return *reinterpret_cast<uint32_t *>(data);
}

uint64_t
typed_value::as_u64() const
{
	return *reinterpret_cast<uint64_t *>(data);
}

int8_t
typed_value::as_i8() const
{
	return *reinterpret_cast<int8_t *>(data);
}

int16_t
typed_value::as_i16() const
{
	return *reinterpret_cast<int16_t *>(data);
}

int32_t
typed_value::as_i32() const
{
	return *reinterpret_cast<int32_t *>(data);
}

int64_t
typed_value::as_i64() const
{
	return *reinterpret_cast<int64_t *>(data);
}

float
typed_value::as_f32() const
{
	return *reinterpret_cast<float *>(data);
}

double
typed_value::as_f64() const
{
	return *reinterpret_cast<double *>(data);
}

const char *
typed_value::as_char() const
{
	return reinterpret_cast<const char *>(data);
}

const char *
typed_value::as_varchar() const
{
	return reinterpret_cast<const char *>(data);
}
