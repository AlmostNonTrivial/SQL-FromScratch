/*
 * SQL From Scratch
 *
 * VM Bytecode Compiler
 *
 * The compiler takes the annotated AST, which has resolved that the query is
 * targeting valid tables/columns etc, and turns it into program that the vm can
 * execute.
 *
 * Our 'program' is an array of vm_instructions, each having an op_code and
 * various parameters that we can utilize. Look at vm.hpp/cpp to see what
 * parameters need to go where.
 *
 * This part of the project is the least developed: The VM is capable of doing
 * most queries but the actual compilation those programs from an AST is quite
 * involved.
 *
 * The implementation can compile the subset of SQL specified in parser.hpp, but
 * in demo.hpp you can see hand-rolled programs that do more advanced queries.
 *
 * The only 'optimization' that has been implemented, is that with selects, if
 * our expression involves a primary key, we do a seek, and if the op is '='
 * then, because we know primary keys are unique, we exit immediately after the
 * op is finished.
 */
#pragma once
#include "compile.hpp"
#include "arena.hpp"
#include "catalog.hpp"
#include "common.hpp"
#include "parser.hpp"
#include "types.hpp"
#include "vm.hpp"
#include <cstdint>
#include <cstring>
#include <string_view>

cursor_context *btree_cursor_from_relation(relation &structure) {
  cursor_context *cctx =
      (cursor_context *)arena<query_arena>::alloc(sizeof(cursor_context));
  cctx->storage.tree = &structure.storage.btree;
  cctx->type = BPLUS;
  cctx->layout = tuple_format_from_relation(structure);
  return cctx;
}

cursor_context *red_black_cursor_from_format(tuple_format &layout,
                                             bool allow_duplicates) {
  cursor_context *cctx =
      (cursor_context *)arena<query_arena>::alloc(sizeof(cursor_context));
  cctx->type = RED_BLACK;
  cctx->layout = layout;
  cctx->flags = allow_duplicates;
  return cctx;
}

static int compile_literal(program_builder *prog, expr_node *expr) {
  switch (expr->lit_type) {
  case TYPE_U32:
    return prog->load(expr->sem.resolved_type, expr->int_val);
  case TYPE_CHAR32:
    return prog->load_string(expr->sem.resolved_type, expr->str_val.data(),
                             expr->str_val.size());
  }

  assert(false);
}

static int compile_expr(program_builder *prog, expr_node *expr, int cursor_id) {
  switch (expr->type) {
  case EXPR_COLUMN:
    return prog->get_column(cursor_id, expr->sem.column_index);

  case EXPR_LITERAL:
    return compile_literal(prog, expr);

  case EXPR_BINARY_OP: {
    int left_reg = compile_expr(prog, expr->left, cursor_id);
    int right_reg = compile_expr(prog, expr->right, cursor_id);

    switch (expr->op) {
    case OP_EQ:
      return prog->eq(left_reg, right_reg);
    case OP_NE:
      return prog->ne(left_reg, right_reg);
    case OP_LT:
      return prog->lt(left_reg, right_reg);
    case OP_LE:
      return prog->le(left_reg, right_reg);
    case OP_GT:
      return prog->gt(left_reg, right_reg);
    case OP_GE:
      return prog->ge(left_reg, right_reg);
    case OP_AND:
      return prog->logic_and(left_reg, right_reg);
    case OP_OR:
      return prog->logic_or(left_reg, right_reg);
    default:
      assert(false);
    }
  }

  case EXPR_UNARY_OP: {
    int operand_reg = compile_expr(prog, expr->operand, cursor_id);
    if (expr->unary_op == OP_NOT) {

      int one = prog->load(TYPE_U32, 1U);
      return prog->sub(one, operand_reg);
    }
    return operand_reg;
  }
  case EXPR_NULL: {
    assert(false);
  }

  default:
    assert(false);
  }
}

/*
 * When the VM calls this function, the new table schema is already in
 * the catalog, so we can create our btree from it (key, record_size).
 */
