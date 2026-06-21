# Documentation Index

This is the map of the `newdocs/` documentation set and the component hierarchy
it describes. Start at [architecture.md](architecture.md) for the big picture,
use [file-index.md](file-index.md) to find where code lives, and drill into a
component doc when working on that area.

## Documents

| Doc | What it covers |
|-----|----------------|
| [architecture.md](architecture.md) | Overall architecture, the compile→execute pipeline, how components interact, configuration knobs. **Start here.** |
| [file-index.md](file-index.md) | Directory-by-directory, file-by-file map of the whole repo. **Use to locate code.** |
| [doc-index.md](doc-index.md) | This file — doc map + component hierarchy. |
| [components/parser.md](components/parser.md) | Lexer + parser → parse tree (AST); macros; codegen. |
| [components/resolved-ast.md](components/resolved-ast.md) | The Resolved AST IR, its codegen, validator, SQLBuilder. |
| [pipe-sql-builder.md](pipe-sql-builder.md) | SQLBuilder's pipe-syntax output: where it can't form pipe SQL and where scans aren't 1:1 with pipe operators. |
| [components/analyzer.md](components/analyzer.md) | The Resolver (AST → Resolved AST) and the rewriter framework. |
| [components/public-api.md](components/public-api.md) | Public API surface: analyzer entry, catalog, functions, evaluator. |
| [components/type-system.md](components/type-system.md) | Type hierarchy, `TypeFactory`, `Value` (subcomponent of public). |
| [components/reference-impl.md](components/reference-impl.md) | Reference execution engine (algebrize + evaluate). |
| [components/common-and-base.md](components/common-and-base.md) | Shared helpers + builtin function definitions (`common/`); foundation lib (`base/`). |
| [components/scripting.md](components/scripting.md) | Procedural SQL scripts (control flow, variables). |
| [components/compliance-and-testing.md](components/compliance-and-testing.md) | Conformance framework + shared test catalog/data. |
| [components/tooling-and-bindings.md](components/tooling-and-bindings.md) | `execute_query`, formatter, `local_service`, Java bindings, examples. |
| [execute-query-visualizer.md](execute-query-visualizer.md) | The `execute_query` query visualizer: input SQL / Resolved AST / SQLBuilder SQL side by side, with correspondence highlighting. |
| `../CLAUDE.md` | Top-level orientation & quick pointers (repo root). |

## Component hierarchy

The pipeline stages (▶ = data flows to), with the cross-cutting foundations
underneath:

```
SQL text
  │
  ▼  ── parser/ ───────────────────────── [parser.md]
  │     lexer (googlesql.tm) · macros/ · ASTNode tree (codegen)
  │
  ▼  ── analyzer/ ────────────────────── [analyzer.md]
  │     Resolver (resolver_*.cc) · name_scope · function_resolver
  │       └─ rewriters/ (rewrite framework) ──── [analyzer.md#rewriters]
  │
  ▼  ── resolved_ast/ ─────────────────── [resolved-ast.md]
  │     ResolvedNode tree (codegen) · validator · sql_builder · rewrite_utils
  │
  ├──▶ reference_impl/ ────────────────── [reference-impl.md]
  │      algebrizer · relational/value ops · evaluation · functions/
  │
  └──▶ resolved_ast/sql_builder ───────── (Resolved AST → SQL)

Cross-cutting foundations (used by all stages):
  public/ ───────────────────────────── [public-api.md]
    ├─ analyzer.h / analyzer_options / language_options   (entry & config)
    ├─ catalog / simple_catalog                           (name resolution)
    ├─ function / builtin_function / coercer / cast        (function model)
    ├─ evaluator                                           (run via reference_impl)
    ├─ functions/                                          (builtin impls)
    └─ types/ + value ──────────────────── [type-system.md]
         Type hierarchy · TypeFactory · Value
  common/ ──┐                              [common-and-base.md]
            ├─ builtin function *definitions* · errors · match_recognize NFA
  base/  ───┘  vendored Abseil-like utilities (status, arena, math, …)

Surrounding components:
  scripting/ ─────────── procedural scripts          [scripting.md]
  compliance/ + testing/ + testdata/ ── conformance  [compliance-and-testing.md]
  tools/ + local_service/ + java/ + examples/ ── UX  [tooling-and-bindings.md]
```

## "I want to change X — where do I look?"

| Goal | Primary doc / location |
|------|------------------------|
| New syntax / grammar | [parser.md](components/parser.md) → `parser/gen_parse_tree.py`, `googlesql.tm` |
| New resolved node | [resolved-ast.md](components/resolved-ast.md) → `resolved_ast/gen_resolved_ast.py` |
| Name resolution / type-check behavior | [analyzer.md](components/analyzer.md) → `analyzer/resolver_*.cc`, `name_scope` |
| New builtin function | [common-and-base.md](components/common-and-base.md) (signature) + [public-api.md](components/public-api.md) (`public/functions/`) + [reference-impl.md](components/reference-impl.md) (`reference_impl/function.cc`) |
| Lower a feature to simpler nodes | [analyzer.md](components/analyzer.md#rewriters) → `analyzer/rewriters/` |
| New / changed type | [type-system.md](components/type-system.md) → `public/types/` (+ coercer, evaluator) |
| Execution behavior | [reference-impl.md](components/reference-impl.md) → `reference_impl/` |
| Catalog / schema integration | [public-api.md](components/public-api.md) → `public/catalog.h`, `simple_catalog.*` |
| Conformance tests | [compliance-and-testing.md](components/compliance-and-testing.md) → `compliance/testdata/*.test` |
| The interactive tool | [tooling-and-bindings.md](components/tooling-and-bindings.md) → `tools/execute_query/` |
