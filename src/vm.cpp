/*
 * SQL From Scratch
 *
 * Virtual Machine
 *
 * The VM is the execution layer of the SQL engine which it takes compiled query
 * plans (sequences of instructions) and executes them against the storage
 * layer. This provides an abstraction boundary: the compiler transforms SQL
 * into instructions without knowing how data is physically stored, and the
 * storage layer doesn't need to understand SQL semantics.
 *
 * Conceptually, a VM is just a big switch statement, executes parameterized
 * instructions based on the opcode, has some state, and can do
 * logic/arithemetic/conditions on values within registers
 */
#include "vm.hpp"
#include "arena.hpp"
#include "btree.hpp"
#include "cassert"
#include "catalog.hpp"
#include "common.hpp"
#include "ephemeral.hpp"
#include "pager.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CURSORS 10

bool _debug = false;

/*
 * The vm cursor wraps other cursors to
 * provide a unified api for the vm to do data manipulation
 *
 * Currently there is just two cursor types, btree and ephemeral (red black)
 * but we could have other storage backends with different properties.
 *
 * If we had hash based storage, while it might be contrived to have a cursor
 * over it (as you'd rarely want to do range queries), you still could, you'd
 * just mainly use direct seeks as opposed to calling next/prev
 */
struct vm_cursor {
  STORAGE_TYPE type;
  tuple_format layout;

  union {
    bt_cursor btree;
    et_cursor ephemeral;
  } cursor;
};

void vmcursor_print(vm_cursor *cursor) {
  auto format = cursor->layout.columns;
  switch (cursor->type) {
  case RED_BLACK:
    et_print(&cursor->cursor.ephemeral.tree, &format);
    break;
  case BPLUS:
    bt_print(cursor->cursor.btree.tree, &format);
  }
}

void vmcursor_open(vm_cursor *cursor, cursor_context *context) {
  switch (context->type) {
  case BPLUS: {
    cursor->type = BPLUS;
    cursor->layout = context->layout;
    cursor->cursor.btree.tree = context->storage.tree;
    cursor->cursor.btree.state = BT_CURSOR_INVALID;
    break;
  }
  case RED_BLACK: {
    cursor->type = RED_BLACK;
    cursor->layout = context->layout;
    data_type key_type = cursor->layout.columns[0];
    bool allow_duplicates = (bool)context->flags;
    cursor->cursor.ephemeral.tree =
        et_create(key_type, cursor->layout.record_size, allow_duplicates);
    cursor->cursor.ephemeral.state = et_cursor::INVALID;
    break;
  }
  }
}

bool vmcursor_rewind(vm_cursor *cur, bool to_end) {
  switch (cur->type) {
  case RED_BLACK:
    return to_end ? et_cursor_last(&cur->cursor.ephemeral)
                  : et_cursor_first(&cur->cursor.ephemeral);
  case BPLUS:
    return to_end ? bt_cursor_last(&cur->cursor.btree)
                  : bt_cursor_first(&cur->cursor.btree);
  default:
    return false;
  }
}

bool vmcursor_step(vm_cursor *cur, bool forward) {
  switch (cur->type) {
  case RED_BLACK:
    return forward ? et_cursor_next(&cur->cursor.ephemeral)
                   : et_cursor_previous(&cur->cursor.ephemeral);
  case BPLUS:
    return forward ? bt_cursor_next(&cur->cursor.btree)
                   : bt_cursor_previous(&cur->cursor.btree);
  default:
    return false;
  }
}

void vmcursor_clear(vm_cursor *cursor) {
  switch (cursor->type) {
  case BPLUS: {
    bt_clear(cursor->cursor.btree.tree);
    break;
  case RED_BLACK: {
    et_clear(&cursor->cursor.ephemeral.tree);
    break;
  }
  }
  }
}

bool vmcursor_seek(vm_cursor *cur, COMPARISON_OP op, uint8_t *key) {
  switch (cur->type) {
  case RED_BLACK:
    return et_cursor_seek(&cur->cursor.ephemeral, key, op);
  case BPLUS:
    return bt_cursor_seek(&cur->cursor.btree, key, op);
  default:
    return false;
  }
}

