# GoogleSQL Architecture

> GoogleSQL was formerly named **ZetaSQL**. Both names appear throughout the
> code (filenames, namespaces, comments). They refer to the same project.

## What GoogleSQL is

GoogleSQL is **not a database or a query engine**. It is a reusable C++ library
that *defines* a SQL language (grammar, type system, data model, semantics, and
a function library) and *implements* the front-end of a SQL compiler for it:
lexing, parsing, and semantic analysis. The output is a fully type-checked,
engine-neutral intermediate representation called the **Resolved AST**.

Real query engines (BigQuery, Spanner, F1, Dremel, Procella, …) embed
GoogleSQL to get consistent name resolution, type checking, implicit casting,
and error behavior, then translate the Resolved AST into their own execution
plans. GoogleSQL ships its own **reference implementation** (an interpreter) so
queries can actually be executed for testing and debugging, plus a
**compliance test suite** that engines use to validate they behave identically.

## The compilation pipeline

The core data flow is a classic compiler front-end pipeline, with two
engine-neutral IRs (the parse tree / AST, then the Resolved AST):

```
   SQL text
      │
      ▼
 ┌──────────┐   tokens    ┌──────────┐   parse tree   ┌────────────┐
 │  Lexer   │────────────▶│  Parser  │───────────────▶│  Analyzer  │
 │(TextMap- │             │ (builds  │   (ASTNode     │ (Resolver) │
 │ per .tm) │             │  AST)    │    tree)       │            │
 └──────────┘             └──────────┘                └─────┬──────┘
      ▲                        ▲                            │ Resolved AST
      │                        │                            ▼
  macros/ expansion       parser/ component          ┌────────────┐
                                                      │ Rewriters  │ (optional,
                                                      │ (resolved- │  fixpoint)
                                                      │  AST → AST)│
                                                      └─────┬──────┘
                                                            │ final Resolved AST
                          ┌─────────────────────────────────┼─────────────────┐
                          ▼                                  ▼                 ▼
                  ┌───────────────┐                 ┌────────────────┐   engine's own
                  │ reference_impl│                 │   SQLBuilder   │   planner/codegen
                  │  (algebrize + │                 │ (Resolved AST  │   (external)
                  │   evaluate)   │                 │   → SQL text)  │
                  └───────────────┘                 └────────────────┘
                          │ Values / result rows
                          ▼
                       results
```

### Stage 1 — Lexing & Parsing (`googlesql/parser/`)
SQL text is tokenized by a lexer generated from a **TextMapper grammar**
(`googlesql.tm`) and parsed into a **parse tree** of `ASTNode` subclasses. SQL
**macros** (`DEFINE MACRO`) are expanded at the token level before parsing. The
parse tree is purely syntactic — no types, no name resolution. The node classes
are **code-generated** from `gen_parse_tree.py`. The parse tree is "semi-public"
— it is exposed but not a stable API.
→ See [components/parser.md](components/parser.md).

### Stage 2 — Semantic Analysis (`googlesql/analyzer/`)
The **Resolver** walks the parse tree and produces the **Resolved AST**. This is
where the real semantics live: names are resolved against a **Catalog**, types
are inferred and checked, functions are matched to **signatures**, implicit
**casts/coercions** are inserted, and constructs like `SELECT`, joins, grouping,
window functions, subqueries, DML and DDL are turned into explicit resolved
nodes. The analyzer is internal; engines call it through the public
`AnalyzeStatement()` API.
→ See [components/analyzer.md](components/analyzer.md).

