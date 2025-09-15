/*
 * SQL From Scratch - Educational Database Engine
 *
 * Virtual Machine
 */

#pragma once
#include "btree.hpp"
#include "catalog.hpp"
#include "common.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

#define REGISTERS 20

/*
 * OP_Function callback
 * for implementing some specific operations that would be contrived
 * to make an opcode for, for example, %LIKE% can be implemented
 * by passing in a column in reg 1, a pattern in reg 2, and then put
 * the result in reg 3
 * (see demo.cpp)
 */
typedef bool (*vm_function)(typed_value *result,   // Output register
							typed_value *args,	   // Input registers array
							uint32_t	 arg_count // Number of arguments
);

enum STORAGE_TYPE : uint8_t
{
	BPLUS,
	RED_BLACK,
	BLOB
};

struct cursor_context
{
	STORAGE_TYPE type;
	tuple_format layout; // ephemeral tree or btree row format
	union {
		btree *tree;
		// potentially add more storage backends
	} storage;
	uint8_t flags;
};

/* The output callback: value array which comprises row + number of columns*/
typedef void (*result_callback)(typed_value *values, size_t count);

extern bool _debug;

/*
 * Whilst not aesthetically pleasing, these macros keep the creation, access, and printing of
 * opcode parameters all in one place, preventing the case where you accidentally
 * put parameter x in p2 but you try to access it in p3, or any such permutation thereof
 */

enum OPCODE : uint8_t
{

	OP_Goto = 1,
#define GOTO_MAKE(pc)	   {OP_Goto, 0, pc, 0, nullptr, 0}
#define GOTO_TARGET()	   (inst->p2)
#define GOTO_DEBUG_PRINT() printf("GOTO -> PC=%d", GOTO_TARGET())

	OP_Halt = 2,
#define HALT_MAKE(exit_code) {OP_Halt, exit_code, 0, 0, nullptr, 0}
#define HALT_EXIT_CODE()	 (inst->p1)
#define HALT_DEBUG_PRINT()	 printf("HALT exit_code=%d", HALT_EXIT_CODE())

	OP_Open = 10,
#define OPEN_MAKE(cursor_id, context) {OP_Open, cursor_id, 0, 0, context, 0}
#define OPEN_CURSOR_ID()			  (inst->p1)
#define OPEN_LAYOUT()				  ((cursor_context *)(inst->p4))
#define OPEN_DEBUG_PRINT()			  printf("OPEN cursor=%d", OPEN_CURSOR_ID())

	OP_Close = 12,
#define CLOSE_MAKE(cursor_id) {OP_Close, cursor_id, 0, 0, nullptr, 0}
#define CLOSE_CURSOR_ID()	  (inst->p1)
#define CLOSE_DEBUG_PRINT()	  printf("CLOSE cursor=%d", CLOSE_CURSOR_ID())

	OP_Rewind = 13,
#define REWIND_MAKE(cursor_id, result_reg, to_end) {OP_Rewind, cursor_id, result_reg, 0, nullptr, (uint8_t)to_end}
#define REWIND_CURSOR_ID()						   (inst->p1)
#define REWIND_RESULT_REG()						   (inst->p2)
#define REWIND_TO_END()							   (inst->p5 != 0)
#define REWIND_DEBUG_PRINT()                                                                                           \
	printf("REWIND cursor=%d to_%s -> R[%d]", REWIND_CURSOR_ID(), REWIND_TO_END() ? "end" : "start",                   \
		   REWIND_RESULT_REG())

	OP_Step = 14,
#define STEP_MAKE(cursor_id, result_reg, forward) {OP_Step, cursor_id, result_reg, 0, nullptr, (uint8_t)forward}
#define STEP_CURSOR_ID()						  (inst->p1)
#define STEP_RESULT_REG()						  (inst->p2)
#define STEP_FORWARD()							  (inst->p5 != 0)
#define STEP_DEBUG_PRINT()                                                                                             \
	printf("STEP cursor=%d %s -> R[%d]", STEP_CURSOR_ID(), STEP_FORWARD() ? "forward" : "backward", STEP_RESULT_REG())

	OP_Seek = 20,
#define SEEK_MAKE(cursor_id, key_reg, result_reg, op) {OP_Seek, cursor_id, key_reg, result_reg, nullptr, (uint8_t)op}
#define SEEK_CURSOR_ID()							  (inst->p1)
#define SEEK_KEY_REG()								  (inst->p2)
#define SEEK_RESULT_REG()							  (inst->p3)
#define SEEK_OP()									  ((COMPARISON_OP)(inst->p5))
#define SEEK_DEBUG_PRINT()                                                                                             \
	printf("SEEK cursor=%d key=R[%d] op=%s -> R[%d]", SEEK_CURSOR_ID(), SEEK_KEY_REG(),                                \
		   debug_compare_op_name(SEEK_OP()), SEEK_RESULT_REG())

