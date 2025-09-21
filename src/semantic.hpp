/*
 * SQL From Scratch
 *
 * Semantic Analyzer
*/

#pragma once

#include "parser.hpp"
struct semantic_result
{
	bool		success;
	string_view error;
	string_view error_context;
};

semantic_result
semantic_analyze(stmt_node *stmt, bool modify_catalog);
