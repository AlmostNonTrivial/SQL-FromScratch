#include "../types.hpp"
#include "types.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cmath>

#define TEST_DB "test_types.db"

void
test_type_construction()
{
	data_type u32_type = TYPE_U32;
	assert(type_id(u32_type) == TYPE_ID_U32);
	assert(type_size(u32_type) == 4);

	data_type varchar_type = TYPE_VARCHAR(128);
	assert(type_id(varchar_type) == TYPE_ID_VARCHAR);
	assert(type_size(varchar_type) == 128);

	data_type char_type = make_char(64);
	assert(type_id(char_type) == TYPE_ID_CHAR);
	assert(type_size(char_type) == 64);

	data_type varchar_runtime = make_varchar(256);
	assert(type_id(varchar_runtime) == TYPE_ID_VARCHAR);
	assert(type_size(varchar_runtime) == 256);

	data_type dual_type = make_dual(TYPE_U32, TYPE_U32);
	assert(type_id(dual_type) == TYPE_ID_DUAL);
	assert(dual_type_id_1(dual_type) == TYPE_ID_U32);
	assert(dual_type_id_2(dual_type) == TYPE_ID_U32);
	assert(type_size(dual_type) == 8);

	data_type mixed_dual = make_dual(TYPE_U32, TYPE_U64);
	assert(type_id(mixed_dual) == TYPE_ID_DUAL);
	assert(type_size(mixed_dual) == 12);
}

void
test_type_checking()
{
	data_type dual_u32 = make_dual(TYPE_U32, TYPE_U32);
	data_type dual_i32 = make_dual(TYPE_I32, TYPE_I32);

	assert(type_is_string(TYPE_CHAR64));
	assert(type_is_string(TYPE_VARCHAR(100)));
	assert(!type_is_string(TYPE_I32));

	data_type dual_char = make_dual(TYPE_CHAR8, TYPE_CHAR8);
	assert(!type_is_string(dual_char));

	assert(type_is_numeric(TYPE_U32));
	assert(type_is_numeric(TYPE_I16));
	assert(type_is_numeric(TYPE_F64));
	assert(!type_is_numeric(TYPE_CHAR32));
	assert(!type_is_numeric(dual_u32));

	assert(type_is_dual(dual_u32));
	assert(type_is_dual(dual_i32));
	assert(!type_is_dual(TYPE_U32));
	assert(!type_is_dual(TYPE_CHAR16));
}

void
test_type_comparison()
{
	uint8_t u8_a = 10, u8_b = 20;
	assert(type_less_than(TYPE_U8, &u8_a, &u8_b));
	assert(!type_greater_than(TYPE_U8, &u8_a, &u8_b));
	assert(type_less_equal(TYPE_U8, &u8_a, &u8_b));

	int32_t i32_a = -5, i32_b = 10;
	assert(type_less_than(TYPE_I32, &i32_a, &i32_b));

	float f32_a = 3.14f, f32_b = 2.71f;
	assert(type_greater_than(TYPE_F32, &f32_a, &f32_b));

	char str1[] = "apple";
	char str2[] = "banana";
	assert(type_less_than(TYPE_CHAR64, str1, str2));
	assert(type_less_than(TYPE_VARCHAR(10), str1, str2));

	uint8_t	 comp1[8], comp2[8];
	uint32_t val1_a = 5, val1_b = 100;
	uint32_t val2_a = 5, val2_b = 200;
	pack_dual(comp1, TYPE_U32, &val1_a, TYPE_U32, &val1_b);
	pack_dual(comp2, TYPE_U32, &val2_a, TYPE_U32, &val2_b);

	data_type dual_type = make_dual(TYPE_U32, TYPE_U32);
	assert(type_less_than(dual_type, comp1, comp2));

	uint32_t val3_a = 6, val3_b = 50;
	pack_dual(comp2, TYPE_U32, &val3_a, TYPE_U32, &val3_b);
	assert(type_less_than(dual_type, comp1, comp2));

	uint16_t u16_x = 42, u16_y = 42;
	assert(type_equals(TYPE_U16, &u16_x, &u16_y));
	assert(!type_not_equals(TYPE_U16, &u16_x, &u16_y));
}

void
test_arithmetic_operations()
{
	uint32_t u32_a = 100, u32_b = 200, u32_result;
	type_add(TYPE_U32, &u32_result, &u32_a, &u32_b);
	assert(u32_result == 300);

	int16_t i16_a = -50, i16_b = 30, i16_result;
	type_add(TYPE_I16, &i16_result, &i16_a, &i16_b);
	assert(i16_result == -20);

	float f32_a = 2.5f, f32_b = 1.5f, f32_result;
	type_add(TYPE_F32, &f32_result, &f32_a, &f32_b);
	assert(fabs(f32_result - 4.0f) < 1e-6f);

	type_sub(TYPE_U32, &u32_result, &u32_b, &u32_a);
	assert(u32_result == 100);

	uint8_t u8_a = 5, u8_b = 4, u8_result;
	type_mul(TYPE_U8, &u8_result, &u8_a, &u8_b);
	assert(u8_result == 20);

	uint64_t u64_a = 100, u64_b = 4, u64_result;
	type_div(TYPE_U64, &u64_result, &u64_a, &u64_b);
	assert(u64_result == 25);
}