static bool vmfunc_create_relation(typed_value *result, typed_value *args,
                                   uint32_t arg_count) {
  const char *table_name = args[0].as_char();

  relation *rel = catalog.get(table_name);

  assert(rel && "Relation should already be in the catalog");

  tuple_format layout = tuple_format_from_relation(*rel);
  rel->storage.btree = bt_create(layout.key_type, layout.record_size, true);

  result->type = TYPE_U32;
  result->data = arena<query_arena>::alloc(sizeof(uint32_t));
  // return the root page of the newly created btree, so that we can insert it
  // into the master catalog
  *(uint32_t *)result->data = rel->storage.btree.root_page_index;
  return true;
}

static bool vmfunc_drop_relation(typed_value *result, typed_value *args,
                                 uint32_t arg_count) {
  if (arg_count != 1) {
    return false;
  }

  const char *name = args[0].as_char();
  relation *rel = catalog.get(name);

  assert(rel &&
         "Relation should still be in the catalog until we remove it here");

  bt_clear(&rel->storage.btree);

  catalog.remove(name);

  result->type = TYPE_U32;
  result->data = arena<query_arena>::alloc(sizeof(uint32_t));
  *(uint32_t *)result->data = 1;
  return true;
}

enum SEEK_STRATEGY_TYPE : uint8_t {
  STRATEGY_FULL_SCAN,    // full table scan
  STRATEGY_SEEK_SCAN,    // seek to position, then scan
  STRATEGY_DIRECT_LOOKUP // direct key lookup (for PK with EQ)
};

struct seek_strategy {
  SEEK_STRATEGY_TYPE type;
  COMPARISON_OP op;
  expr_node *key_expr;
  bool scan_forward; // direction for scanning after seek
};

/*
 * Recursively analysize the expression tree to see if there is a primary key
 * condition. If there is, then because the table is sorted on the primary key,
 * we can use a seek to either:
 * - Go to the only row that it could possibly satisfy, for example, 'WHERE
 * user_id = 4 AND age > 30' can only be satisfied by a row with user_id 4, so
 * seek directly to it, THEN, test the other condition(s) and exit.
 * - Seek to the the first row that satisfies the primary key condition, then
 * evaluate the other conditions until we reach the end of the table. This cuts
 * down the search space in proportion to the selectivity of the primary key
 * condition. If there are users with id's 1-1000, then doing 'WHERE user_id >=
 * 900' reduces the rows processed down to 1/10th of the original.
 */
static seek_strategy analyze_where_clause(expr_node *where_clause,
                                          relation *table) {
  seek_strategy strategy;
  strategy.type = STRATEGY_FULL_SCAN;
  strategy.scan_forward = true;
  strategy.op = EQ;
  strategy.key_expr = nullptr;

  if (!where_clause || !table) {
    return strategy;
  }

  // check if this is a direct PK comparison
  if (where_clause->type == EXPR_BINARY_OP &&
      where_clause->left->type == EXPR_COLUMN &&
      where_clause->left->sem.column_index == 0 &&
      where_clause->right->type == EXPR_LITERAL) {

    strategy.key_expr = where_clause->right;

    switch (where_clause->op) {
    case OP_EQ:
      strategy.op = EQ;
      strategy.type = STRATEGY_DIRECT_LOOKUP;
      break;
    case OP_LT:
      strategy.op = LT;
      strategy.type = STRATEGY_SEEK_SCAN;
      strategy.scan_forward = false;
      break;
    case OP_LE:
      strategy.op = LE;
      strategy.type = STRATEGY_SEEK_SCAN;
      strategy.scan_forward = false;
      break;
    case OP_GT:
      strategy.op = GT;
      strategy.type = STRATEGY_SEEK_SCAN;
      strategy.scan_forward = true;
      break;
    case OP_GE:
      strategy.op = GE;
      strategy.type = STRATEGY_SEEK_SCAN;
      strategy.scan_forward = true;
      break;
    default:
      return strategy;
    }

    // remove this predicate from the tree it's now handled by seek
    where_clause->type = EXPR_LITERAL;
    where_clause->lit_type = TYPE_U32;
    where_clause->int_val = 1; // replace with true
    return strategy;
  }

  /*
   * Check for AND with PK comparison on one side
   * if we find it, remove it from the expression tree and set it as expr_key
   * which means we don't have to evaluate it after the seek
   */
  if (where_clause->type == EXPR_BINARY_OP && where_clause->op == OP_AND) {
    seek_strategy left_strategy =
        analyze_where_clause(where_clause->left, table);
    if (left_strategy.type != STRATEGY_FULL_SCAN) {

      *where_clause = *where_clause->right;
      return left_strategy;
    }

    seek_strategy right_strategy =
        analyze_where_clause(where_clause->right, table);
    if (right_strategy.type != STRATEGY_FULL_SCAN) {

      *where_clause = *where_clause->left;
      return right_strategy;
    }
  }

  return strategy;
}

