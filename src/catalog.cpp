/*
 *  SQL From Scratch - Educational Database Engine
 *
 * Master Catalog (Schema Cache)
 */

#include "catalog.hpp"
#include "arena.hpp"
#include "common.hpp"
#include "arena.hpp"
#include "pager.hpp"
#include "semantic.hpp"
#include "types.hpp"
#include <cassert>
#include <cstring>
#include <string_view>
#include "compile.hpp"

hash_map<fixed_string<RELATION_NAME_MAX_SIZE>, relation, catalog_arena> catalog;

/*
 * Creates a format descriptor for tuples with the given column types.
 * The first column is treated as the key and stored separately in the
 * btree, so offsets begin from the second column.
 */
tuple_format
tuple_format_from_types(array<data_type, query_arena> &columns)
{
	tuple_format format = {};

	// First column is always the key
	format.key_type = columns[0];
	for (auto col : columns)
	{
		format.columns.push(col);
	}

	// Calculate offsets for record portion (excluding key)
	// The key is stored separately in the btree node
	int offset = 0;
	format.offsets.push(0); // First record column starts at offset 0

	// Start from 1 because column 0 is the key
	for (int i = 1; i < columns.size(); i++)
	{
		offset += type_size(columns[i]);
		format.offsets.push(offset);
	}

	format.record_size = offset;
	return format;
}

tuple_format
tuple_format_from_relation(relation &schema)
{
	array<data_type, query_arena> column_types;

	for (auto &col : schema.columns)
	{
		column_types.push(col.type);
	}

	return tuple_format_from_types(column_types);
}

relation
create_relation(string_view name, array<attribute, query_arena> columns)
{
	relation rel = {};

	for (auto &col : columns)
	{
		rel.columns.push(col);
	}

	sv_to_cstr(name, rel.name, RELATION_NAME_MAX_SIZE);
	return rel;
}

void
bootstrap_master(bool is_new_database)
{

	array<attribute, query_arena> master_columns = {{MC_ID, TYPE_U32},
													{MC_NAME, TYPE_CHAR32},
													{MC_TBL_NAME, TYPE_CHAR32},
													{MC_ROOTPAGE, TYPE_U32},
													{MC_SQL, TYPE_CHAR256}};

	relation master_table = create_relation(MASTER_CATALOG, master_columns);
	master_table.next_key.type = TYPE_U32;
	master_table.next_key.data = arena<catalog_arena>::alloc(type_size(TYPE_U32));
	type_zero(master_table.next_key.type, master_table.next_key.data);

	tuple_format layout = tuple_format_from_relation(master_table);

	if (is_new_database)
	{
		pager_begin_transaction();
		master_table.storage.btree = bt_create(layout.key_type, layout.record_size, is_new_database);

		assert(1 == master_table.storage.btree.root_page_index &&
			   "Master catalog MUST be at page 1 so it can be found on start-up");

		pager_commit();
	}
	else
	{
		master_table.storage.btree = bt_create(layout.key_type, layout.record_size, is_new_database);
		master_table.storage.btree.root_page_index = 1;
	}

	catalog.insert(MASTER_CATALOG, master_table);
}

/*
 * The output of the from the SELECT * FROM master_catalog is inserted into the
 * catalog
 */
void catalog_reload_callback(typed_value *result, size_t count) {
  if (count != 5) {
    return;
  }

  const uint32_t key = result[0].as_u32();
  const char *name = result[1].as_char();
  const char *tbl_name = result[2].as_char();
  uint32_t rootpage = result[3].as_u32();
  const char *sql = result[4].as_char();

  if (strcmp(name, MASTER_CATALOG) == 0) {
    return;
  }

  auto master = catalog.get(MASTER_CATALOG);
  if (master->next_key.as_u32() <= key) {
    *(uint32_t *)(master->next_key.data) = key + 1;
  }

  // parse 'CREATE TABLE users (INT user_id, TEXT username ...) -> attributes
  stmt_node *stmt = parse_sql(sql).statements[0];
  array<attribute, query_arena> columns;

  if (strcmp(tbl_name, name) == 0) {

    create_table_stmt &create_stmt = stmt->create_table_stmt;
    columns.reserve(create_stmt.columns.size());

    for (uint32_t i = 0; i < create_stmt.columns.size(); i++) {
      attribute_node &col_def = create_stmt.columns[i];
      attribute col;
      col.type = col_def.type;
      sv_to_cstr(col_def.name, col.name, ATTRIBUTE_NAME_MAX_SIZE);
      columns.push(col);
    }
  }

  relation structure = create_relation(name, columns);
  tuple_format format = tuple_format_from_relation(structure);

  structure.storage.btree =
      bt_create(format.key_type, format.record_size, false);
  structure.storage.btree.root_page_index = rootpage;

  catalog.insert(name, structure);
}


void
load_catalog_from_master()
{
	vm_set_result_callback(catalog_reload_callback);

	parser_result result = parse_sql("SELECT * FROM master_catalog");
	assert(result.success && result.statements.size() == 1);
	semantic_analyze(result.statements[0], true);

	auto program = compile_program(result.statements[0]);

	vm_execute(program.front(), program.size());
}

void
catalog_reload()
{
	arena<catalog_arena>::reset_and_decommit();
	catalog.clear();

	bootstrap_master(false);

	load_catalog_from_master();
}
