/*
 * SQL From Scratch
 *
 * These showcase programs that there isn't the parser/compiler support for
 * but can be executed by the VM/backend.
 *
 * Demo Programs
 */

#include "demo.hpp"
#include "arena.hpp"
#include "catalog.hpp"
#include "common.hpp"
#include "compile.hpp"
#include "repl.hpp"
#include "types.hpp"
#include "vm.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
bool execute_sql_statements(const char *sql);
void formatted_result_callback(typed_value *result, size_t count);
void load_table_from_csv_sql(const char *csv_file, const char *table_name) {
  relation *structure = catalog.get(table_name);
  if (!structure) {
    return;
  }

  FILE *file = fopen(csv_file, "r");
  if (!file) {
    fprintf(stderr, "Failed to open CSV file: %s\n", csv_file);
    return;
  }

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size <= 0) {
    fprintf(stderr, "CSV file is empty: %s\n", csv_file);
    fclose(file);
    return;
  }

  char *file_buffer = (char *)arena<query_arena>::alloc(file_size + 1);
  if (!file_buffer) {
    fprintf(stderr, "Failed to allocate buffer for CSV file\n");
    fclose(file);
    return;
  }

  size_t bytes_read = fread(file_buffer, 1, file_size, file);
  fclose(file);

  if (bytes_read != (size_t)file_size) {
    fprintf(stderr, "Failed to read entire CSV file\n");
    return;
  }
  file_buffer[bytes_read] = '\0';

  auto column_stream = stream_writer<query_arena>::begin();
  for (uint32_t i = 0; i < structure->columns.size(); i++) {
    if (i > 0) {
      column_stream.write(", ");
    }
    column_stream.write(structure->columns[i].name);
  }
  string_view column_list = column_stream.finish().as_view();

  char *current = file_buffer;
  char *line_start = current;

  while (*current && *current != '\n') {
    current++;
  }
  if (*current == '\n') {
    current++;
  }

  int count = 0;
  int batch_count = 0;
  const int BATCH_SIZE = 50;

  while (*current) {
    line_start = current;

    char *line_end = current;
    while (*line_end && *line_end != '\n' && *line_end != '\r') {
      line_end++;
    }

    if (line_end == current) {
      if (*current == '\r')
        current++;
      if (*current == '\n')
        current++;
      continue;
    }

    char **fields = (char **)arena<query_arena>::alloc(
        sizeof(char *) * structure->columns.size());
    size_t field_count = 0;

    char *field_start = current;
    char *write_pos = nullptr;
    bool in_quotes = false;

    char *field_buffer =
        (char *)arena<query_arena>::alloc(line_end - current + 1);
    write_pos = field_buffer;

    while (current <= line_end) {
      if (*current == '"' && current < line_end) {
        if (!in_quotes) {
          in_quotes = true;
          field_start = current + 1;
          write_pos = field_buffer;
        } else if (*(current + 1) == '"') {

          *write_pos++ = '"';
          current++;
        } else {

          in_quotes = false;
        }
      } else if ((*current == ',' || current == line_end) && !in_quotes) {

        if (field_count < structure->columns.size()) {
          *write_pos = '\0';

          char *field_final = field_buffer;
          size_t field_len = write_pos - field_buffer;

          while (*field_final == ' ' || *field_final == '\t') {
            field_final++;
          }

          while (field_len > 0 && (field_final[field_len - 1] == ' ' ||
                                   field_final[field_len - 1] == '\t' ||
                                   field_final[field_len - 1] == '\r')) {
            field_final[--field_len] = '\0';
          }

          fields[field_count++] = field_final;

          field_buffer =
              (char *)arena<query_arena>::alloc(line_end - current + 1);
          write_pos = field_buffer;
          field_start = current + 1;
        }

        if (current == line_end) {
          break;
        }
      } else if (in_quotes || (*current != ',' && *current != '\r')) {

        *write_pos++ = *current;
      }

      current++;
    }

    current = line_end;
    if (*current == '\r') {
      current++;
    }
    if (*current == '\n') {
      current++;
    }

    if (field_count != structure->columns.size()) {
      printf("Warning: row has %zu fields, expected %zu\n", field_count,
             structure->columns.size());
      continue;
    }

    auto sql_stream = stream_writer<query_arena>::begin();
    sql_stream.write("INSERT INTO ");
    sql_stream.write(table_name);
    sql_stream.write(" (");
    sql_stream.write(column_list.data(), column_list.size());
    sql_stream.write(") VALUES (");

    for (size_t i = 0; i < field_count; i++) {
      if (i > 0) {
        sql_stream.write(", ");
      }

      data_type col_type = structure->columns[i].type;

      if (type_is_numeric(col_type)) {
        sql_stream.write(fields[i]);
      } else if (type_is_string(col_type)) {
        sql_stream.write("'");

        char *p = fields[i];
        while (*p) {
          if (*p == '\'') {
            sql_stream.write("''");
          } else {
            sql_stream.write(p, 1);
          }
          p++;
        }
        sql_stream.write("'");
      }
    }

    sql_stream.write(");");
    string_view sql_statement = sql_stream.finish().as_view();

    if (execute_sql_statements(sql_statement.data())) {
      count++;
    } else {
      printf("❌ Failed to insert row %d\n", count + 1);
    }

    if (++batch_count >= BATCH_SIZE) {
      batch_count = 0;
    }
  }
}

