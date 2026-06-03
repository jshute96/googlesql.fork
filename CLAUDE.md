# CLAUDE.md

Orientation for working in this repository. Detailed architecture/code docs live
in [`newdocs/`](newdocs/doc-index.md).

## What this project is

**GoogleSQL** (formerly **ZetaSQL** — both names appear throughout the code) is a
reusable C++ library that defines a SQL language and implements its compiler
**front-end**: lexing, parsing, and semantic analysis. It is **not a database or
query engine**. Its output is an engine-neutral, fully type-checked intermediate
representation, the **Resolved AST**, which real engines (BigQuery, Spanner, F1,
Dremel, Procella, …) translate into their own execution plans. It ships a
**reference implementation** (an interpreter) for executing queries in tests/
debugging, and a **compliance suite** engines use to prove conformance.

There is no API-stability guarantee, and the project does not accept external
contributions (commits are exports of internal changes).

## The pipeline (one line)

`SQL text → parser (AST) → analyzer/Resolver (Resolved AST) → rewriters → {reference_impl executes | engine consumes | SQLBuilder re-emits SQL}`

See [newdocs/architecture.md](newdocs/architecture.md) for the full picture.

## Where things are

| You want… | Go to | Doc |
|-----------|-------|-----|
| Big-picture architecture | — | [newdocs/architecture.md](newdocs/architecture.md) |
| "Which file/dir is X?" | — | [newdocs/file-index.md](newdocs/file-index.md) |
| Doc map + "where do I change X?" table | — | [newdocs/doc-index.md](newdocs/doc-index.md) |
| Grammar / parse tree | `googlesql/parser/` | [parser.md](newdocs/components/parser.md) |
| The Resolved AST IR | `googlesql/resolved_ast/` | [resolved-ast.md](newdocs/components/resolved-ast.md) |
| Name resolution / type checking / rewriters | `googlesql/analyzer/` | [analyzer.md](newdocs/components/analyzer.md) |
| Public API (analyzer entry, catalog, functions) | `googlesql/public/` | [public-api.md](newdocs/components/public-api.md) |
| Type system & `Value` | `googlesql/public/types/`, `public/value.*` | [type-system.md](newdocs/components/type-system.md) |
| Query execution | `googlesql/reference_impl/` | [reference-impl.md](newdocs/components/reference-impl.md) |
| Builtin function defs / shared helpers / base lib | `googlesql/common/`, `googlesql/base/` | [common-and-base.md](newdocs/components/common-and-base.md) |
| Procedural scripts | `googlesql/scripting/` | [scripting.md](newdocs/components/scripting.md) |
| Conformance tests & sample catalog | `googlesql/compliance/`, `testing/`, `testdata/` | [compliance-and-testing.md](newdocs/components/compliance-and-testing.md) |
| `execute_query`, formatter, Java bindings | `googlesql/tools/`, `local_service/`, `java/` | [tooling-and-bindings.md](newdocs/components/tooling-and-bindings.md) |
| **Language** reference (user-facing) | `docs/` | (e.g. `query-syntax.md`, `data-types.md`, `pipe-syntax.md`, `resolved_ast.md`) |

## Key facts to keep in mind

- **Two big chunks are code-generated**, not hand-edited:
  - Parse-tree node classes ← `googlesql/parser/gen_parse_tree.py` (+ `*.template`).
  - Resolved AST node classes (C++ **and** Java) ← `googlesql/resolved_ast/gen_resolved_ast.py`.
  To add/modify a node, edit the generator, not the generated `*_generated.*` files.
- The **lexer** is generated from the TextMapper grammar `googlesql/parser/googlesql.tm`.
- Behavior is driven by **`LanguageOptions`** (which features are on) and
  **`AnalyzerOptions`** (analysis config + enabled rewriters), both in `public/`.
- A new builtin function usually touches three places: signature in
  `common/builtin_function_*`, optional shared impl in `public/functions/`, and
  evaluation in `reference_impl/function.cc`.
- The **`Validator`** (`resolved_ast/validator.*`) checks any Resolved AST tree —
  useful when writing rewriters or constructing trees.
- **`base/`** holds the status/error macros (`RETURN_IF_ERROR`, `ASSIGN_OR_RETURN`,
  `RET_CHECK`) used everywhere; errors carry source-location payloads via
  `common/errors.*`.

## Build & run (Bazel)

```bash
bazel build ...                                              # build everything
bazel test //googlesql/parser:parser_set_test               # run a test
bazel run //googlesql/tools/execute_query:execute_query -- --web   # interactive tool
```

`tzdata` is required. A `Dockerfile` provides a pinned toolchain. The Bazel
version is pinned in `.bazelversion`. See `README.md` for full instructions and
`execute_query.md` for the tool.

## Background reading

The README links external papers worth skimming: *GoogleSQL: A SQL Language as a
Component* (CDMS 2022), *Spanner: Becoming a SQL System* (SIGMOD 2017, §6), and
*SQL Has Problems. We Can Fix Them: Pipe Syntax in SQL* (VLDB 2024).