bool vmcursor_is_valid(vm_cursor *cur) {
  switch (cur->type) {
  case RED_BLACK:
    return et_cursor_is_valid(&cur->cursor.ephemeral);
  case BPLUS:
    return bt_cursoris_valid(&cur->cursor.btree);
  }
  return false;
}

uint8_t *vmcursor_get_key(vm_cursor *cur) {
  switch (cur->type) {
  case RED_BLACK:
    return (uint8_t *)et_cursor_key(&cur->cursor.ephemeral);
  case BPLUS:
    return (uint8_t *)bt_cursor_key(&cur->cursor.btree);
  }
  return nullptr;
}

uint8_t *vmcursor_get_record(vm_cursor *cur) {
  switch (cur->type) {
  case RED_BLACK:
    return (uint8_t *)et_cursor_record(&cur->cursor.ephemeral);
  case BPLUS:
    return (uint8_t *)bt_cursor_record(&cur->cursor.btree);
  }
  return nullptr;
}

uint8_t *vmcursor_column(vm_cursor *cur, uint32_t col_index) {

  /*
   * keys and records are stored separately in btree leaf nodes
   *
   * [key0, key1, ...keyn][record0,record1, recordn]
   *
   * So when we try to access column 0 (implicitly the primary key), we don't
   * apply any offset.
   *
   * The the trees view the record as a black box of a given size, so
   * we ask for the record, which points to the beginning of this memory
   * then the vm cursor, which has the schema information and calculated offsets
   * applies the nessssary offset:
   *
   * record:
   * [u32, char16, i32, i32]
   * offsets:
   * [0, 4, 20, 24]
   * vmcursor_column(3)
   * offsets[3 - 1] = 20
   * data + (u8)20 -> i32
   *
   */
  if (col_index == 0) {
    return vmcursor_get_key(cur);
  }
  uint8_t *record = vmcursor_get_record(cur);
  return record + cur->layout.offsets[col_index - 1];
}

data_type vmcursor_column_type(vm_cursor *cur, uint32_t col_index) {
  return cur->layout.columns[col_index];
}

bool vmcursor_insert(vm_cursor *cur, uint8_t *key, uint8_t *record,
                     uint32_t size) {
  switch (cur->type) {
  case RED_BLACK:
    return et_cursor_insert(&cur->cursor.ephemeral, (void *)key,
                            (void *)record);
  case BPLUS:
    return bt_cursor_insert(&cur->cursor.btree, key, record);
  default:
    return false;
  }
}

bool vmcursor_update(vm_cursor *cur, uint8_t *record) {
  switch (cur->type) {
  case RED_BLACK:
    return et_cursor_update(&cur->cursor.ephemeral, record);
  case BPLUS:
    return bt_cursor_update(&cur->cursor.btree, record);
  default:
    return false;
  }
}

bool vmcursor_remove(vm_cursor *cur) {
  switch (cur->type) {
  case RED_BLACK:
    return et_cursor_delete(&cur->cursor.ephemeral);
  case BPLUS:
    return bt_cursor_delete(&cur->cursor.btree);
  default:
    return false;
  }
}

const char *vmcursor_type_name(vm_cursor *cur) {
  switch (cur->type) {
  case RED_BLACK:
    return "RED_BLACK";
  case BPLUS:
    return "BPLUS";
  }
  return "UNKNOWN";
}

void vmcursor_print_current(vm_cursor *cur) {
  printf("Cursor type=%s, valid=%d", vmcursor_type_name(cur),
         vmcursor_is_valid(cur));
  if (vmcursor_is_valid(cur)) {
    uint8_t *key = vmcursor_get_key(cur);
    if (key) {
      printf(", key=");
      data_type key_type = cur->layout.columns[0];
      type_print(key_type, key);
    }
  }
  printf("\n");
}

static struct {
  vm_instruction *program;
  int program_size;
  uint32_t pc;
  bool halted;
  typed_value registers[REGISTERS];
  vm_cursor cursors[CURSORS];
  result_callback emit_row;
} VM = {};