void create_all_tables_sql() {

  execute_sql_statements("BEGIN;");

  const char *create_users_sql = "CREATE TABLE users ("
                                 "user_id INT, "
                                 "username TEXT, "
                                 "email TEXT, "
                                 "age INT, "
                                 "city TEXT"
                                 ");";

  if (!execute_sql_statements(create_users_sql)) {
    return;
  }

  const char *create_products_sql = "CREATE TABLE products ("
                                    "product_id INT, "
                                    "title TEXT, "
                                    "category TEXT, "
                                    "price INT, "
                                    "stock INT, "
                                    "brand TEXT"
                                    ");";

  if (!execute_sql_statements(create_products_sql)) {
    return;
  }

  const char *create_orders_sql = "CREATE TABLE orders ("
                                  "order_id INT, "
                                  "user_id INT, "
                                  "total INT, "
                                  "total_quantity INT, "
                                  "discount INT"
                                  ");";

  if (!execute_sql_statements(create_orders_sql)) {
    return;
  }

  load_table_from_csv_sql("../users.csv", "users");
  load_table_from_csv_sql("../products.csv", "products");
  load_table_from_csv_sql("../orders.csv", "orders");

  execute_sql_statements("COMMIT;");
}
bool vmfunc_like(typed_value *result, typed_value *args, uint32_t arg_count) {
  assert(arg_count == 2 && "Expecting text and pattern");

  std::string_view text(args[0].as_char(),
                        strnlen(args[0].as_char(), type_size(args[0].type)));
  std::string_view pattern(args[1].as_char(),
                           strnlen(args[1].as_char(), type_size(args[1].type)));

  bool match = false;

  std::string_view literal = pattern.substr(1, pattern.size() - 2);
  match = (text.find(literal) != std::string_view::npos);

  result->type = TYPE_U32;
  result->data = (uint8_t *)arena<query_arena>::alloc(sizeof(uint32_t));
  *(uint32_t *)result->data = match ? 1 : 0;
  return true;
}

void demo_like_pattern(const char *args) {

  char pattern[32];
  if (args && *args) {
    strncpy(pattern, args, 32);
  } else {
    strcpy(pattern, "%osc%");
  }

  printf("\n=== LIKE Pattern Matching Demo ===\n");
  printf("Query: SELECT * FROM users WHERE username LIKE '%s'\n\n", pattern);

  program_builder prog;

  relation *users = catalog.get("users");

  if (!users) {
    printf("Products table not found!\n");
    return;
  }
  auto &cols = users->columns;

  vm_set_result_callback(formatted_result_callback);

  auto products_ctx = btree_cursor_from_relation(*users);
  int cursor = prog.open_cursor(products_ctx);

  int pattern_reg = prog.load_string(TYPE_CHAR32, pattern, 32);

  int at_end = prog.first(cursor);
  auto loop = prog.begin_while(at_end);
  {
    prog.regs.push_scope();

    int title_reg = prog.get_column(cursor, 1);

    int args_start = prog.regs.allocate_range(2);
    prog.move(title_reg, args_start);
    prog.move(pattern_reg, args_start + 1);
    int match_reg = prog.call_function(vmfunc_like, args_start, 2);

    auto if_match = prog.begin_if(match_reg);
    {
      int row = prog.get_columns(cursor, 0, users->columns.size());
      prog.result(row, users->columns.size());
    }
    prog.end_if(if_match);

    prog.next(cursor, at_end);
    prog.regs.pop_scope();
  }
  prog.end_while(loop);

  prog.close_cursor(cursor);
  prog.halt();
  prog.resolve_labels();

  vm_execute(prog.instructions.front(), prog.instructions.size());
}