array<vm_instruction, query_arena> compile_select(stmt_node *stmt) {
  program_builder prog;
  select_stmt *select_stmt = &stmt->select_stmt;

  relation *table = catalog.get(select_stmt->table_name);
  auto table_ctx = btree_cursor_from_relation(*table);
  int table_cursor = prog.open_cursor(table_ctx);

  seek_strategy strategy =
      analyze_where_clause(select_stmt->where_clause, table);

  // handle direct lookup case separately (no ORDER BY needed)
  if (strategy.type == STRATEGY_DIRECT_LOOKUP) {
    int key_reg = compile_literal(&prog, strategy.key_expr);

    int found = prog.seek(table_cursor, key_reg, EQ);

    auto found_block = prog.begin_if(found);
    {

      int result_count = select_stmt->sem.column_indices.size();
      int result_start = prog.regs.allocate_range(result_count);

      for (uint32_t i = 0; i < result_count; i++) {
        prog.get_column(table_cursor, select_stmt->sem.column_indices[i],
                        result_start + i);
      }

      prog.result(result_start, result_count);
    }
    prog.end_if(found_block);

    prog.close_cursor(table_cursor);
    prog.halt();
    prog.resolve_labels();
    return prog.instructions;
  }

  // setup for ORDER BY if needed
  bool has_order_by = select_stmt->sem.rb_format.columns.size() > 0;
  int result_count = select_stmt->sem.column_indices.size();
  if (has_order_by) {
    result_count++;
  }

  int rb_cursor = -1;
  if (has_order_by) {
    auto rb_ctx =
        red_black_cursor_from_format(select_stmt->sem.rb_format, true);
    rb_cursor = prog.open_cursor(rb_ctx);
  }

  int at_end_register;

  if (strategy.type == STRATEGY_SEEK_SCAN) {
    int key_reg = compile_literal(&prog, strategy.key_expr);
    // seek returns 1 if found, 0 if not. For a range scan
    // not found means we're at the end of the table
    at_end_register = prog.seek(table_cursor, key_reg, strategy.op);
  } else {
    // start from beginning
    at_end_register = prog.first(table_cursor);
  }

  auto scan_loop = prog.begin_while(at_end_register);
  {
    prog.regs.push_scope();

    conditional_context where_ctx;
    if (select_stmt->where_clause) {
      int where_result =
          compile_expr(&prog, select_stmt->where_clause, table_cursor);
      where_ctx = prog.begin_if(where_result);
    }

    int result_start = prog.regs.allocate_range(result_count);

    if (has_order_by) {
      prog.get_column(table_cursor, select_stmt->sem.order_by_index,
                      result_start);
    }

    uint32_t offset = has_order_by ? 1 : 0;
    for (uint32_t i = 0; i < result_count - offset; i++) {
      prog.get_column(table_cursor, select_stmt->sem.column_indices[i],
                      result_start + offset + i);
    }

    if (has_order_by) {
      prog.insert_record(rb_cursor, result_start, result_count);
    } else {
      prog.result(result_start, result_count);
    }

    if (select_stmt->where_clause) {
      prog.end_if(where_ctx);
    }

    if (strategy.type == STRATEGY_SEEK_SCAN && !strategy.scan_forward) {
      prog.prev(table_cursor, at_end_register);
    } else {
      prog.next(table_cursor, at_end_register);
    }

    prog.regs.pop_scope();
  }
  prog.end_while(scan_loop);

  prog.close_cursor(table_cursor);

  if (has_order_by) {
    int rb_at_end =
        select_stmt->order_desc ? prog.last(rb_cursor) : prog.first(rb_cursor);

    auto output_loop = prog.begin_while(rb_at_end);
    {
      prog.regs.push_scope();

      int output_count = select_stmt->sem.column_indices.size();
      int output_start = prog.get_columns(rb_cursor, 1, output_count);
      prog.result(output_start, output_count);

      if (select_stmt->order_desc) {
        prog.prev(rb_cursor, rb_at_end);
      } else {
        prog.next(rb_cursor, rb_at_end);
      }

      prog.regs.pop_scope();
    }
    prog.end_while(output_loop);

    prog.close_cursor(rb_cursor);
  }

  prog.halt();
  prog.resolve_labels();
  return prog.instructions;
}
array<vm_instruction, query_arena> compile_insert(stmt_node *stmt) {
  program_builder prog;
  insert_stmt *insert_stmt = &stmt->insert_stmt;

  relation *table = catalog.get(insert_stmt->table_name);
  auto table_ctx = btree_cursor_from_relation(*table);
  int cursor = prog.open_cursor(table_ctx);

  int row_size = table->columns.size();
  int row_start = prog.regs.allocate_range(row_size);

  for (uint32_t i = 0; i < insert_stmt->values.size(); i++) {
    expr_node *expr = insert_stmt->values[i];
    uint32_t col_idx = insert_stmt->sem.column_indices[i];

    int value_reg;
    if (expr->type == EXPR_LITERAL) {
      value_reg = compile_literal(&prog, expr);
    }

    prog.move(value_reg, row_start + col_idx);
  }

  prog.insert_record(cursor, row_start, row_size);

  prog.close_cursor(cursor);

  prog.halt();
  prog.resolve_labels();

  return prog.instructions;
}