static void set_register(typed_value *dest, uint8_t *src, data_type type) {
  /*
   * Our registers operate with heap(arena) allocated memory which is reset
   * every query.
   *
   * If we want to set a register, that already has memory set, and that memory
   * can fit our new data type, just copy that data in and set the registers new
   * type.
   *
   * Reg 1 before
   * type: u64, 8 bytes
   * [F,F,F,F,F,F,F,F]
   *
   * then set the register as a u32
   *
   * Reg 1 after
   * type: u32, 4 bytes
   * [F,F,F,F,F,F,F,F], still has 8 bytes allocated, but we only interpret the 4
   *
   *
   */
  if (dest->get_size() < type_size(type)) {
    arena<query_arena>::reclaim(dest->data, type_size(dest->type));
    dest->data = (uint8_t *)arena<query_arena>::alloc(type_size(type));
  } else if (nullptr == dest->data) {
    dest->data = (uint8_t *)arena<query_arena>::alloc(type_size(type));
  }

  dest->type = type;
  type_copy(type, dest->data, src);
}

static void set_register(typed_value *dest, typed_value *src) {
  set_register(dest, (uint8_t *)src->data, src->type);
}

static void build_record(uint8_t *data, int32_t first_reg, int32_t count) {
  /*
   * In order to insert a record into a tree, we need to create one contiguous
   * block of a given size.
   *
   * Our btree doesn't interpret records, just how large they are.
   *
   * For an insert of a record type [u32, u32, u32], we load the values
   * into contiguous registers, then call this function pointing at the
   * first non-key containing register, which copies the value into a buffer
   * of the required size, and that will be inserted.
   */

  int32_t offset = 0;
  for (int i = 0; i < count; i++) {
    typed_value *val = &VM.registers[first_reg + i];
    uint32_t size = type_size(val->type);
    memcpy(data + offset, val->data, size);
    offset += size;
  }
}

static void reset() {
  VM.pc = 0;
  VM.halted = false;
  for (uint32_t i = 0; i < REGISTERS; i++) {
    VM.registers[i].type = TYPE_NULL;
    VM.registers[i].data = nullptr;
  }
  VM.program = nullptr;
  VM.program_size = 0;
}

void vm_debug_print_all_registers() {
  printf("===== REGISTERS =====\n");
  for (int i = 0; i < REGISTERS; i++) {
    if (VM.registers[i].type != TYPE_NULL && VM.registers[i].data != nullptr) {
      printf("R[%2d] = ", i);
      type_print(VM.registers[i].type, VM.registers[i].data);
      printf(" (%s)\n", type_name(VM.registers[i].type));
    }
  }
  printf("====================\n");
}

