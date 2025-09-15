/*
 * SQL From Scratch - Educational Database Engine
 *
 * Read Execute Print Loop
 */

#pragma once

#include "catalog.hpp"
int
run_repl(const char *database_path);
bool
execute_sql_statements(const char *sql);
void
setup_result_formatting(std::initializer_list<attribute> columns);
