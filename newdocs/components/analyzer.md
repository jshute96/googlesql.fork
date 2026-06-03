# Component: Analyzer (`googlesql/analyzer/`)

**Role:** The heart of GoogleSQL. The **Resolver** consumes a parse tree from the
[parser](parser.md) and produces a fully type-checked
[Resolved AST](resolved-ast.md). Then the **rewriters** simplify/expand that
Resolved AST. This is where SQL *semantics* live: name resolution, type
inference & checking, function/signature matching, implicit coercion, and the
lowering of surface syntax into explicit resolved nodes.

Engines never call the analyzer directly — they call
`public/analyzer.h::AnalyzeStatement()`, which delegates here through
`analyzer_impl.{h,cc}` (a thin bridge that exists to break dependency cycles).

## What the analyzer does (stages)

1. **Parse** (delegated to the parser) → `ASTStatement`/`ASTExpression`.
2. **Resolve** — `Resolver` walks the AST:
   - **Name resolution** against the `Catalog` (tables, functions, types,
     procedures, constants) using a stack of **`NameScope`s**.
   - **Type inference & checking** for every expression.
   - **Function resolution** — match a call to a `FunctionSignature`, inserting
     implicit casts via the `Coercer`.
   - **Query construction** — build resolved scans for FROM/JOIN/WHERE/GROUP BY/
     HAVING/ORDER BY/LIMIT, set operations, subqueries, CTEs, window functions,
     and pipe operators.
   - **Statement handling** — queries, DML, DDL/ALTER, etc.
3. **Rewrite** — apply enabled rewriters to fixpoint.
4. **Validate & finalize** — `Validator` checks the tree; `AnalyzerOutput` is
   returned.

## The Resolver — file-per-concern

`resolver.{h,cc}` defines the `Resolver` class (a very large header). Resolution
logic is split across files by concern:

| File | Responsibility |
|------|----------------|
| `resolver.cc` | Construction, top-level setup, type/hint resolution. |
| `resolver_stmt.cc` | Statement dispatch; `ResolveType`/`ResolveArrayType`/`ResolveStructType`/… (type syntax). |
| `resolver_query.cc` | `SELECT`/`FROM`/`JOIN`/`GROUP BY`/`HAVING`/`ORDER BY`/`LIMIT`, set ops, **pipe operators** (`\|>`). Largest file. |
| `resolver_expr.cc` | Scalar expression resolution. |
| `resolver_dml.cc` | `INSERT`/`UPDATE`/`DELETE`/`MERGE`. |
| `resolver_alter_stmt.cc` | DDL / `ALTER`. |

**Primary entry points** (on `Resolver`): `ResolveStatement()`,
`ResolveStandaloneExpr()`, `ResolveQueryStatement()`.

### Name resolution & scope
- `name_scope.{h,cc}` — the `NameScope` hierarchy: what names (columns, ranges,
  aliases) are visible where, and how they shadow. Central to correct SQL scoping.
- `query_resolver_helper.{h,cc}` — per-query scratch state (visible columns,
  aliases, aggregation/grouping state).
- `path_expression_span.{h,cc}` — tracks dotted paths like `a.b.c`.

### Function / operator / window resolution
- `function_resolver.{h,cc}` — match a call to a function & signature.
- `function_signature_matcher.{h,cc}` — the signature-matching algorithm
  (argument counts, templated types, coercion candidates).
- `analytic_function_resolver.{h,cc}` — window functions (`OVER`, `PARTITION BY`,
  frames).
- Expression helpers: `expr_resolver_helper.*`, `expr_matching_helpers.*`,
  `constant_resolver_helper.*`, `input_argument_type_resolver_helper.*`,
  `lambda_util.*`.

### Specialized resolution
- `set_operation_resolver_base.*` — `UNION`/`INTERSECT`/`EXCEPT` column matching
  & coercion.