array<vm_instruction, query_arena> compile_update(stmt_node *stmt) {
  program_builder prog;
  update_stmt *update_stmt = &stmt->update_stmt;

  relation *table = catalog.get(update_stmt->table_name);
  auto table_ctx = btree_cursor_from_relation(*table);
  int cursor = prog.open_cursor(table_ctx);

  int at_end = prog.first(cursor);

  auto scan_loop = prog.begin_while(at_end);
  {
    prog.regs.push_scope();

    conditional_context where_ctx;
    if (update_stmt->where_clause) {
      int where_result = compile_expr(&prog, update_stmt->where_clause, cursor);
      where_ctx = prog.begin_if(where_result);
    }

    int row_start = prog.get_columns(cursor, 0, table->columns.size());

    for (uint32_t i = 0; i < update_stmt->columns.size(); i++) {
      uint32_t col_idx = update_stmt->sem.column_indices[i];
      expr_node *value_expr = update_stmt->values[i];

      int new_value;
      if (value_expr->type == EXPR_LITERAL) {
        new_value = compile_literal(&prog, value_expr);
      }

      prog.move(new_value, row_start + col_idx);
    }

    prog.update_record(cursor, row_start);

    if (update_stmt->where_clause) {
      prog.end_if(where_ctx);
    }

    prog.next(cursor, at_end);
    prog.regs.pop_scope();
  }
  prog.end_while(scan_loop);

  prog.close_cursor(cursor);

  prog.halt();
  prog.resolve_labels();

  return prog.instructions;
}

array<vm_instruction, query_arena> compile_delete(stmt_node *stmt) {
  program_builder prog;
  delete_stmt *delete_stmt = &stmt->delete_stmt;

  relation *table = catalog.get(delete_stmt->table_name);
  auto table_ctx = btree_cursor_from_relation(*table);
  int cursor = prog.open_cursor(table_ctx);

  int at_end = prog.first(cursor);

  auto scan_loop = prog.begin_while(at_end);
  {
    prog.regs.push_scope();

    int should_delete;
    if (delete_stmt->where_clause) {
      should_delete = compile_expr(&prog, delete_stmt->where_clause, cursor);
    } else {
      should_delete = prog.load(TYPE_U32, 1U);
    }

    auto delete_if = prog.begin_if(should_delete);
    {
      int deleted = prog.regs.allocate();
      int still_valid = prog.regs.allocate();
      prog.delete_record(cursor, deleted, still_valid);

      auto if_valid = prog.begin_if(still_valid);
      {

        prog.move(still_valid, at_end);
      }
      prog.begin_else(if_valid);
      {

        prog.first(cursor, at_end);
      }
      prog.end_if(if_valid);
    }
    prog.begin_else(delete_if);
    {

      prog.next(cursor, at_end);
    }
    prog.end_if(delete_if);

    prog.regs.pop_scope();
  }
  prog.end_while(scan_loop);

  prog.close_cursor(cursor);

  prog.halt();
  prog.resolve_labels();

  return prog.instructions;
}

