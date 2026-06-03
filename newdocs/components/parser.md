# Component: Parser (`googlesql/parser/`)

**Role:** Turn SQL text into a syntactic **parse tree** (an `ASTNode` tree). No
types, no name resolution, no catalog — purely grammar. This is the first stage
of the pipeline; its output feeds the [analyzer](analyzer.md).

The parse tree is *semi-public*: it is exposed to engines/tools but is **not a
stable API** (unlike the Resolved AST).

## Pipeline within the parser

```
 SQL text
   │
   ▼  macro expansion (macros/)          token-level, before parsing
 tokens (TextMapper lexer from googlesql.tm)
   │
   ▼  lookahead_transformer (disambiguate context-sensitive keywords)
 parser_internal.cc  (builds nodes via ast_node_factory, arena-allocated)
   │
   ▼
 ASTNode tree  (parse_tree_generated.h classes)
```

## Lexing & grammar

- **`googlesql.tm`** — the single source of truth for **both the lexer and the
  parser grammar**, written in **TextMapper** grammar syntax (~400KB:
  `language sql(cc);`). It defines tokens, string/identifier/comment rules,
  keywords, *and* the grammar productions with their semantic actions. A Bazel
  rule (`tm_syntax` in `bazel/textmapper.bzl`) runs TextMapper to generate a
  bison-style parser (`bison_parser_generated_lib`) plus the lexer.
- **`textmapper_lexer_adapter.*`, `tokenizer.*`, `token_with_location.*`** —
  adapt the generated lexer to the parser and attach source locations (for error
  messages).
- **`keywords.*`** — reserved/non-reserved keyword tables.
- **`lookahead_transformer.*`** — resolves context-sensitive keywords (e.g.
  `QUALIFY`, graph keywords) that can't be decided by the lexer alone.

> So the grammar lives in `googlesql.tm`; `parser_internal.{h,cc}` provides the
> C++ entry points, glue, and error recovery around the generated parser, and the
> grammar's semantic actions build `ASTNode`s via `ast_node_factory`.

## The AST and code generation

The ~300 concrete node classes are **code-generated**, not hand-written:

- **`gen_parse_tree.py`** (+ `gen_extra_files.py`) reads node definitions and
  renders the `*.template` files into:
  - `parse_tree_generated.{h,cc}` — the node classes (getters, fields, visitor
    dispatch).
  - `ast_node_kind.h` — the `ASTNodeKind` enum (one value per node).
  - `parse_tree.proto`, `parse_tree_serializer.{h,cc}` — serialization.
  - `parse_tree_generated.md` — the node-hierarchy reference doc.
- **Hand-written base/infra:** `ast_node.{h,cc}` (base class, visitor support),
  `ast_node_factory.*` (arena allocation), `ast_node_util.*` (safe casts,
  predicates), `parse_tree.{h,cc}` (public surface, e.g. `FakeASTNode`),
  `parse_tree_errors.*`, `parser_mode.h`, `statement_properties.h`,
  `visit_result.h`.

**To add/modify a parse-tree node you edit `gen_parse_tree.py`**, not the
generated files.

## Entry points

- `parser/parser.h` — `Parser`, `ParserOptions`, and parse functions for the
  different `ParserMode`s (`kStatement`, `kExpression`, `kType`,
  `kNextStatement`, `kScript`, …). The analyzer and scripting both call in here.
- `parser_internal.{h,cc}` — the actual parsing + error recovery.

## Macros (`macros/` subdirectory)

GoogleSQL supports SQL-level macros (`DEFINE MACRO foo …;` then `$foo(...)`)
expanded **at the token level before parsing**:
- `macro_expander.*` — the expander (handles nesting, recursion limits).
- `macro_catalog.*` — stores macro definitions.
- `token_provider.*`, `quoting.*` — token stream source & quoting context.
- `standalone_macro_expansion.*` — expand without parsing.
- `diagnostic.*` — macro warnings/errors.

## Other utilities

- **`unparser.{h,cc}`** — AST → SQL text. Reconstructs/pretty-prints SQL from a
  parse tree (the `--unparse` mode in `execute_query`). Distinct from the
  Resolved-AST `sql_builder`.
- **`deidentify.*`** — strip identifiers/literals from an AST (anonymization).
- **`join_processor.*`** — normalize JOIN expressions (ON/USING) into a
  consistent shape.

## How it connects

- **Downstream:** the [analyzer](analyzer.md) consumes `ASTStatement` /
  `ASTExpression` and produces the [Resolved AST](resolved-ast.md).
  [Scripting](scripting.md) calls `ParseScript` to get an `ASTScript`.
- **Tools:** `execute_query` exposes `--parse` (print tree) and `--unparse`.
- **Dependencies:** only `base/` and shared protos — the parser does **not**
  depend on the catalog or type system.

## Tests

`testdata/*.test` (~200 golden files spanning statements, expressions, macros,
GQL, MATCH_RECOGNIZE, errors) run by `run_parser_test.cc`; plus `parser_test.cc`,
`parse_tree_test.cc`, `unparser_test.cc`, `lookahead_transformer_test.cc`,
`keywords_test.cc`, `deidentify_test.cc`.