- `recursive_queries.*` — `WITH RECURSIVE`.
- `graph_query_resolver.*`, `graph_stmt_resolver.*`,
  `graph_expr_resolver_helper.*` — GQL property-graph queries.

### Supporting logic
- `annotation_propagator.*` — propagate type annotations (e.g. collation) through
  expressions.
- `column_cycle_detector.*` — detect cycles in generated columns.
- `filter_fields_path_validator.*` — validate `FILTER_FIELDS` paths.
- `substitute.*` — substitute expressions/parameters (used by rewriters and
  builtin lowering).
- `resolver_util.*`, `builtin_only_catalog.*`, `analyzer_output_mutator.*`.

## Rewriters

After resolution, **rewriters** transform the Resolved AST into an equivalent but
simpler/expanded tree. This lets engines support many high-level features by
only implementing a smaller core — and lets the reference impl reuse one
lowering. Rewriters run **iteratively to fixpoint**.

### Framework
- `rewrite_resolved_ast.{h,cc}` — orchestrates: figure out which rewriters are
  relevant, apply them, repeat until stable.
- `all_rewriters.{h,cc}` — registers the builtin rewriters **in dependency
  order** (e.g. SQL-function inlining runs early so later rewriters see inlined
  bodies; cleanup rewriters run last).
- `rewriters/registration.h` — the thread-safe `RewriteRegistry` singleton; each
  rewriter is keyed by a `ResolvedASTRewrite` enum value, and
  `AnalyzerOptions::enabled_rewrites()` selects which run.
- `rewriters/rewriter_relevance_checker.*` — cheaply decide whether a tree
  contains anything a given rewriter cares about.

Each rewriter is a `RewriteVisitor` subclass (base class generated from
`gen_resolved_ast.py`) using `resolved_ast/rewrite_utils.*` to manipulate columns
and subtrees.

### Notable rewriters by theme
- **SQL inlining:** `sql_function_inliner` (scalar/aggregate/TVF/UDA),
  `sql_view_inliner`, `templated_function_call_rewriter`.
- **Reshaping / expansion:** `flatten_rewriter` (ARRAY/STRUCT),
  `pivot_rewriter`, `unpivot_rewriter`, `multiway_unnest_rewriter`,
  `grouping_set_rewriter`, `insert_dml_values_rewriter`.
- **Operators / functions:** `like_any_all_rewriter`,
  `is_first_is_last_function_rewriter`, `builtin_function_inliner`,
  `match_recognize_function_rewriter`, `nulliferror_function_rewriter`,
  `typeof_function_rewriter`, `map_function_rewriter`,
  `variadic_function_signature_expander`.
- **Privacy / security:** `anonymization_rewriter` (+ `anonymization_helper`,
  `privacy/`), `aggregation_threshold_rewriter` (differential privacy &
  k-anonymity).
- **Pipe syntax:** `pipe_assert_rewriter`, `pipe_if_rewriter`,
  `pipe_describe_rewriter`, `generalized_query_stmt_rewriter`,
  `subpipeline_stmt_rewriter` (+ `rewrite_subpipeline`).
- **Measures / misc:** `measure_type_rewriter` (+ helpers),
  `order_by_and_limit_in_aggregate_rewriter`, `update_constructor_rewriter`,
  `with_expr_rewriter`.

## How it connects

- **Input:** [parser](parser.md) AST; the `Catalog`, `TypeFactory`,
  `AnalyzerOptions`/`LanguageOptions` from [public/](public-api.md).
- **Builtin functions:** signatures come from `public/builtin_function*` and the
  `common/builtin_function_*` definitions.
- **Output:** a [Resolved AST](resolved-ast.md) wrapped in `AnalyzerOutput`.
- **Consumers:** [reference_impl](reference-impl.md) and external engines.

## Tests

`analyzer_test.cc` + `testdata/*.test` (~200 golden resolved-AST files via
`run_analyzer_test.*`), `resolver_test.cc`, `rewrite_resolved_ast_test.cc`, plus
focused helper tests.