void demo_nested_loop_join(const char *args) {
  vm_set_result_callback(formatted_result_callback);
  int limit = 0;

  if (args && *args) {
    sscanf(args, "%d", &limit);
  }

  printf("\n=== Nested Loop JOIN Demo ===\n");
  printf("Query: SELECT username, city, order_id, total FROM users JOIN orders "
         "ON users.user_id = orders.user_id");
  if (limit > 0) {
    printf(" LIMIT %d", limit);
  }

  printf("\n\n");

  program_builder prog;

  relation *users = catalog.get("users");
  relation *orders = catalog.get("orders");
  if (!users || !orders) {
    printf("Required tables not found!\n");
    return;
  }

  auto users_ctx = btree_cursor_from_relation(*users);
  auto orders_ctx = btree_cursor_from_relation(*orders);

  int users_cursor = prog.open_cursor(users_ctx);
  int orders_cursor = prog.open_cursor(orders_ctx);

  int count_reg = prog.load(TYPE_U32, 0U);

  int limit_reg = prog.load(TYPE_U32, limit);
  int one_reg = prog.load(TYPE_U32, 1U);

  int at_end_users = prog.first(users_cursor);
  auto outer_loop = prog.begin_while(at_end_users);
  {
    prog.regs.push_scope();
    int user_id = prog.get_column(users_cursor, 0);

    int at_end_orders = prog.first(orders_cursor);
    auto inner_loop = prog.begin_while(at_end_orders);
    {
      prog.regs.push_scope();

      if (limit > 0) {
        int limit_reached = prog.ge(count_reg, limit_reg);
        prog.jumpif_true(limit_reached, "done");
      }

      int order_user_id = prog.get_column(orders_cursor, 1);
      int match = prog.eq(user_id, order_user_id);

      auto if_match = prog.begin_if(match);
      {

        int result_start = prog.regs.allocate_range(4);
        int username = prog.get_column(users_cursor, 1);
        int city = prog.get_column(users_cursor, 4);
        int order_id = prog.get_column(orders_cursor, 0);
        int total = prog.get_column(orders_cursor, 2);

        prog.move(username, result_start);
        prog.move(city, result_start + 1);
        prog.move(order_id, result_start + 2);
        prog.move(total, result_start + 3);

        prog.result(result_start, 4);

        if (limit > 0) {
          prog.add(count_reg, one_reg, count_reg);
        }
      }
      prog.end_if(if_match);

      prog.next(orders_cursor, at_end_orders);
      prog.regs.pop_scope();
    }
    prog.end_while(inner_loop);

    prog.next(users_cursor, at_end_users);
    prog.regs.pop_scope();
  }
  prog.end_while(outer_loop);

  prog.label("done");
  prog.close_cursor(users_cursor);
  prog.close_cursor(orders_cursor);
  prog.halt();
  prog.resolve_labels();

  vm_execute(prog.instructions.front(), prog.instructions.size());
}

void demo_subquery_pattern(const char *args) {
  vm_set_result_callback(formatted_result_callback);
  int age = 30;
  char city[32] = "Chicago";

  if (args && *args) {
    char temp_city[32];
    int parsed = sscanf(args, "%d %31s", &age, temp_city);
    if (parsed == 2) {
      strcpy(city, temp_city);
    } else if (parsed == 1) {
    }
  }

  printf("\n=== Subquery Pattern Demo ===\n");
  printf("Query: SELECT * FROM (SELECT * FROM users WHERE age > %d) WHERE city "
         "= '%s'\n\n",
         age, city);

  program_builder prog;

  relation *users = catalog.get("users");
  if (!users) {
    printf("Users table not found!\n");
    return;
  }

  auto users_ctx = btree_cursor_from_relation(*users);
  tuple_format temp_layout = users_ctx->layout;
  auto temp_ctx = red_black_cursor_from_format(temp_layout);

  int users_cursor = prog.open_cursor(users_ctx);
  int temp_cursor = prog.open_cursor(temp_ctx);

  {
    prog.regs.push_scope();
    int age_const = prog.load(TYPE_U32, age);

    int at_end = prog.first(users_cursor);
    auto scan_loop = prog.begin_while(at_end);
    {
      prog.regs.push_scope();
      int age_reg = prog.get_column(users_cursor, 3);
      int age_test = prog.gt(age_reg, age_const);

      auto if_ctx = prog.begin_if(age_test);
      {
        int row_start =
            prog.get_columns(users_cursor, 0, users->columns.size());
        prog.insert_record(temp_cursor, row_start, users->columns.size());
      }
      prog.end_if(if_ctx);

      prog.next(users_cursor, at_end);
      prog.regs.pop_scope();
    }
    prog.end_while(scan_loop);
    prog.regs.pop_scope();
  }

  {
    prog.regs.push_scope();
    int city_const = prog.load_string(TYPE_CHAR32, city, 32);

    int at_end = prog.first(temp_cursor);
    auto scan_loop = prog.begin_while(at_end);
    {
      prog.regs.push_scope();
      int city_reg = prog.get_column(temp_cursor, 4);
      int city_test = prog.eq(city_reg, city_const);

      auto if_ctx = prog.begin_if(city_test);
      {
        int row_start = prog.get_columns(temp_cursor, 0, users->columns.size());
        prog.result(row_start, users->columns.size());
      }
      prog.end_if(if_ctx);

      prog.next(temp_cursor, at_end);
      prog.regs.pop_scope();
    }
    prog.end_while(scan_loop);
    prog.regs.pop_scope();
  }

  prog.close_cursor(users_cursor);
  prog.close_cursor(temp_cursor);
  prog.halt();
  prog.resolve_labels();

  vm_execute(prog.instructions.front(), prog.instructions.size());
}