	OP_Column = 30,
#define COLUMN_MAKE(cursor_id, column_index, dest_reg) {OP_Column, cursor_id, column_index, dest_reg, nullptr, 0}
#define COLUMN_CURSOR_ID()							   (inst->p1)
#define COLUMN_INDEX()								   (inst->p2)
#define COLUMN_DEST_REG()							   (inst->p3)
#define COLUMN_DEBUG_PRINT()                                                                                           \
	printf("COLUMN cursor=%d col=%d -> R[%d]", COLUMN_CURSOR_ID(), COLUMN_INDEX(), COLUMN_DEST_REG())

	OP_Insert = 34,
#define INSERT_MAKE(cursor_id, start_reg, reg_count) {OP_Insert, cursor_id, start_reg, reg_count, nullptr, 0}
#define INSERT_CURSOR_ID()							 (inst->p1)
#define INSERT_KEY_REG()							 (inst->p2)
#define INSERT_REG_COUNT()							 (inst->p3)
#define INSERT_DEBUG_PRINT()                                                                                           \
	printf("INSERT cursor=%d key=R[%d] reg_count=%d", INSERT_CURSOR_ID(), INSERT_KEY_REG(), INSERT_REG_COUNT())

	OP_Delete = 35,
#define DELETE_MAKE(cursor_id, cursor_valid_reg, delete_occurred_reg)                                                  \
	{OP_Delete, cursor_id, cursor_valid_reg, delete_occurred_reg, nullptr, 0}
#define DELETE_CURSOR_ID()			 (inst->p1)
#define DELETE_CURSOR_VALID_REG()	 (inst->p2)
#define DELETE_DELETE_OCCURRED_REG() (inst->p3)
#define DELETE_DEBUG_PRINT()                                                                                           \
	printf("DELETE cursor=%d -> R[%d]=valid R[%d]=occurred", DELETE_CURSOR_ID(), DELETE_CURSOR_VALID_REG(),            \
		   DELETE_DELETE_OCCURRED_REG())

	OP_Update = 36,
#define UPDATE_MAKE(cursor_id, record_reg) {OP_Update, cursor_id, record_reg, 0, nullptr, 0}
#define UPDATE_CURSOR_ID()				   (inst->p1)
#define UPDATE_RECORD_REG()				   (inst->p2)
#define UPDATE_DEBUG_PRINT()			   printf("UPDATE cursor=%d record=R[%d]", UPDATE_CURSOR_ID(), UPDATE_RECORD_REG())

	OP_Move = 40,
#define MOVE_MOVE_MAKE(dest_reg, src_reg) {OP_Move, dest_reg, 0, src_reg, nullptr, 0}
#define MOVE_DEST_REG()					  (inst->p1)
#define MOVE_SRC_REG()					  (inst->p3)
#define MOVE_DEBUG_PRINT()				  printf("MOVE R[%d] <- R[%d]", MOVE_DEST_REG(), MOVE_SRC_REG())

	OP_Load = 41,
#define LOAD_MAKE(dest_reg, type, data) {OP_Load, dest_reg, type, 0, data, 0}
#define LOAD_DEST_REG()					(inst->p1)
#define LOAD_TYPE()						((data_type)(inst->p2))
#define LOAD_DATA()						((uint8_t *)(inst->p4))
#define LOAD_IS_LOAD()					(inst->opcode == OP_Load)
#define LOAD_DEBUG_PRINT()                                                                                             \
	do                                                                                                                 \
	{                                                                                                                  \
		printf("LOAD R[%d] <- ", LOAD_DEST_REG());                                                                     \
		type_print(LOAD_TYPE(), LOAD_DATA());                                                                          \
		printf(" (%s)", type_name(LOAD_TYPE()));                                                                       \
	} while (0)

	OP_Arithmetic = 51,
#define ARITHMETIC_MAKE(dest_reg, left_reg, right_reg, op)                                                             \
	{OP_Arithmetic, dest_reg, left_reg, right_reg, nullptr, (uint8_t)op}
#define ARITHMETIC_DEST_REG()  (inst->p1)
#define ARITHMETIC_LEFT_REG()  (inst->p2)
#define ARITHMETIC_RIGHT_REG() (inst->p3)
#define ARITHMETIC_OP()		   ((ARITH_OP)(inst->p5))
#define ARITHMETIC_DEBUG_PRINT()                                                                                       \
	printf("ARITHMETIC R[%d] <- R[%d] %s R[%d]", ARITHMETIC_DEST_REG(), ARITHMETIC_LEFT_REG(),                         \
		   debug_arith_op_name(ARITHMETIC_OP()), ARITHMETIC_RIGHT_REG())