void
test_utility_operations()
{
	uint64_t src = 0x123456789ABCDEF0, dst = 0;
	type_copy(TYPE_U64, &dst, &src);
	assert(dst == src);

	char src_str[] = "hello world";
	char dst_str[64];
	type_copy(TYPE_CHAR64, dst_str, src_str);
	assert(strcmp(dst_str, src_str) == 0);

	uint8_t	 src_comp[8], dst_comp[8];
	uint32_t val_a = 12345, val_b = 67890;
	pack_dual(src_comp, TYPE_U32, &val_a, TYPE_U32, &val_b);

	data_type dual_type = make_dual(TYPE_U32, TYPE_U32);
	type_copy(dual_type, dst_comp, src_comp);

	uint32_t extracted_a, extracted_b;
	unpack_dual(dual_type, dst_comp, &extracted_a, &extracted_b);
	assert(extracted_a == 12345);
	assert(extracted_b == 67890);

	uint32_t val = 0xDEADBEEF;
	type_zero(TYPE_U32, &val);
	assert(val == 0);

	char str_val[32] = "test";
	type_zero(TYPE_CHAR32, str_val);
	assert(str_val[0] == 0);

	type_zero(dual_type, dst_comp);
	unpack_dual(dual_type, dst_comp, &extracted_a, &extracted_b);
	assert(extracted_a == 0);
	assert(extracted_b == 0);
}

void
test_typed_value()
{
	uint32_t	val = 42;
	typed_value tv = typed_value::make(TYPE_U32, &val);

	assert(tv.get_type_id() == TYPE_ID_U32);
	assert(tv.get_size() == 4);
	assert(tv.is_numeric());
	assert(!tv.is_string());
	assert(!tv.is_dual());

	uint32_t	val2 = 50;
	typed_value tv2 = typed_value::make(TYPE_U32, &val2);

	assert(tv < tv2);
	assert(tv <= tv2);
	assert(tv2 > tv);
	assert(tv2 >= tv);
	assert(tv != tv2);

	uint32_t	val3 = 42;
	typed_value tv3 = typed_value::make(TYPE_U32, &val3);
	assert(tv == tv3);
	assert(tv <= tv3);
	assert(tv >= tv3);

	uint8_t	 comp_data[8];
	uint32_t comp_a = 100, comp_b = 200;
	pack_dual(comp_data, TYPE_U32, &comp_a, TYPE_U32, &comp_b);

	data_type	dual_type = make_dual(TYPE_U32, TYPE_U32);
	typed_value comp_tv = typed_value::make(dual_type, comp_data);

	assert(comp_tv.is_dual());
	assert(!comp_tv.is_numeric());
	assert(!comp_tv.is_string());
	assert(comp_tv.get_size() == 8);

	char		str_data[] = "hello";
	typed_value str_tv = typed_value::make(TYPE_VARCHAR(10), str_data);

	assert(str_tv.is_string());
	assert(!str_tv.is_numeric());
	assert(!str_tv.is_dual());

	typed_value varchar_tv;
	char		varchar_data[] = "test string";
	varchar_tv.set_varchar(varchar_data);
	assert(varchar_tv.get_type_id() == TYPE_ID_VARCHAR);
	assert(varchar_tv.get_size() == strlen(varchar_data));
}

void
test_type_edge_cases()
{
	typed_value null_tv = typed_value::make(TYPE_NULL, nullptr);
	assert(null_tv.is_null());
	assert(null_tv.get_size() == 0);

	data_type max_varchar = TYPE_VARCHAR(65535);
	assert(type_size(max_varchar) == 65535);

	uint8_t zero_u8 = 0, nonzero_u8 = 1;
	assert(type_equals(TYPE_U8, &zero_u8, &zero_u8));
	assert(!type_equals(TYPE_U8, &zero_u8, &nonzero_u8));

	int32_t neg_a = -100, neg_b = -50;
	assert(type_less_than(TYPE_I32, &neg_a, &neg_b));

	float f_zero = 0.0f, f_neg_zero = -0.0f;
	assert(type_equals(TYPE_F32, &f_zero, &f_neg_zero));

	uint8_t	 comp_min[8], comp_max[8];
	uint32_t min_val = 0, max_val = 0xFFFFFFFF;
	pack_dual(comp_min, TYPE_U32, &min_val, TYPE_U32, &min_val);
	pack_dual(comp_max, TYPE_U32, &max_val, TYPE_U32, &max_val);

	data_type dual_type = make_dual(TYPE_U32, TYPE_U32);
	assert(type_less_than(dual_type, comp_min, comp_max));
}