void demo_group_by_aggregate(const char *args) {
  vm_set_result_callback(formatted_result_callback);
  bool show_avg = false;
  if (args && *args) {
    show_avg = (strcmp(args, "avg") == 0 || strcmp(args, "1") == 0);
  }
  printf("\n=== GROUP BY Aggregate Demo ===\n");
  if (show_avg) {
    printf("Query: SELECT city, COUNT(*), SUM(age), AVG(age) FROM users GROUP "
           "BY city\n\n");
  } else {
    printf(
        "Query: SELECT city, COUNT(*), SUM(age) FROM users GROUP BY city\n\n");
  }
  program_builder prog;
  relation *users = catalog.get("users");
  if (!users) {
    printf("Users table not found!\n");
    return;
  }

  array<data_type, query_arena> agg_types = {
      (TYPE_CHAR16),
      (TYPE_U32),
      (TYPE_U32),
  };
  tuple_format agg_layout = tuple_format_from_types(agg_types);
  auto users_ctx = btree_cursor_from_relation(*users);
  auto agg_ctx = red_black_cursor_from_format(agg_layout);
  int users_cursor = prog.open_cursor(users_ctx);
  int agg_cursor = prog.open_cursor(agg_ctx);

  {
    prog.regs.push_scope();
    int one_const = prog.load(TYPE_U32, 1U);
    int at_end = prog.first(users_cursor);
    auto scan_loop = prog.begin_while(at_end);
    {
      prog.regs.push_scope();
      int city_reg = prog.get_column(users_cursor, 4);
      int age_reg = prog.get_column(users_cursor, 3);

      int found = prog.seek(agg_cursor, city_reg, EQ);
      auto if_found = prog.begin_if(found);
      {

        int city_key = prog.get_column(agg_cursor, 0);
        int cur_count = prog.get_column(agg_cursor, 1);
        int cur_sum = prog.get_column(agg_cursor, 2);

        int update_start = prog.regs.allocate_range(3);
        prog.move(city_key, update_start);
        prog.add(cur_count, one_const, update_start + 1);
        prog.add(cur_sum, age_reg, update_start + 2);

        prog.update_record(agg_cursor, update_start);
      }
      prog.begin_else(if_found);
      {

        int insert_start = prog.regs.allocate_range(3);
        prog.move(city_reg, insert_start);
        prog.move(one_const, insert_start + 1);
        prog.move(age_reg, insert_start + 2);
        prog.insert_record(agg_cursor, insert_start, 3);
      }
      prog.end_if(if_found);
      prog.next(users_cursor, at_end);
      prog.regs.pop_scope();
    }
    prog.end_while(scan_loop);
    prog.regs.pop_scope();
  }

  {
    prog.regs.push_scope();
    int at_end = prog.first(agg_cursor);
    auto output_loop = prog.begin_while(at_end);
    {
      prog.regs.push_scope();
      if (show_avg) {

        int city = prog.get_column(agg_cursor, 0);
        int count = prog.get_column(agg_cursor, 1);
        int sum = prog.get_column(agg_cursor, 2);
        int avg = prog.div(sum, count);
        int result_start = prog.regs.allocate_range(4);
        prog.move(city, result_start);
        prog.move(count, result_start + 1);
        prog.move(sum, result_start + 2);
        prog.move(avg, result_start + 3);
        prog.result(result_start, 4);
      } else {

        int result_start = prog.get_columns(agg_cursor, 0, 3);
        prog.result(result_start, 3);
      }
      prog.next(agg_cursor, at_end);
      prog.regs.pop_scope();
    }
    prog.end_while(output_loop);
    prog.regs.pop_scope();
  }
  prog.close_cursor(users_cursor);
  prog.close_cursor(agg_cursor);
  prog.halt();
  prog.resolve_labels();

  vm_execute(prog.instructions.front(), prog.instructions.size());
}
