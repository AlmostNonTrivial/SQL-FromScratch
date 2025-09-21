# SQL-FromScratch

A toy database, written from scratch for educational value.

See parser.hpp for list of supported commands sql commands

See the youtube video for a primer on the codebase: https://www.youtube.com/watch?v=d9a3attUq3o


## Build Instructions

### Prerequisites
- C++23 compiler (GCC 11+, Clang 14+, or MSVC 2019+)
- CMake 3.15+

### Build
```bash
mkdir build && cd build
cmake ..
make

# Default database (relational_test.db)
./SqlFromScratch

# Custom database file
./SqlFromScratch mydata.db

# Run tests
./SqlFromScratch test

# Show help
./SqlFromScratch -h





## Architecture overview

┌─────────────────────────────────────────────────────────────┐
│                        SQL Query                            │
│                  "SELECT * FROM users"                      │
└────────────────────┬────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────┐
│                    PARSER (parser.cpp)                      │
│                                                             │
│  Lexical Analysis → Tokens → Recursive Descent → AST        │
│                                                             │
│  Output: Abstract Syntax Tree                               │
└────────────────────┬────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────┐
│              SEMANTIC ANALYZER (semantic.cpp)               │
│                                                             │
│  Validate tables, columns, types           ┌──────────────┐ │
│  Resolve names to IDs                      │   CATALOG    │ │
│  Type checking                             │ (catalog.cpp)│ │
│                                            │              │ │
│  Output: Validated & Annotated AST         │ Schema cache │ │
└────────────────────┬───────────────────────│ Table defs   │ │
                     │                       │ Column types │ │
                     ▼                       │ Index info   │ │
┌────────────────────────────────────────────└──────────────┘─│
│                  COMPILER (compile.cpp)                     │
│                                                             │
│  Transform AST → VM bytecode                                │
│  Resolve table/column IDs to storage locations              │
│  Select access methods (sequential scan vs index)           │
│                                                             │
│  Output: Bytecode Program + Execution Metadata              │
└────────────────────┬────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────┐
│              VIRTUAL MACHINE (vm.cpp)                       │
│                                                             │
│  Register-based VM                                          │
│  Executes bytecode instructions                             │
│  Interfaces with storage via cursors                        │
│                                                             │
└────────────────────┬────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────┐
│                    STORAGE LAYER                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │           CURSOR ABSTRACTION                         │   │
│  │     [Table, Index, and In-Memory cursors]            │   │
│  └──────────────────┬───────────────────────────────────┘   │
│                     │                                       │
│  ┌──────────────────▼───────────────────────────────────┐   │
│  │     B+TREE (btree.cpp)  │  BLOB STORE (blob.cpp)     │   │
│  └──────────────────┬──────────────┬────────────────────┘   │
│                     │              │                        │
│  ┌──────────────────▼──────────────▼────────────────────┐   │
│  │                 PAGER (pager.cpp)                    │   │
│  │         [Page cache, Rollback Journal, Free pages]   │   │
│  └──────────────────┬───────────────────────────────────┘   │
│                     │                                       │
│  ┌──────────────────▼───────────────────────────────────┐   │
│  │              OS LAYER (os_layer.cpp)                 │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘


## Example query execution

┌──────────────────────────────────────────────────────────────────────────────┐
│ SQL: SELECT name, age FROM users WHERE age > 30 ORDER BY name                │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ LAYER 1: LEXICAL ANALYSIS (Tokenization)                                     │
├──────────────────────────────────────────────────────────────────────────────┤
│ [SELECT]  [name]  [,]  [age]  [FROM]  [users]  [WHERE]  [age]  [>]  [30]     │
│    ↓        ↓     ↓     ↓       ↓       ↓        ↓       ↓     ↓     ↓       │
│ KEYWORD  IDENT  COMMA IDENT  KEYWORD  IDENT   KEYWORD  IDENT  OP  NUMBER     │
│                                                                              │
│ [ORDER]  [BY]  [name]                                                        │
│    ↓      ↓      ↓     // 'SELECT £ FROM... ' would raise a lexical error    │
│ KEYWORD KEYWORD IDENT                                                        │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ LAYER 2: SYNTAX ANALYSIS (Abstract Syntax Tree)                              │ I
├──────────────────────────────────────────────────────────────────────────────┤
│ SelectStmt {                                                                 │
│   table_name: "users"                                                        │
│   columns: ["name", "age"]  // 'SELECT FROM ..' would raise a syntax error   │
│   is_star: false                                                             │
│   order_by_column: "name"                                                    │
│   order_desc: false                                                          │
│   where_clause: ──────────┐                                                  │
│ }                         ▼                                                  │
│                    BinaryOp {                                                │
│                      op: GT (>)                                              │
│                      left: Column("age")                                     │
│                      right: Literal(30, TYPE_U32)                            │
│                    }                                                         │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ LAYER 3: SEMANTIC ANALYSIS (Type Resolution & Validation)                    │
├──────────────────────────────────────────────────────────────────────────────┤
│ Catalog Lookup:                                                              │
│ ┌────────────────────────────────────────────────┐                           │
│ │ Table: users                                   │                           │
│ │ Columns:                                       │                           │
│ │   [0] user_id  TYPE_U32    (PRIMARY KEY)       │                           │
│ │   [1] name     TYPE_CHAR32                     │                           │
│ │   [2] age      TYPE_U32                        │                           │
│ │   [3] city     TYPE_CHAR16                     │                           │
│ └────────────────────────────────────────────────┘                           │
│                                                                              │
│ Annotated AST:                                                               │
│ SelectStmt {                                                                 │
│   table_name: "users"                                                        │
│   columns: ["name", "age"]                                                   │
│   sem.column_indices: [1, 2]     ← Resolved                                  │
│   sem.order_by_index: 1          ← Column 1 = name                           │
│   where_clause: BinaryOp {                                                   │
│     left: Column {                                                           │
│       name: "age"                                                            │
│       sem.column_index: 2        ← Resolved                                  │
│       sem.resolved_type: TYPE_U32 ← Type checked                             │
│     }                                                                        │
│     right: Literal {                                                         │
│       value: 30                                                              │
│       sem.resolved_type: TYPE_U32 ← Compatible                               │
│     }                                                                        │
│   }                                                                          │
│ }   // 'SELECT * FROM fake_table' would raise a semantic error               │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ LAYER 4: COMPILATION (Bytecode Generation)                                   │
├──────────────────────────────────────────────────────────────────────────────┤
│ Register Allocation:                                                         │
│   R[0]: loop control (at_end flag)                                           │
│   R[1]: WHERE result                                                         │
│   R[2]: age value                                                            │
│   R[3]: literal 30                                                           │
│   R[4]: comparison result                                                    │
│   R[5]: name (for RB tree key)                                               │
│   R[6]: name (for result)                                                    │
│   R[7]: age (for result)                                                     │
│                                                                              │
│ Generated Bytecode:                                                          │
│ ┌────┬─────────────────────────────────────────────────────────────┐         │
│ │ PC │ Instruction                                                 │         │
│ ├────┼─────────────────────────────────────────────────────────────┤         │
│ │ 00 │ OPEN    C[0] red_black_tree  // Temp tree for ORDER BY      │         │
│ │ 01 │ OPEN    C[1] users_btree     // Main table cursor           │         │
│ │ 02 │ REWIND  C[1] → R[0]          // Position at first row       │         │
│ │ 03 │ LOAD    R[3] ← 30            // Load literal 30             │         │
│ │ 04 │ JUMPIF  R[0] @end            // Exit if empty table         │         │
│ │    │ ┌──── SCAN LOOP ────┐                                       │         │
│ │ 05 │ │ COLUMN  C[1] 2 → R[2]      // Get age (column 2)          │         │
│ │ 06 │ │ TEST    R[4] ← R[2] > R[3] // Compare age > 30            │         │
│ │ 07 │ │ JUMPIF  !R[4] @skip        // Skip if WHERE false         │         │
│ │    │ │   ┌── COLLECT ──┐                                         │         │
│ │ 08 │ │   │ COLUMN C[1] 1 → R[5]   // Get name for sorting        │         │
│ │ 09 │ │   │ COLUMN C[1] 1 → R[6]   // Get name for output         │         │
│ │ 10 │ │   │ COLUMN C[1] 2 → R[7]   // Get age for output          │         │
│ │ 11 │ │   │ INSERT C[0] R[5:7]     // Insert into RB tree         │         │
│ │    │ │   └─────────────┘                                         │         │
│ │ 12 │ │ @skip:                                                    │         │
│ │ 13 │ │ STEP   C[1] → R[0]         // Next row                    │         │
│ │ 14 │ └ GOTO   @05                 // Continue scan               │         │
│ │ 15 │ @end:                                                       │         │
│ │    │ ┌──── OUTPUT LOOP ────┐                                     │         │
│ │ 16 │ │ REWIND C[0] → R[0]         // Start of sorted results     │         │
│ │ 17 │ │ JUMPIF R[0] @done          // Exit if no results          │         │
│ │ 18 │ │ @output:                                                  │         │
│ │ 19 │ │ COLUMN C[0] 1 → R[6]       // Get name from RB tree       │         │
│ │ 20 │ │ COLUMN C[0] 2 → R[7]       // Get age from RB tree        │         │
│ │ 21 │ │ RESULT R[6:7]              // Output row                  │         │
│ │ 22 │ │ STEP   C[0] → R[0]         // Next sorted row             │         │
│ │ 23 │ └ GOTO   @output             // Continue output             │         │
│ │ 24 │ @done:                                                      │         │
│ │ 25 │ CLOSE   C[0]                 // Close RB cursor             │         │
│ │ 26 │ CLOSE   C[1]                 // Close table cursor          │         │
│ │ 27 │ HALT    0                    // Success                     │         │
│ └────┴─────────────────────────────────────────────────────────────┘         │
│                                                                              │
│ After full table scan (PC[16]):                                              │
│                                                                              │
│ Red-Black Tree (sorted by name):                                             │
│                    ("Charlie",42)                                            │
│                    /            \                                            │
│             ("Alice",35)    ("David",31)                                     │
│                    \                                                         │
│                 ("Bob",45)                                                   │
│                                                                              │
│ In-order traversal: Alice→Bob→Charlie→David                                  │
└──────────────────────────────────────────────────────────────────────────────┘