### Stage 3 — Rewrites (`googlesql/analyzer/rewriters/`)
After resolution, a set of **rewriters** transform the Resolved AST into a
simpler/expanded but semantically-equivalent Resolved AST. Rewriters inline SQL
UDFs/views, expand `PIVOT`/`UNNEST`/`FLATTEN`/`GROUPING SETS`, lower pipe
operators, apply differential-privacy transforms, and more. They run iteratively
to fixpoint. Rewriting lets engines support high-level features "for free" by
reducing them to a smaller core.
→ See [components/analyzer.md](components/analyzer.md#rewriters).

### Stage 4a — Execution (`googlesql/reference_impl/`)
The reference implementation **algebrizes** the Resolved AST into a tree of
relational/scalar operators, then **evaluates** them with an iterator
("Volcano") model to produce result rows. It prioritizes correctness and
completeness over performance and is the oracle for the compliance suite.
→ See [components/reference-impl.md](components/reference-impl.md).

### Stage 4b — SQL generation (`googlesql/resolved_ast/sql_builder.*`)
The **SQLBuilder** runs the pipeline in reverse: Resolved AST → SQL text. Used
for round-tripping, debugging, and engines that re-emit SQL.

## The Resolved AST — the central IR
→ See [components/resolved-ast.md](components/resolved-ast.md).

The Resolved AST is the contract between GoogleSQL and its embedding engines.
Key properties:
- ~150 node classes (`ResolvedStatement`, `ResolvedScan`, `ResolvedExpr`, …),
  all **code-generated** from `gen_resolved_ast.py` (C++ and Java, plus protobuf
  serialization).
- Every node is fully typed; every column is a `ResolvedColumn` with a unique id.
- A `Validator` enforces structural and semantic invariants on any tree.
- Generated visitors (`DeepCopyVisitor`, `RewriteVisitor`) support traversal and
  transformation; this is what the rewriters and engines build on.
- Documented for users in `docs/resolved_ast.md`.

## Cross-cutting foundations

These are not pipeline stages but are used by every stage:

- **Type system & Values** (`public/types/`, `public/value.*`) — the `Type`
  class hierarchy (INT64, STRING, ARRAY, STRUCT, PROTO, ENUM, RANGE, MAP, …),
  the `TypeFactory` that interns them, and the immutable `Value` class.
  → See [components/type-system.md](components/type-system.md).
- **Catalog** (`public/catalog.h`, `simple_catalog.*`) — the abstract interface
  through which the analyzer looks up tables, functions, types, procedures, and
  constants. Engines implement `Catalog`; `SimpleCatalog` is the in-memory
  reference implementation.
- **Functions** (`public/function.*`, `public/builtin_function*`,
  `common/builtin_function_*`, `public/functions/`) — function/signature model,
  the builtin function library, and signature matching/coercion (`coercer`,
  `cast`).
- **Public API surface** (`public/`) — everything an engine includes:
  `analyzer.h`, `analyzer_options.h`, `language_options.h`, `evaluator.h`,
  catalog, types, functions, error helpers.
  → See [components/public-api.md](components/public-api.md).
- **Base & common utilities** (`base/`, `common/`) — `base/` is a vendored
  Abseil-like foundation (status, arena, logging, math, containers); `common/`
  holds shared analyzer/runtime helpers, error construction, and the bulk of the
  builtin function *definitions*.
  → See [components/common-and-base.md](components/common-and-base.md).

## Configuration knobs

Behavior is controlled almost entirely by two option objects passed into the
analyzer:

- **`LanguageOptions`** — which language *dialect* features and reserved
  keywords are enabled (e.g. pipe syntax, specific function groups). Lets engines
  expose a subset of the language.
- **`AnalyzerOptions`** — analysis configuration: the `LanguageOptions`, query
  parameters, system variables, which rewriters to run, error message format,
  arenas, etc.

This is how one library serves many engines with different feature sets.

## Surrounding components

- **Scripting** (`scripting/`) — procedural multi-statement scripts (`DECLARE`,
  `IF`, `WHILE`, `BEGIN…EXCEPTION`, `CALL`), built on top of the analyzer + a
  control-flow graph + a statement executor.
  → See [components/scripting.md](components/scripting.md).
- **Compliance** (`compliance/`) — the engine-agnostic conformance test
  framework (`TestDriver` interface, `.test` golden files, known-error lists).
  → See [components/compliance-and-testing.md](components/compliance-and-testing.md).
- **Tools** (`tools/execute_query/`, `tools/formatter/`) — the `execute_query`
  CLI/web playground and the SQL formatter.
- **local_service + Java** (`local_service/`, `java/`) — a gRPC/JNI service that
  wraps the C++ library so the Java client can use it out-of-process.
  → See [components/tooling-and-bindings.md](components/tooling-and-bindings.md).

## How a query flows through the code (concrete entry points)

1. Engine calls `AnalyzeStatement(sql, options, catalog, type_factory, &output)`
   (`public/analyzer.cc`).
2. That parses via `parser/parser.cc` → `ASTStatement`.
3. `analyzer/resolver*.cc` (`Resolver`) resolves it → `ResolvedStatement`.
4. `analyzer/rewrite_resolved_ast.cc` applies enabled rewriters.
5. Result is an `AnalyzerOutput` holding the final `ResolvedStatement`.
6. To execute: `reference_impl/algebrizer.cc` turns it into operators,
   `reference_impl/evaluation.cc` runs them — or the engine consumes the
   Resolved AST itself.

## Build system

The project builds with **Bazel** (`MODULE.bazel`, `.bazelversion`, per-directory
`BUILD` files). Code generation (parse tree, resolved AST) runs as Bazel genrules
invoking the `gen_*.py` scripts against `.template` files. A `Dockerfile` pins the
toolchain. There is **no API stability guarantee** and the project does not accept
external contributions.

## Where to go next

- Per-directory / per-file map: [file-index.md](file-index.md)
- Full doc tree & component hierarchy: [doc-index.md](doc-index.md)