void
test_comprehensive_arithmetic()
{
	uint8_t u8_max = 255, u8_one = 1, u8_overflow_result;
	type_add(TYPE_U8, &u8_overflow_result, &u8_max, &u8_one);

	uint32_t dividend = 1000;
	for (uint32_t divisor = 1; divisor <= 10; ++divisor)
	{
		uint32_t result;
		type_div(TYPE_U32, &result, &dividend, &divisor);
		assert(result == dividend / divisor);
	}

	double d_a = 1.0 / 3.0, d_b = 2.0 / 3.0, d_result;
	type_add(TYPE_F64, &d_result, &d_a, &d_b);
	assert(fabs(d_result - 1.0) < 1e-15);
}

void
test_dual_operations()
{
	data_type dual_type = make_dual(TYPE_U32, TYPE_U64);

	data_type comp1 = dual_component_type(dual_type, 0);
	data_type comp2 = dual_component_type(dual_type, 1);

	assert(type_id(comp1) == TYPE_ID_U32);
	assert(type_size(comp1) == 4);
	assert(type_id(comp2) == TYPE_ID_U64);
	assert(type_size(comp2) == 8);

	assert(dual_component_offset(dual_type, 0) == 0);
	assert(dual_component_offset(dual_type, 1) == 4);

	uint8_t	 key1[8], key2[8], key3[8];
	uint32_t k1_a = 5, k1_b = 100;
	uint32_t k2_a = 5, k2_b = 200;
	uint32_t k3_a = 6, k3_b = 50;

	pack_dual(key1, TYPE_U32, &k1_a, TYPE_U32, &k1_b);
	pack_dual(key2, TYPE_U32, &k2_a, TYPE_U32, &k2_b);
	pack_dual(key3, TYPE_U32, &k3_a, TYPE_U32, &k3_b);

	data_type u32_u32_type = make_dual(TYPE_U32, TYPE_U32);
	assert(type_compare(u32_u32_type, key1, key2) < 0);
	assert(type_compare(u32_u32_type, key2, key3) < 0);
	assert(type_compare(u32_u32_type, key1, key1) == 0);

	uint8_t	 mixed_key1[12], mixed_key2[12];
	uint32_t m1_a = 100;
	uint64_t m1_b = 0x1000000000000000ULL;
	uint32_t m2_a = 100;
	uint64_t m2_b = 0x2000000000000000ULL;

	data_type mixed_dual = make_dual(TYPE_U32, TYPE_U64);
	pack_dual(mixed_key1, TYPE_U32, &m1_a, TYPE_U64, &m1_b);
	pack_dual(mixed_key2, TYPE_U32, &m2_a, TYPE_U64, &m2_b);

	assert(type_less_than(mixed_dual, mixed_key1, mixed_key2));
}

void
test_string_operations()
{
	char fixed1[32], fixed2[32];
	strcpy(fixed1, "hello");
	strcpy(fixed2, "world");

	assert(type_less_than(TYPE_CHAR32, fixed1, fixed2));

	char fixed_dst[32];
	type_copy(TYPE_CHAR32, fixed_dst, fixed1);
	assert(strcmp(fixed_dst, fixed1) == 0);

	char varchar1[] = "alpha";
	char varchar2[] = "beta";

	assert(type_less_than(TYPE_VARCHAR(10), varchar1, varchar2));
}

void
test_type_names()
{
	assert(strcmp(type_name(TYPE_U8), "U8") == 0);
	assert(strcmp(type_name(TYPE_I64), "I64") == 0);
	assert(strcmp(type_name(TYPE_F32), "F32") == 0);
	assert(strcmp(type_name(TYPE_NULL), "NULL") == 0);

	data_type	char_type = make_char(128);
	const char *char_name = type_name(char_type);
	assert(strstr(char_name, "CHAR128") != nullptr);

	data_type	varchar_type = make_varchar(256);
	const char *varchar_name = type_name(varchar_type);
	assert(strstr(varchar_name, "VARCHAR(256)") != nullptr);

	data_type	dual_type = make_dual(TYPE_U32, TYPE_U32);
	const char *dual_name = type_name(dual_type);
	assert(strstr(dual_name, "DUAL(U32,U32)") != nullptr);
}

void
test_hot_path_operations()
{
	uint32_t values[100];
	for (int i = 0; i < 100; ++i)
	{
		values[i] = i;
	}

	for (int i = 0; i < 99; ++i)
	{
		assert(type_less_than(TYPE_U32, &values[i], &values[i + 1]));
	}

	uint8_t	  comp_keys[10][8];
	data_type dual_type = make_dual(TYPE_U32, TYPE_U32);

	for (int i = 0; i < 10; i++)
	{
		uint32_t first = i / 3;
		uint32_t second = i % 3;
		pack_dual(comp_keys[i], TYPE_U32, &first, TYPE_U32, &second);
	}

	for (int i = 0; i < 9; i++)
	{
		assert(type_less_equal(dual_type, comp_keys[i], comp_keys[i + 1]));
	}
}

void
test_types()
{
	test_type_construction();
	test_type_checking();
	test_type_comparison();
	test_arithmetic_operations();
	test_utility_operations();
	test_typed_value();
	test_type_edge_cases();
	test_comprehensive_arithmetic();
	test_dual_operations();
	test_string_operations();
	test_type_names();
	test_hot_path_operations();
	printf("types tests passed\n");
}
