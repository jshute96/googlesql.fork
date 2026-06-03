# Component: Public API (`googlesql/public/`)

**Role:** The public surface of GoogleSQL — what an embedding engine actually
`#include`s. It exposes the analyzer entry points, the catalog interface, the
function model, the evaluator, and (in `types/`) the type system and `Value`.
Most files here are stable-ish APIs; internal machinery lives in
[analyzer/](analyzer.md), [common/](common-and-base.md), and
[reference_impl/](reference-impl.md).

> The type system and `Value` are large enough to have their own doc:
> [type-system.md](type-system.md).

## The five things an engine uses

### 1. Analyzer entry points
- **`analyzer.{h,cc}`** — `AnalyzeStatement(sql, options, catalog, type_factory,
  &output)` and `AnalyzeExpression(...)`: the main way to compile SQL to a
  [Resolved AST](resolved-ast.md). Also `RewriteResolvedAst(...)`.
- **`analyzer_options.{h,cc}`** — `AnalyzerOptions`: query parameters, system
  variables, enabled rewriters, error-message mode, arenas, and the
  `LanguageOptions`.
- **`language_options.{h,cc}`** — `LanguageOptions`: which dialect *features* and
  reserved keywords are on. This is how engines expose a language subset.
- **`analyzer_output.{h,cc}`** — `AnalyzerOutput`: the result (resolved
  statement/expression + metadata).

### 2. Catalog (name resolution)
The analyzer resolves names through an abstract **`Catalog`**; engines implement
it over their own metadata.
- **`catalog.{h,cc}`** — the `Catalog` interface plus the object interfaces it
  returns: `Table`, `Column`, `Function`, `TableValuedFunction`, `Procedure`,
  `Type`, `Constant`, `Model`, `Connection`.
- **`simple_catalog.{h,cc}`** — `SimpleCatalog`, a ready-to-use in-memory
  implementation (used by tests, tools, and many engines for bootstrap).
- `multi_catalog.*` (compose catalogs), `catalog_helper.*`,
  `simple_catalog_util.*`, `table_name_resolver.*`, `table_from_proto.*`.

### 3. Functions & signatures
- **`function.{h,cc}`** — `Function`, the base for named functions, carrying one
  or more `FunctionSignature`s.
- **`function_signature.*`** — argument types/cardinality, templated types,
  return-type computation.
- `input_argument_type.*`, `signature_match_result.*` — inputs and results of
  signature matching (used by the analyzer's matcher).
- **`builtin_function.{h,cc}` + `builtin_function_options.*`** — populate a
  catalog with the standard library. (The actual signature *definitions* live in
  [`common/builtin_function_*`](common-and-base.md); implementations for engines
  live in `public/functions/` and `reference_impl/`.)
- User-defined function kinds: `sql_function.*`, `templated_sql_function.*`,
  `non_sql_function.*`, `anon_function.*`, `procedure.*`,
  `table_valued_function.*`, `sql_tvf.*`, `templated_sql_tvf.*`.
- **Coercion/cast:** `coercer.{h,cc}` (implicit/explicit coercion rules),
  `cast.{h,cc}` (cast catalog & cost), `convert_type_to_proto.*`.

### 4. Evaluator
A convenience wrapper over the [reference implementation](reference-impl.md) so
callers can run SQL without wiring up the algebrizer themselves.
- **`evaluator.{h,cc}`** — `PreparedExpression` / `PreparedQuery`: analyze once,
  execute many times with parameters/columns. Full builtin function set.
- `evaluator_base.*` — shared base logic.
- `evaluator_lite.h` — a lighter-weight variant (smaller binary, fewer functions).
- `prepared_expression_constant_evaluator.*` / `constant_evaluator.h` — constant
  folding during analysis.

### 5. The type system & values → see [type-system.md](type-system.md)
`types/` (the `Type` hierarchy + `TypeFactory`) and `value.{h,cc}` (the immutable
`Value`), plus scalar value types (`numeric_value`, `json_value`, `uuid_value`,
`interval_value`, `timestamp_picos_value`, `civil_time`, `pico_time`).

## `functions/` subdirectory — shared builtin implementations

Reusable C++ implementations of builtin functions that engines (and the
reference impl) can call directly: arithmetic/math, string & string-format,
date/time (parse/format/cast), type conversion, JSON, regexp/like, net, hash,
bitwise, percentile, range, distance, compression, uuid, numeric. These are the
*runtime* logic; their *signatures* are defined in `common/builtin_function_*`.

## `annotation/` subdirectory

Type **annotations** — metadata layered on types, most importantly **collation**.
`annotation/collation.*` and `default_annotation_spec.*` define how annotations
propagate; the analyzer's `annotation_propagator` carries them through
expressions.

## Other public utilities

- **Modules** (`modules.*`, `module_factory.*`, `module_contents_fetcher.*`,
  `file_module_contents_fetcher.*`, `module_details.*`) — reusable SQL packages
  (functions/types/procedures) with namespacing and dependency loading.
- **Parsing/formatting helpers** (`parse_helpers.*`, `parse_location.*`,
  `parse_tokens.*`, `sql_formatter.*`, `lenient_formatter.*`,
  `formatter_options.*`) — statement-type detection, location tracking, and the
  SQL formatter API (internals in [tools/formatter](tooling-and-bindings.md)).
- **Collation** (`collator.*` full ICU, `collator_lite.*` ICU-free), `strings.*`.
- **Errors/diagnostics** (`error_helpers.*`, `literal_remover.*`,
  `feature_label_extractor.*`, `cycle_detector.*`).
- **Property graph** (`property_graph.*`, `simple_property_graph.*`).
- **Privacy** (`anonymization_utils.*`, `aggregation_threshold_utils.*`),
  **measures** (`measure_expression.*`).
- `id_string.*` (interned identifiers — used pervasively for fast name compares),
  `proto_util.*`, `proto_value_conversion.*`, `constant.*`, `sql_constant.*`.

## Connections

`public/` is the hub: it defines the data types (`Type`, `Value`, `Catalog`,
`Function`) that flow between the [parser](parser.md), [analyzer](analyzer.md),
[resolved_ast](resolved-ast.md), and [reference_impl](reference-impl.md). The
analyzer takes `Catalog` + `AnalyzerOptions` and returns a Resolved AST; the
evaluator takes that Resolved AST and returns `Value`s.
