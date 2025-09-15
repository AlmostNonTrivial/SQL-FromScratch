/*
*  SQL From Scratch - Educational Database Engine
*
* Master Catalog (Schema Cache)
*
* The catalog is essentially an in-memory copy of the contents of the master catalog
* table, which contains data about other, user created tables.
* We want a version of this in memory because for each query we need to
* a) validate that a query is semantically valid, aka, does the table 'users'
* from 'select * from users;' actually exist, and b) how do we interpret
* the 'users' b+tree record format e.g., [u32][char16][char32][...], as opposed
* to literally any other configuration.
*
* If we didn't have an in-memory copy we'd need to do several I/Os on the master table
* just to validate a query in the first place.
*
* To build the catalog, we do exactly that, we first bootstrap the master catalog
* and then do SELECT * FROM master_catalog, and parse output to generate the catalog.
*
* We decide that the root of master_catalog always goes at page index 1. If it's a new database
* we'll create the master_catalog root, such that it will implicitly be placed at one. If it's an
* existing database we know to look for it at 1.
*
* Further alterations, like 'CREATE TABLE X' will directly update the catalog itself, but
* will also need to insert into the master_catalog table within the same transaction.
* On the event of a rollback for simplicity, we reload the catalog to avoid a more
* complicated sync mechanism.
*/

#pragma once
#include "btree.hpp"
#include "common.hpp"
#include "types.hpp"
#include "arena.hpp"
#include <cstdint>

// master catalog schema
#define MASTER_CATALOG "master_catalog"
#define MC_ID		   "id"
#define MC_NAME		   "name"
#define MC_TBL_NAME	   "tbl_name"
#define MC_ROOTPAGE	   "rootpage"
#define MC_SQL		   "sql"

// data_type char32's
#define ATTRIBUTE_NAME_MAX_SIZE 32
#define RELATION_NAME_MAX_SIZE	32

/*
 *
 * This arena holds all catalog data that survives across queries.
 * It is only reset when the catalog is reloaded (e.g., after rollback).
 */
struct catalog_arena
{
};

/*
 * Attribute aka column definition within a relation
 */
struct attribute
{
	char	  name[ATTRIBUTE_NAME_MAX_SIZE];
	data_type type;
};

/*
 * Relation aka schema definition for a table
 *
 * Holds the complete metadata for a table including its schema and
 * a handle to its btree storage. The relation itself lives in the
 * catalog arena and persists across queries.
 */
struct relation
{
	char		name[RELATION_NAME_MAX_SIZE];
	typed_value next_key; // Next auto-increment value (supports various key types)

	// Physical storage handle
	union {
		btree btree; // B+tree storage backend
					 // could add other storage backends here, like a persistent hash table
	} storage;

	array<attribute, catalog_arena> columns;
};

/*
 * The tuple_format is a runtime layout descriptor
 * created per-query to describe the physical layout of tuples being
 * processed. It can be derived from a relation's schema or created
 * independently for intermediate results (e.g., ORDER BY temp storage).
 *
 * Layout example for (id:u32, email:char32, name:char16, age:u16):
 *   columns: [TYPE_U32, TYPE_CHAR32, TYPE_CHAR16, TYPE_U16]
 *   offsets: [0, 32, 48]  // Offsets for email, name, age (id is the key)
 *   record_size: 50       // Total size of record portion
 *   key_type: TYPE_U32    // Type of the key (id)
 *
 * Note: The key is stored separately in the btree, so offsets start
 * from the first non-key column.
 */
struct tuple_format
{
	array<data_type, query_arena> columns;	   // All column types including key
	array<uint32_t, query_arena>  offsets;	   // Byte offsets (excluding key)
	uint32_t					  record_size; // Size of record (excluding key)
	data_type					  key_type;	   // Type of the key column
};


extern hash_map<fixed_string<RELATION_NAME_MAX_SIZE>, relation, catalog_arena> catalog;

void
catalog_reload();

tuple_format
tuple_format_from_types(array<data_type, query_arena> &columns);

tuple_format
tuple_format_from_relation(relation &schema);

relation
create_relation(string_view name, array<attribute, query_arena> columns);

void
bootstrap_master(bool is_new_database);