static VM_RESULT step() {
  vm_instruction *inst = &VM.program[VM.pc];

  switch (inst->opcode) {
  case OP_Halt: {
    if (_debug) {
      printf("=> Halting with code %d\n", HALT_EXIT_CODE());
    }
    VM.halted = true;
    return OK;
  }
  case OP_Goto: {
    int32_t target = GOTO_TARGET();
    if (_debug) {
      printf("=> Jumping to PC=%d\n", target);
    }
    VM.pc = target;
    return OK;
  }
  case OP_Load: {
    int32_t dest_reg = LOAD_DEST_REG();
    uint8_t *data = LOAD_DATA();
    data_type type = LOAD_TYPE();

    auto to_load = typed_value::make(type, data);
    if (_debug) {
      printf("=> R[%d] = ", dest_reg);
      type_print(type, data);
      printf(" (%s)\n", type_name(type));
    }

    set_register(&VM.registers[dest_reg], &to_load);
    VM.pc++;
    return OK;
  }
  case OP_Move: {
    int32_t dest_reg = MOVE_DEST_REG();
    int32_t src_reg = MOVE_SRC_REG();
    auto *src = &VM.registers[src_reg];
    if (_debug) {
      printf("=> R[%d] = R[%d] = ", dest_reg, src_reg);
      if (src->data) {
        type_print(src->type, src->data);
      } else {
        printf("NULL");
      }
      printf(" (%s)\n", type_name(src->type));
    }
    set_register(&VM.registers[dest_reg], src);
    VM.pc++;
    return OK;
  }
  case OP_Test: {
    int32_t dest = TEST_DEST_REG();
    int32_t left = TEST_LEFT_REG();
    int32_t right = TEST_RIGHT_REG();
    COMPARISON_OP op = TEST_OP();
    typed_value *a = &VM.registers[left];
    typed_value *b = &VM.registers[right];
    int cmp_result = type_compare(a->type, a->data, b->data);
    uint32_t test_result = false;

    switch (op) {
    case EQ:
      test_result = (cmp_result == 0);
      break;
    case NE:
      test_result = (cmp_result != 0);
      break;
    case LT:
      test_result = (cmp_result < 0);
      break;
    case LE:
      test_result = (cmp_result <= 0);
      break;
    case GT:
      test_result = (cmp_result > 0);
      break;
    case GE:
      test_result = (cmp_result >= 0);
      break;
    }

    if (_debug) {
      printf("=> R[%d] = (", dest);
      type_print(a->type, a->data);
      printf(" %s ", debug_compare_op_name(op));
      type_print(b->type, b->data);
      printf(") = %s\n", test_result ? "TRUE" : "FALSE");
    }

    set_register(&VM.registers[dest], (uint8_t *)&test_result, TYPE_U32);
    VM.pc++;
    return OK;
  }
  case OP_Function: {
    int32_t dest = FUNCTION_DEST_REG();
    int32_t first_arg = FUNCTION_FIRST_ARG_REG();
    int32_t count = FUNCTION_ARG_COUNT();
    vm_function fn = FUNCTION_FUNCTION();

    if (_debug) {
      printf("=> R[%d] = fn(", dest);
      for (int i = 0; i < count; i++) {
        if (i > 0)
          printf(", ");
        typed_value *arg = &VM.registers[first_arg + i];
        printf("R[%d]=", first_arg + i);
        if (arg->data) {
          type_print(arg->type, arg->data);
        } else {
          printf("NULL");
        }
      }
      printf(")\n");
    }

    bool success = fn(&VM.registers[dest], &VM.registers[first_arg], count);

    if (!success) {
      return ERR;
    }

    VM.pc++;
    return OK;
  }
  case OP_JumpIf: {
    int32_t test_reg = JUMPIF_TEST_REG();
    int32_t target = JUMPIF_JUMP_TARGET();
    bool jump_on_true = JUMPIF_JUMP_ON_TRUE();
    typed_value *val = &VM.registers[test_reg];
    bool is_true = (*(uint32_t *)val->data != 0);
    bool will_jump = (is_true && jump_on_true) || (!is_true && !jump_on_true);

    if (_debug) {
      printf("=> R[%d]=", test_reg);
      type_print(val->type, val->data);
      printf(" (%s), jump_on_%s => %s to PC=%d\n", is_true ? "TRUE" : "FALSE",
             jump_on_true ? "true" : "false",
             will_jump ? "JUMPING" : "CONTINUE",
             will_jump ? target : VM.pc + 1);
    }

    if (will_jump) {
      VM.pc = target;
    } else {
      VM.pc++;
    }
    return OK;
  }
  case OP_Logic: {
    int32_t dest = LOGIC_DEST_REG();
    int32_t left = LOGIC_LEFT_REG();
    int32_t right = LOGIC_RIGHT_REG();
    LOGIC_OP op = LOGIC_OP();
    typed_value result = typed_value::make(TYPE_U32);
    result.data = (uint8_t *)arena<query_arena>::alloc(type_size(TYPE_U32));
    uint32_t a = *(uint32_t *)VM.registers[left].data;
    uint32_t b = *(uint32_t *)VM.registers[right].data;
    uint32_t res_val;

    switch (op) {
    case LOGIC_AND:
      res_val = (a && b) ? 1 : 0;
      break;
    case LOGIC_OR:
      res_val = (a || b) ? 1 : 0;
      break;
    default:
      return ERR;
    }

    *(uint32_t *)result.data = res_val;

    if (_debug) {
      printf("=> R[%d] = %d %s %d = %d\n", dest, a, debug_logic_op_name(op), b,
             res_val);
    }

    set_register(&VM.registers[dest], &result);
    VM.pc++;
    return OK;
  }
  case OP_Result: {
    int32_t first_reg = RESULT_FIRST_REG();
    int32_t reg_count = RESULT_REG_COUNT();

    if (_debug) {
      printf("=> RESULT: ");
      for (int i = 0; i < reg_count; i++) {
        if (i > 0)
          printf(", ");
        typed_value *val = &VM.registers[first_reg + i];
        printf("R[%d]=", first_reg + i);
        if (val->data) {
          type_print(val->type, val->data);
        } else {
          printf("NULL");
        }
      }
      printf("\n");
    }

    typed_value *values = (typed_value *)arena<query_arena>::alloc(
        sizeof(typed_value) * reg_count);

    for (int i = 0; i < reg_count; i++) {
      typed_value *val = &VM.registers[first_reg + i];
      values[i].type = val->type;
      values[i].data =
          (uint8_t *)arena<query_arena>::alloc(type_size(val->type));
      memcpy(values[i].data, val->data, type_size(val->type));
    }

    VM.emit_row(values, reg_count);
    VM.pc++;
    return OK;
  }
  case OP_Arithmetic: {
    int32_t dest = ARITHMETIC_DEST_REG();
    int32_t left = ARITHMETIC_LEFT_REG();
    int32_t right = ARITHMETIC_RIGHT_REG();
    ARITH_OP op = ARITHMETIC_OP();
    typed_value *a = &VM.registers[left];
    typed_value *b = &VM.registers[right];

    typed_value result = {.type = (a->type > b->type) ? a->type : b->type};
    result.data = (uint8_t *)arena<query_arena>::alloc(type_size(result.type));

    bool success = true;

    switch (op) {
    case ARITH_ADD:
      type_add(result.type, result.data, a->data, b->data);
      break;
    case ARITH_SUB:
      type_sub(result.type, result.data, a->data, b->data);
      break;
    case ARITH_MUL:
      type_mul(result.type, result.data, a->data, b->data);
      break;
    case ARITH_DIV:
      type_div(result.type, result.data, a->data, b->data);
      break;
    }

    if (_debug) {
      printf("=> R[%d] = ", dest);
      type_print(a->type, a->data);
      printf(" %s ", debug_arith_op_name(op));
      type_print(b->type, b->data);
      printf(" = ");
      if (success) {
        type_print(result.type, result.data);
      } else {
        printf("ERROR");
      }
      printf("\n");
    }

    if (!success) {
      return ERR;
    }

    set_register(&VM.registers[dest], &result);
    VM.pc++;
    return OK;
  }
  case OP_Open: {
    int32_t cursor_id = OPEN_CURSOR_ID();
    vm_cursor *cursor = &VM.cursors[cursor_id];
    cursor_context *context = OPEN_LAYOUT();

    if (_debug) {
      const char *name;
      switch (context->type) {
      case BPLUS:
        name = "BPLUS";
        break;
      case RED_BLACK:
        name = "RED_BLACK";
        break;
      default:
        name = "UNKNOWN";
      }
      printf("=> Opening cursor %d type=%s\n", cursor_id, name);
    }

    vmcursor_open(cursor, context);

    VM.pc++;
    return OK;
  }
  case OP_Close: {
    int32_t cursor_id = CLOSE_CURSOR_ID();
    if (_debug) {
      vmcursor_print(&VM.cursors[cursor_id]);
      printf("=> Closed cursor %d\n", cursor_id);
    }

    VM.pc++;
    return OK;
  }
  case OP_Rewind: {
    int32_t cursor_id = REWIND_CURSOR_ID();
    int32_t result_reg = REWIND_RESULT_REG();
    bool to_end = REWIND_TO_END();
    vm_cursor *cursor = &VM.cursors[cursor_id];
    bool valid = vmcursor_rewind(cursor, to_end);
    uint32_t result_val = valid ? 1 : 0;

    if (_debug) {
      printf("=> Cursor %d rewound to %s, R[%d]=%d\n", cursor_id,
             to_end ? "end" : "start", result_reg, result_val);
    }

    set_register(&VM.registers[result_reg], (uint8_t *)&result_val, TYPE_U32);
    VM.pc++;
    return OK;
  }
  case OP_Step: {
    int32_t cursor_id = STEP_CURSOR_ID();
    int32_t result_reg = STEP_RESULT_REG();
    bool forward = STEP_FORWARD();
    vm_cursor *cursor = &VM.cursors[cursor_id];
    uint32_t has_more = vmcursor_step(cursor, forward) ? 1 : 0;

    if (_debug) {
      printf("=> Cursor %d stepped %s, R[%d]=%d\n", cursor_id,
             forward ? "forward" : "backward", result_reg, has_more);
    }

    set_register(&VM.registers[result_reg], (uint8_t *)&has_more, TYPE_U32);
    VM.pc++;
    return OK;
  }
  case OP_Seek: {
    int32_t cursor_id = SEEK_CURSOR_ID();
    int32_t key_reg = SEEK_KEY_REG();
    int32_t result_reg = SEEK_RESULT_REG();
    COMPARISON_OP op = SEEK_OP();
    vm_cursor *cursor = &VM.cursors[cursor_id];
    typed_value *key = &VM.registers[key_reg];
    bool found = vmcursor_seek(cursor, op, (uint8_t *)key->data);
    uint32_t result_val = found ? 1 : 0;

    if (_debug) {
      printf("=> Cursor %d seek %s with key=", cursor_id,
             debug_compare_op_name(op));
      type_print(key->type, key->data);
      printf(", R[%d]=%d\n", result_reg, result_val);
    }

    set_register(&VM.registers[result_reg], (uint8_t *)&result_val, TYPE_U32);
    VM.pc++;
    return OK;
  }
  case OP_Column: {
    int32_t cursor_id = COLUMN_CURSOR_ID();
    int32_t col_index = COLUMN_INDEX();
    int32_t dest_reg = COLUMN_DEST_REG();
    vm_cursor *cursor = &VM.cursors[cursor_id];

    data_type column_type = vmcursor_column_type(cursor, col_index);
    auto column_value = vmcursor_column(cursor, col_index);

    typed_value src = typed_value::make(column_type, column_value);

    if (_debug) {
      printf("=> R[%d] = cursor[%d].col[%d] = ", dest_reg, cursor_id,
             col_index);
      if (src.data) {
        type_print(src.type, src.data);
      } else {
        printf("NULL");
      }
      printf(" (%s)\n", type_name(src.type));
    }

    set_register(&VM.registers[dest_reg], &src);
    VM.pc++;
    return OK;
  }
  case OP_Delete: {
    int32_t cursor_id = DELETE_CURSOR_ID();
    int32_t delete_occurred = DELETE_DELETE_OCCURRED_REG();
    int32_t cursor_valid = DELETE_CURSOR_VALID_REG();
    vm_cursor *cursor = &VM.cursors[cursor_id];

    int32_t success = vmcursor_remove(cursor) ? 1 : 0;

    int32_t valid = vmcursor_is_valid(cursor) ? 1 : 0;

    typed_value src_success = typed_value::make(TYPE_U32, &success);
    typed_value src_valid = typed_value::make(TYPE_U32, &valid);

    set_register(&VM.registers[delete_occurred], &src_success);
    set_register(&VM.registers[cursor_valid], &src_valid);

    if (_debug) {
      printf("=> Cursor %d delete %s, cursor still valid=%d\n", cursor_id,
             success ? "SUCCESS" : "FAILED", valid);
    }

    VM.pc++;
    return OK;
  }
  case OP_Insert: {
    int32_t cursor_id = INSERT_CURSOR_ID();
    int32_t key_reg = INSERT_KEY_REG();

    vm_cursor *cursor = &VM.cursors[cursor_id];
    typed_value *first = &VM.registers[key_reg];
    uint32_t count = cursor->layout.columns.size() - 1;
    bool success;

    if (_debug) {
      printf("=> Cursor %d insert key=", cursor_id);
      type_print(first->type, first->data);
      printf(" with %d record values", count);
    }

    uint8_t data[cursor->layout.record_size];
    if (count >= 1) {
      build_record(data, key_reg + 1, count);
    }

    success = vmcursor_insert(cursor, (uint8_t *)first->data, data,
                              cursor->layout.record_size);

    if (_debug) {
      printf(" [");
      for (uint32_t i = 0; i < count; i++) {
        if (i > 0)
          printf(", ");
        typed_value *val = &VM.registers[key_reg + 1 + i];
        type_print(val->type, val->data);
      }
      printf("]");
    }

    if (_debug) {
      printf(", success=%d\n", success);
    }

    if (!success) {
      return ERR;
    }

    VM.pc++;
    return OK;
  }
  case OP_Update: {
    int32_t cursor_id = UPDATE_CURSOR_ID();
    int32_t record_reg = UPDATE_RECORD_REG();
    vm_cursor *cursor = &VM.cursors[cursor_id];
    uint8_t data[cursor->layout.record_size];
    uint32_t record_count = cursor->layout.columns.size() - 1;

    // record reg = key, but we need to build the record from the non key so
    // plus 1
    build_record(data, record_reg + 1, record_count);
    bool success = vmcursor_update(cursor, data);

    if (_debug) {
      printf("=> Cursor %d update with [", cursor_id);
      for (uint32_t i = 0; i < record_count; i++) {
        if (i > 0)
          printf(", ");
        typed_value *val = &VM.registers[record_reg + i];
        type_print(val->type, val->data);
      }
      printf("], success=%d\n", success);
    }

    VM.pc++;
    return OK;
  }

  case OP_Begin: {
    if (_debug) {
      printf("=> Beginning transaction\n");
    }
    pager_begin_transaction();
    VM.pc++;
    return OK;
  }
  case OP_Commit: {
    if (_debug) {
      printf("=> Committing transaction\n");
    }
    pager_commit();
    VM.pc++;
    return OK;
  }
  case OP_Rollback: {
    if (_debug) {
      printf("=> Rolling back transaction\n");
    }
    pager_rollback();
    return ABORT;
  }
  case OP_Pack: {

    /*
     * Packing and unpacking two datatypes to make composite keys
     * for use in indexes and multi order-by queries.
     */

    int32_t dest = PACK2_DEST_REG();
    int32_t left = PACK2_LEFT_REG();
    int32_t right = PACK2_RIGHT_REG();

    typed_value *a = &VM.registers[left];
    typed_value *b = &VM.registers[right];

    assert(!type_is_dual(a->type) && "Type to pack must not be a dual type");
    assert(!type_is_dual(b->type) && "Type to pack must not be a dual type");

    data_type dual_type = make_dual(a->type, b->type);
    uint32_t total_size = type_size(dual_type);

    uint8_t packed[total_size];

    pack_dual(packed, a->type, a->data, b->type, b->data);

    if (_debug) {
      printf("=> R[%d] = pack(", dest);
      type_print(a->type, a->data);
      printf(", ");
      type_print(b->type, b->data);
      printf(") -> ");
      type_print(dual_type, packed);
      printf(" (%s)\n", type_name(dual_type));
    }

    set_register(&VM.registers[dest], packed, dual_type);
    VM.pc++;
    return OK;
  }

  case OP_Unpack: {
    int32_t first_dest = UNPACK2_FIRST_DEST_REG();
    int32_t src = UNPACK2_SRC_REG();

    typed_value *dual_val = &VM.registers[src];

    assert(type_is_dual(dual_val->type) &&
           "Type to unpack must be a dual type");

    data_type type1 = dual_component_type(dual_val->type, 0);
    data_type type2 = dual_component_type(dual_val->type, 1);

    uint8_t data1[type_size(type1)];
    uint8_t data2[type_size(type2)];

    unpack_dual(dual_val->type, dual_val->data, data1, data2);

    if (_debug) {
      printf("=> unpack(");
      type_print(dual_val->type, dual_val->data);
      printf(") -> R[%d]=", first_dest);
      type_print(type1, data1);
      printf(" (%s), R[%d]=", type_name(type1), first_dest + 1);
      type_print(type2, data2);
      printf(" (%s)\n", type_name(type2));
    }

    set_register(&VM.registers[first_dest], data1, type1);
    set_register(&VM.registers[first_dest + 1], data2, type2);

    VM.pc++;
    return OK;
  }

  default:
    printf("Unknown opcode: %d\n", inst->opcode);
    return ERR;
  }
}

VM_RESULT
vm_execute(vm_instruction *instructions, int instruction_count) {
  reset();
  VM.program = instructions;
  VM.program_size = instruction_count;

  if (_debug) {
    vm_debug_print_program(instructions, instruction_count);
    printf("\n===== EXECUTION TRACE =====\n");
  }

  while (!VM.halted && VM.pc < VM.program_size) {
    VM_RESULT result = step();
    if (result != OK) {
      return result;
    }
  }

  if (_debug) {
    printf("\n\n===== EXECUTION COMPLETED =====\n");
    vm_debug_print_all_registers();
  }

  return OK;
}

void vm_set_result_callback(result_callback callback) {
  VM.emit_row = callback;
}