	OP_JumpIf = 52,
#define JUMPIF_MAKE(test_reg, jump_pc, jump_on_true) {OP_JumpIf, test_reg, jump_pc, 0, nullptr, (uint8_t)jump_on_true}
#define JUMPIF_TEST_REG()							 (inst->p1)
#define JUMPIF_JUMP_TARGET()						 (inst->p2)
#define JUMPIF_JUMP_ON_TRUE()						 (inst->p5 != 0)
#define JUMPIF_DEBUG_PRINT()                                                                                           \
	printf("JUMPIF R[%d] %s -> PC=%d", JUMPIF_TEST_REG(), JUMPIF_JUMP_ON_TRUE() ? "TRUE" : "FALSE",                    \
		   JUMPIF_JUMP_TARGET())

	OP_Logic = 53,
#define LOGIC_MAKE(dest_reg, left_reg, right_reg, op) {OP_Logic, dest_reg, left_reg, right_reg, nullptr, (uint8_t)op}
#define LOGIC_DEST_REG()							  (inst->p1)
#define LOGIC_LEFT_REG()							  (inst->p2)
#define LOGIC_RIGHT_REG()							  (inst->p3)
#define LOGIC_OP()									  ((LOGIC_OP)(inst->p5))
#define LOGIC_DEBUG_PRINT()                                                                                            \
	printf("LOGIC R[%d] <- R[%d] %s R[%d]", LOGIC_DEST_REG(), LOGIC_LEFT_REG(), debug_logic_op_name(LOGIC_OP()),       \
		   LOGIC_RIGHT_REG())

	OP_Result = 54,
#define RESULT_MAKE(first_reg, reg_count) {OP_Result, first_reg, reg_count, 0, nullptr, 0}
#define RESULT_FIRST_REG()				  (inst->p1)
#define RESULT_REG_COUNT()				  (inst->p2)
#define RESULT_DEBUG_PRINT()                                                                                           \
	printf("RESULT R[%d..%d] (%d registers)", RESULT_FIRST_REG(), RESULT_FIRST_REG() + RESULT_REG_COUNT() - 1,         \
		   RESULT_REG_COUNT())

	OP_Test = 60,
#define TEST_MAKE(dest_reg, left_reg, right_reg, op) {OP_Test, dest_reg, left_reg, right_reg, nullptr, (uint8_t)op}
#define TEST_DEST_REG()								 (inst->p1)
#define TEST_LEFT_REG()								 (inst->p2)
#define TEST_RIGHT_REG()							 (inst->p3)
#define TEST_OP()									 ((COMPARISON_OP)(inst->p5))
#define TEST_DEBUG_PRINT()                                                                                             \
	printf("TEST R[%d] <- R[%d] %s R[%d]", TEST_DEST_REG(), TEST_LEFT_REG(), debug_compare_op_name(TEST_OP()),         \
		   TEST_RIGHT_REG())

	OP_Function = 61,
#define FUNCTION_MAKE(dest_reg, first_arg_reg, arg_count, fn_ptr)                                                      \
	{OP_Function, dest_reg, first_arg_reg, arg_count, fn_ptr, 0}
#define FUNCTION_DEST_REG()		 (inst->p1)
#define FUNCTION_FIRST_ARG_REG() (inst->p2)
#define FUNCTION_ARG_COUNT()	 (inst->p3)
#define FUNCTION_FUNCTION()		 ((vm_function)(inst->p4))
#define FUNCTION_DEBUG_PRINT()                                                                                         \
	printf("FUNCTION R[%d] <- fn(R[%d..%d]) %d args", FUNCTION_DEST_REG(), FUNCTION_FIRST_ARG_REG(),                   \
		   FUNCTION_FIRST_ARG_REG() + FUNCTION_ARG_COUNT() - 1, FUNCTION_ARG_COUNT())

	OP_Begin = 62,
#define BEGIN_MAKE()		{OP_Begin, 0, 0, 0, nullptr, 0}
#define BEGIN_DEBUG_PRINT() printf("BEGIN transaction")

	OP_Commit = 63,
#define COMMIT_MAKE()		 {OP_Commit, 0, 0, 0, nullptr, 0}
#define COMMIT_DEBUG_PRINT() printf("COMMIT transaction")

	OP_Rollback = 64,
#define ROLLBACK_MAKE()		   {OP_Rollback, 0, 0, 0, nullptr, 0}
#define ROLLBACK_DEBUG_PRINT() printf("ROLLBACK transaction")