array<vm_instruction, query_arena> compile_create_table(stmt_node *stmt) {
  program_builder prog;
  create_table_stmt *create_stmt = &stmt->create_table_stmt;

  int table_name_reg =
      prog.load_string(TYPE_CHAR32, create_stmt->table_name.data(),
                       create_stmt->table_name.size());
  int root_page_reg =
      prog.call_function(vmfunc_create_relation, table_name_reg, 1);

  relation &master = *catalog.get(MASTER_CATALOG);
  auto master_ctx = btree_cursor_from_relation(master);
  int master_cursor = prog.open_cursor(master_ctx);

  int row_start = prog.regs.allocate_range(5);

  prog.load_ptr(master.next_key.data, row_start);

  type_increment(master.next_key.type, master.next_key.data,
                 master.next_key.data);

  prog.load_string(TYPE_CHAR32, create_stmt->table_name.data(),
                   create_stmt->table_name.size(), row_start + 1);

  prog.load_string(TYPE_CHAR32, create_stmt->table_name.data(),
                   create_stmt->table_name.size(), row_start + 2);

  prog.move(root_page_reg, row_start + 3);

  string_view sql = stmt->sql_stmt;

  prog.load_string(TYPE_CHAR256, sql.data(), sql.size(), row_start + 4);

  prog.insert_record(master_cursor, row_start, 5);

  prog.close_cursor(master_cursor);

  prog.halt();

  prog.resolve_labels();

  return prog.instructions;
}

array<vm_instruction, query_arena> compile_drop_table(stmt_node *stmt) {
  program_builder prog;
  drop_table_stmt *drop_stmt = &stmt->drop_table_stmt;

  int name_reg = prog.load_string(TYPE_CHAR32, drop_stmt->table_name.data(),
                                  drop_stmt->table_name.size());
  prog.call_function(vmfunc_drop_relation, name_reg, 1);

  relation &master = *catalog.get(MASTER_CATALOG);
  auto master_ctx = btree_cursor_from_relation(master);
  int cursor = prog.open_cursor(master_ctx);

  int at_end = prog.first(cursor);
  auto scan_loop = prog.begin_while(at_end);
  {
    prog.regs.push_scope();

    int entry_name = prog.get_column(cursor, 1);
    int matches = prog.eq(entry_name, name_reg);

    auto delete_if = prog.begin_if(matches);
    {
      int deleted = prog.regs.allocate();
      int still_valid = prog.regs.allocate();
      prog.delete_record(cursor, deleted, still_valid);
      prog.goto_label("done");
    }
    prog.end_if(delete_if);

    prog.next(cursor, at_end);
    prog.regs.pop_scope();
  }
  prog.end_while(scan_loop);

  prog.label("done");
  prog.close_cursor(cursor);

  prog.halt();
  prog.resolve_labels();

  return prog.instructions;
}

array<vm_instruction, query_arena> compile_begin() {
  program_builder prog;
  prog.begin_transaction();
  prog.halt();
  return prog.instructions;
}

array<vm_instruction, query_arena> compile_commit() {
  program_builder prog;
  prog.commit_transaction();
  prog.halt();
  return prog.instructions;
}

array<vm_instruction, query_arena> compile_rollback() {
  program_builder prog;
  prog.rollback_transaction();
  prog.halt();
  return prog.instructions;
}

array<vm_instruction, query_arena> compile_program(stmt_node *stmt) {

  switch (stmt->type) {
  case STMT_SELECT:
    return compile_select(stmt);
  case STMT_INSERT:
    return compile_insert(stmt);
  case STMT_UPDATE:
    return compile_update(stmt);
  case STMT_DELETE:
    return compile_delete(stmt);
  case STMT_CREATE_TABLE:
    return compile_create_table(stmt);
  case STMT_DROP_TABLE:
    return compile_drop_table(stmt);
  case STMT_BEGIN:
    return compile_begin();
  case STMT_COMMIT:
    return compile_commit();
  case STMT_ROLLBACK:
    return compile_rollback();
  }

  assert(false && "Invalid program");
}