	OP_Pack = 65,
#define PACK2_MAKE(dest_reg, left_reg, right_reg) {OP_Pack, dest_reg, left_reg, right_reg, nullptr, 0}
#define PACK2_DEST_REG()						  (inst->p1)
#define PACK2_LEFT_REG()						  (inst->p2)
#define PACK2_RIGHT_REG()						  (inst->p3)
#define PACK2_DEBUG_PRINT()                                                                                            \
	printf("PACK2 R[%d] <- pack(R[%d], R[%d])", PACK2_DEST_REG(), PACK2_LEFT_REG(), PACK2_RIGHT_REG())

	OP_Unpack = 66,
#define UNPACK2_MAKE(first_dest_reg, src_reg) {OP_Unpack, first_dest_reg, src_reg, 0, nullptr, 0}
#define UNPACK2_FIRST_DEST_REG()			  (inst->p1)
#define UNPACK2_SRC_REG()					  (inst->p2)
#define UNPACK2_DEBUG_PRINT()                                                                                          \
	printf("UNPACK2 R[%d],R[%d] <- unpack(R[%d])", UNPACK2_FIRST_DEST_REG(), UNPACK2_FIRST_DEST_REG() + 1,             \
		   UNPACK2_SRC_REG())
};

/* A loose interpretation of sqlite's opcode structure */
struct vm_instruction
{
	OPCODE	opcode;
	int32_t p1;
	int64_t p2;
	int32_t p3;
	void   *p4; // pointer to memory allocated in the query_arena, for strings or config items
	            // like the cursor_context
	uint8_t p5;
};


/*
 * We could have more detailed error handling,
 * but the design is such that the VM shouldn't ever fail, providing
 * that the compiled program is correct (though this is only possible due to the small scope).
 * The only potential source of invalid input is (ideally) from the user's query.
 */
enum VM_RESULT : uint8_t
{
	OK,
	ABORT,
	ERR
};

VM_RESULT
vm_execute(vm_instruction *instructions, int instruction_count);

void
vm_set_result_callback(result_callback callback);



inline void
vm_debug_print_instruction(const vm_instruction *inst, int pc)
{
	printf("PC[%3d] ", pc);

	switch (inst->opcode)
	{
	case OP_Goto:
		GOTO_DEBUG_PRINT();
		break;
	case OP_Halt:
		HALT_DEBUG_PRINT();
		break;
	case OP_Open:
		OPEN_DEBUG_PRINT();
		break;
	case OP_Close:
		CLOSE_DEBUG_PRINT();
		break;
	case OP_Rewind:
		REWIND_DEBUG_PRINT();
		break;
	case OP_Step:
		STEP_DEBUG_PRINT();
		break;
	case OP_Seek:
		SEEK_DEBUG_PRINT();
		break;
	case OP_Column:
		COLUMN_DEBUG_PRINT();
		break;
	case OP_Insert:
		INSERT_DEBUG_PRINT();
		break;
	case OP_Delete:
		DELETE_DEBUG_PRINT();
		break;
	case OP_Update:
		UPDATE_DEBUG_PRINT();
		break;
	case OP_Move:
		MOVE_DEBUG_PRINT();
		break;
	case OP_Load:
		LOAD_DEBUG_PRINT();
		break;
	case OP_Arithmetic:
		ARITHMETIC_DEBUG_PRINT();
		break;
	case OP_JumpIf:
		JUMPIF_DEBUG_PRINT();
		break;
	case OP_Logic:
		LOGIC_DEBUG_PRINT();
		break;
	case OP_Result:
		RESULT_DEBUG_PRINT();
		break;
	case OP_Test:
		TEST_DEBUG_PRINT();
		break;
	case OP_Function:
		FUNCTION_DEBUG_PRINT();
		break;
	case OP_Begin:
		BEGIN_DEBUG_PRINT();
		break;
	case OP_Commit:
		COMMIT_DEBUG_PRINT();
		break;
	case OP_Rollback:
		ROLLBACK_DEBUG_PRINT();
		break;
	case OP_Pack:
		PACK2_DEBUG_PRINT();
		break;
	case OP_Unpack:
		UNPACK2_DEBUG_PRINT();
		break;
	default:
		printf("UNKNOWN opcode=%d", inst->opcode);
		break;
	}
	printf("\n");
}

inline void
vm_debug_print_program(vm_instruction *instructions, int count)
{
	printf("\n===== PROGRAM LISTING =====\n");
	for (int i = 0; i < count; i++)
	{
		vm_debug_print_instruction(&instructions[i], i);
	}
	printf("===========================\n\n");
}
