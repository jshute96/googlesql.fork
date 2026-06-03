# File Index

A directory-by-directory, file-by-file map of the GoogleSQL codebase. Descriptions
are intentionally brief — see the per-component docs under
[components/](.) for depth. Within each directory, test files (`*_test.cc`,
`testdata/`) are summarized as a group rather than listed individually.

> Generated `*_generated.*`, `*.pb.h`, and template-expanded files are noted as
> codegen output, not listed individually.

## Repository root

| Path | What it is |
|------|------------|
| `README.md` | Project overview, build/run instructions, links to papers. |
| `CLAUDE.md` | Top-level orientation for this codebase (start here). |
| `execute_query.md` | Guide to the `execute_query` tool. |
| `zetasql_to_googlesql_migration.md` | Migration guide for the ZetaSQL→GoogleSQL rename. |
| `CONTRIBUTING.md`, `PULL_REQUEST_TEMPLATE.md` | Contribution notes (no external contributions accepted). |
| `MODULE.bazel`, `BUILD`, `.bazelrc`, `.bazelversion`, `bazel/` | Bazel build configuration & toolchain. |
| `Dockerfile`, `docker_build.sh` | Containerized build/run environment. |
| `go.mod`, `go.sum`, `requirements.txt` | Go and Python tooling deps. |
| `docs/` | **User-facing** language reference (see below). |
| `newdocs/` | **This** architecture/code documentation set. |
| `examples/` | Top-level usage examples. |
| `tools/` | Repo-level helper scripts (distinct from `googlesql/tools/`). |
| `java/`, `javatests/` | Java bindings and their tests. |
| `googlesql/` | All the C++ source — the actual library (see below). |

## `docs/` — user language reference
~70 markdown files documenting the *language*, not the code: `data-types.md`,
`query-syntax.md`, `pipe-syntax.md`, `data-model.md`, `functions-reference.md`
and per-category function docs (`string_functions.md`, `date_functions.md`, …),
`resolved_ast.md` (generated node-hierarchy reference), graph/GQL docs, DDL/DML,
differential privacy, lexical structure. Useful for understanding *what the
language supports*; nearly silent on *how the code works*.

---

## `googlesql/` — the library

Top-level layout:

| Subdir | Component | Doc |
|--------|-----------|-----|
| `parser/` | Lexer + parser → parse tree (AST) | [parser.md](components/parser.md) |
| `resolved_ast/` | The Resolved AST IR + codegen, validator, SQLBuilder | [resolved-ast.md](components/resolved-ast.md) |
| `analyzer/` | Resolver (AST → Resolved AST) + rewriters | [analyzer.md](components/analyzer.md) |
| `public/` | Public API: types, values, catalog, functions, analyzer/evaluator entry points | [public-api.md](components/public-api.md), [type-system.md](components/type-system.md) |
| `reference_impl/` | Reference execution engine (algebrize + evaluate) | [reference-impl.md](components/reference-impl.md) |
| `common/` | Shared analyzer/runtime helpers + builtin function definitions | [common-and-base.md](components/common-and-base.md) |
| `base/` | Vendored Abseil-like base utilities | [common-and-base.md](components/common-and-base.md) |
| `scripting/` | Procedural SQL scripts (control flow, variables) | [scripting.md](components/scripting.md) |
| `compliance/` | Conformance test framework + golden tests | [compliance-and-testing.md](components/compliance-and-testing.md) |
| `testing/`, `testdata/` | Shared test utilities, sample catalog & protos | [compliance-and-testing.md](components/compliance-and-testing.md) |
| `local_service/` | gRPC/JNI service wrapping the C++ library | [tooling-and-bindings.md](components/tooling-and-bindings.md) |
| `tools/` | `execute_query` CLI/web + SQL formatter | [tooling-and-bindings.md](components/tooling-and-bindings.md) |
| `proto/` | Shared `.proto` definitions (options, function, catalog, …) | below |
| `examples/` | TPC-H + pipe-syntax example queries & catalog | [tooling-and-bindings.md](components/tooling-and-bindings.md) |

---

### `googlesql/parser/`
Lexing, macro expansion, parsing → `ASTNode` tree. Node classes are codegen.

- `parser.{h,cc}`, `parser_internal.{h,cc}` — entry point (`Parser`,
  `ParserOptions`) and core parsing/error-recovery logic.
- `googlesql.tm` — **TextMapper grammar** (lexer rules, tokens, keywords). The
  source of truth for lexical structure.
- `tokenizer.*`, `textmapper_lexer_adapter.*`, `token_with_location.*`,
  `keywords.*` — tokenization layer and keyword tables.
- `lookahead_transformer.*` — disambiguates context-sensitive keywords.
- `ast_node.{h,cc}`, `ast_node_factory.*`, `ast_node_util.*` — AST base class,
  arena allocation, traversal helpers.
- `parse_tree.{h,cc}`, `parse_tree_errors.*`, `parser_mode.h`,
  `statement_properties.h`, `visit_result.h` — parse-tree surface API.
- `gen_parse_tree.py`, `gen_extra_files.py`, `*.template` — **codegen** that
  produces `parse_tree_generated.*`, `ast_node_kind.h`, visitors, serializers,
  and `parse_tree.proto`.
- `ast_enums.proto` — enums used by AST nodes.
- `unparser.{h,cc}` — AST → SQL text (reconstruction / pretty-print).
- `deidentify.*` — strip identifiers/literals from an AST.
- `join_processor.*` — normalize JOIN trees.
- `macros/` — SQL macro system: `macro_expander.*` (token-level expansion),
  `macro_catalog.*`, `token_provider.*`, `quoting.*`,
  `standalone_macro_expansion.*`, `diagnostic.*`.
- `testdata/` (~200 `.test` files) + `*_test.cc` — golden parse-tree tests.

### `googlesql/resolved_ast/`
The Resolved AST IR — mostly codegen plus hand-written infrastructure.

- `resolved_node.{h,cc}` — hand-written base class (introspection, visitor).
- `gen_resolved_ast.py` + `*.template` — **codegen** producing `resolved_ast.*`
  (~150 node classes), builders, visitors (`deep_copy`, `rewrite`), comparator,
  node-kind enums, `resolved_ast.proto`, and the `resolved_ast.md` reference.
- `resolved_column.{h,cc}`, `column_factory.{h,cc}` — `ResolvedColumn` (typed
  column refs with unique ids) and its allocator.
- `resolved_collation.{h,cc}` — collation spec attached to resolved nodes.
- `validator.{h,cc}` — structural/semantic invariant checker for any tree.
- `sql_builder.{h,cc}` — Resolved AST → SQL text (very large; reverse of analyzer).
- `query_expression.{h,cc}` — scratch IR used while building SQL/queries.
- `rewrite_utils.{h,cc}` — column remapping, correlation, copying utilities used
  by rewriters.
- `resolved_ast_helper.*`, `make_node_vector*.h`, `node_sources.h`,
  `target_syntax.h`, `serialization.proto`, `test_utils.*` — helpers.
- `*_test.cc` — node, validator, builder, visitor, rewrite-utils tests.

### `googlesql/analyzer/`
The Resolver: parse tree → Resolved AST. File-per-concern.

- `resolver.{h,cc}` — the `Resolver` class (huge header; entry points
  `ResolveStatement`, `ResolveStandaloneExpr`).
- `resolver_stmt.cc` — statement dispatch + type-syntax resolution.
- `resolver_query.cc` — `SELECT`/`FROM`/`JOIN`/`GROUP BY`/`ORDER BY`, set ops,
  pipe operators (largest resolver file).
- `resolver_expr.cc` — expression resolution.
- `resolver_dml.cc`, `resolver_alter_stmt.cc` — DML and DDL/ALTER.
- `analyzer_impl.{h,cc}` — internal bridge from `public/analyzer.cc` (breaks dep
  cycles); orchestrates resolve + rewrite.
- `name_scope.{h,cc}`, `query_resolver_helper.{h,cc}`,
  `path_expression_span.{h,cc}` — name resolution & scope tracking.
- `function_resolver.{h,cc}`, `function_signature_matcher.{h,cc}`,
  `analytic_function_resolver.{h,cc}` — function/operator/window resolution.
- `expr_resolver_helper.*`, `expr_matching_helpers.*`,
  `constant_resolver_helper.*`, `input_argument_type_resolver_helper.*`,
  `lambda_util.*` — expression-resolution helpers.
- `set_operation_resolver_base.*`, `recursive_queries.*` — UNION/etc. and
  `WITH RECURSIVE`.
- `graph_query_resolver.*`, `graph_stmt_resolver.*`,
  `graph_expr_resolver_helper.*` — GQL property-graph queries.
- `annotation_propagator.*`, `column_cycle_detector.*`,
  `filter_fields_path_validator.*`, `substitute.*`, `resolver_util.*`,
  `builtin_only_catalog.*`, `analyzer_output_mutator.*` — supporting logic.
- `rewrite_resolved_ast.{h,cc}`, `all_rewriters.{h,cc}` — rewrite orchestration
  and registration.
- `rewriters/` — the rewriter library (~30 rewriters: SQL/view inlining, flatten,
  pivot/unpivot, grouping sets, anonymization & aggregation-threshold privacy,
  pipe operators, measures, map functions, etc.) plus `registration.h` and
  `rewriter_relevance_checker.*`. See [analyzer.md](components/analyzer.md#rewriters).
- `testdata/` (~200 `.test` files) + `analyzer_test.cc`, `resolver_test.cc`,
  `run_analyzer_test.*` — golden resolved-AST tests.

### `googlesql/public/`
The public API surface (most things an engine `#include`s). Grouped by theme:

- **Analyzer entry**: `analyzer.{h,cc}`, `analyzer_options.{h,cc}`,
  `analyzer_output.{h,cc}`, `language_options.{h,cc}`.
- **Catalog**: `catalog.{h,cc}`, `simple_catalog.{h,cc}`, `multi_catalog.*`,
  `catalog_helper.*`, `simple_catalog_util.*`, `table_name_resolver.*`,
  `table_from_proto.*`.
- **Functions**: `function.{h,cc}`, `function_signature.*`,
  `input_argument_type.*`, `signature_match_result.*`, `builtin_function.{cc,h}`,
  `builtin_function_options.*`, `sql_function.*`, `templated_sql_function.*`,
  `non_sql_function.*`, `anon_function.*`, `procedure.*`,
  `table_valued_function.*`, `sql_tvf.*`, `templated_sql_tvf.*`.
- **Coercion/cast**: `coercer.{h,cc}`, `cast.{h,cc}`, `convert_type_to_proto.*`.
- **Evaluator**: `evaluator.{h,cc}`, `evaluator_base.*`, `evaluator_lite.h`,
  `prepared_expression_constant_evaluator.*`, `constant_evaluator.h`.
- **Modules**: `modules.*`, `module_details.*`, `module_factory.*`,
  `module_contents_fetcher.*`, `file_module_contents_fetcher.*`.
- **Parsing/formatting helpers**: `parse_helpers.*`, `parse_location.*`,
  `parse_tokens.*`, `sql_formatter.*`, `lenient_formatter.*`, `formatter_options.*`.
- **Collation/strings**: `collator.*`, `collator_lite.*`, `strings.*`.
- **Values & scalar types**: `value.{h,cc}`, `numeric_value.*`, `json_value.*`,
  `uuid_value.*`, `interval_value.*`, `timestamp_picos_value.*`, `civil_time.*`,
  `pico_time.*`, `numeric_parser.*`, `time_zone_util.*`.
- **Errors/diagnostics**: `error_helpers.*`, `literal_remover.*`,
  `feature_label_extractor.*`, `cycle_detector.*`.
- **Misc**: `id_string.*`, `proto_util.*`, `proto_value_conversion.*`,
  `constant.*`, `sql_constant.*`, `property_graph.*`, `simple_property_graph.*`,
  `measure_expression.*`, `aggregation_threshold_utils.*`, `anonymization_utils.*`.
- **Subdirs**:
  - `types/` — the type-system hierarchy (`type.*`, `type_factory.*`,
    `simple_type.*`, `array_type.*`, `struct_type.*`, `proto_type.*`,
    `enum_type.*`, `map_type.*`, `range_type.*`, `row_type*.*`,
    `extended_type.*`, `graph_element_type.*`, `graph_path_type.*`,
    `measure_type.*`, `value`/`type` modifiers, parameters, deserializer).
    → [type-system.md](components/type-system.md).
  - `functions/` — engine-shared builtin **implementations** (arithmetic, math,
    string, date/time, convert, json, regexp/like, net, hash, bitwise,
    percentile, range, distance, compression, uuid, …).
  - `annotation/` — type annotations (collation, default annotation spec).
  - `information_schema/`, `proto/` — schema & wire-format protos.
  - `testing/` — public test helpers/matchers.

### `googlesql/reference_impl/`
Reference execution engine (algebrize → evaluate).

- `algebrizer.{h,cc}` (+ `algebrizer_graph.cc`) — Resolved AST → operator tree.
- `operator.{h,cc}` — base `AlgebraNode`/`AlgebraArg` classes.
- `value_expr.cc` — scalar expression operators (`ValueExpr::Eval`).
- `relational_op.cc` — relational operators (scan/filter/join/sort/union),
  iterator-based.
- `aggregate_op.cc`, `analytic_op.cc`, `pattern_matching_op.cc` — GROUP BY,
  window functions, MATCH_RECOGNIZE.
- `tuple.{h,cc}`, `tuple_comparator.*`, `variable_id.h`, `variable_generator.*` —
  row/tuple representation and intermediate-value identifiers.
- `function.{h,cc}` — builtin function evaluation (largest file in the dir).
- `evaluation.{h,cc}` — `EvaluationContext` (tables, params, RNG, deadlines).
- `uda_evaluation.*`, `tvf_evaluation.*`, `measure_evaluation.*` — UDA/TVF/measure
  execution.
- `statement_evaluator.{h,cc}` — high-level statement execution API.
- `reference_driver.{h,cc}` — implements the compliance `TestDriver`.
- `common.*`, `proto_util.*`, `type_helpers.*`, `parameters.h`,
  `rewrite_flags.*`, `expected_errors.*` — utilities.
- `functions/` — pluggable/specialized function impls (`hash`, `json`, `like`,
  `compression`, `string_with_collation`, `range`, `map`, `uuid`, `graph`,
  `register_all`).
- `*_test.cc` — per-operator and function tests.

### `googlesql/common/`
Shared helpers + the bulk of builtin function **definitions** (signatures).

- `errors.*`, `status_payload_utils.*`, `scope_error_catalog.*`,
  `warning_sink.*` — error/warning construction.
- `builtin_function_*.cc` / `builtin_function_internal*.{h,cc}` /
  `builtin_function_map.cc` / `builtin_tvfs.*` — definitions/registration of the
  builtin function & TVF library (split by category for compile speed).
- `type_visitors.*`, `type_and_argument_kind_visitor.*`, `constant_utils.*`,
  `internal_analyzer_options.h`, `resolution_scope.h`, `function_utils.*`,
  `options_utils.*`, `measure_utils.*`, `measure_analysis_utils.*`,
  `graph_element_utils.*` — analyzer-side helpers.
- `proto_helper.*`, `proto_from_iterator.*`, `reflection_helper.*`,
  `initialize_required_fields.*` — proto/reflection helpers.
- `internal_value.*`, `multiprecision_int*.*`, `json_parser.*`, `json_util.*`,
  `unicode_utils.*`, `utf_util.*`, `canonicalize_signed_zero_to_string.*`,
  `float_margin.h` — value/number/text utilities.
- `evaluator_registration_utils.*`, `simple_evaluator_table_iterator.*`,
  `lazy_resolution_catalog.*`, `evaluator_test_table.h` — evaluation/catalog glue.
- `thread_stack.*`, `timer_util.h`, `string_util.h`, `box_glyphs.h` — misc.
- `match_recognize/` — NFA engine for `MATCH_RECOGNIZE` (`nfa*`, `nfa_builder`,
  `compiled_nfa`, `epsilon_remover`, edge/row matchers, `nfa_match_partition`).
- `search/` — full-text search token-list utilities.
- `testing/` — proto/status matchers.

### `googlesql/base/`
Vendored foundation library (Abseil-/Google-internal style). Not SQL-specific.
- Status/errors: `status.h`, `status_builder.*`, `status_macros.h`, `ret_check.*`,
  `canonical_errors.h`, `status_payload.*`, `check.h`.
- Memory: `arena.*`, `arena_allocator.*`, `no_destructor.h`,
  `compact_reference_counted.h`.
- Containers/views: `flat_set.h`, `flat_internal.*`, `general_trie.h`,
  `map_util.h`, `map_view.h`, `stl_util.h`, `optional_ref.h`.
- Numeric/bits: `exactfloat.*`, `mathutil.*`, `bits.*`, `castops.h`,
  `lossless_convert.h`, `endian.h`, `unaligned_access.h`.
- Strings/system: `string_numbers.*`, `case.*`, `edit_distance.h`, `logging.*`,
  `clock.*`, `path.*`, `file_util.h`, `time_proto_util.*`, `source_location.h`,
  `varsetter.h`, `die_if_null.h`, `atomic_sequence_num.h`, `enum_utils.h`,
  `requires.h`.
- `net/` — `ipaddress`, `public_suffix`, `idn` (`*_oss` variants for open source).
- `testing/` — `status_matchers`, `proto_matchers`, gtest main.

### `googlesql/scripting/`
Procedural multi-statement scripts.
- `parsed_script.{h,cc}`, `script_segment.*`, `parse_helpers.*` — parsed script
  model and segmentation.
- `control_flow_graph.{h,cc}` — control-flow graph (branches, loops, exceptions,
  variable scope).
- `script_executor.h`, `script_executor_impl.{h,cc}`, `stack_frame.h`,
  `type_aliases.h` — execution loop and variable state.
- `serialization_helpers.*`, `error_helpers.*` — checkpoint/resume and errors.

### `googlesql/compliance/`
Engine-agnostic conformance suite.
- `test_driver.{h,cc}` — abstract `TestDriver` engines implement.
- `sql_test_base.*` — base for file- and code-based tests.
- `compliance_test_cases.{h,cc}`, `functions_testlib*.cc` — code-based function
  test cases.
- `compliance_label_extractor.*` — labels for filtering/coverage.
- `known_errors/*.textproto` — per-engine expected-failure lists.
- `testdata/*.test` — ~100 golden SQL conformance files.

### `googlesql/testing/` & `googlesql/testdata/`
Shared test infrastructure.
- `testing/`: `test_function.*` (function test-case struct), `test_value.*`,
  `test_catalog.*`, `sql_types_test.*`, `type_util.*`,
  `test_module_contents_fetcher.*`.
- `testdata/`: `sample_catalog*.{h,cc}` (large prebuilt catalog),
  `populate_sample_tables.*`, `test_schema.proto` + many test protos,
  `sample_annotation.*`, `sample_system_variables.*`, `error_catalog.*`,
  `modules/`, `proto_dag_like/`.

### `googlesql/local_service/`
gRPC/JNI bridge for the Java client.
- `local_service.proto` — the `GoogleSqlLocalService` RPC surface (Prepare,
  Evaluate, Analyze, …).
- `local_service.{h,cc}`, `local_service_grpc.*`, `state.h` — implementation.
- `local_service_jni.{h,cc}` — JNI entry for in-process Java calls.

### `googlesql/tools/`
- `execute_query/` — parse/analyze/execute playground (CLI + web). Key files:
  `execute_query.cc` (binary/flags), `execute_query_tool.{h,cc}` (modes/config),
  `execute_query_web*.{h,cc}` + `web/` (web UI), `execute_query_writer.*` &
  `output_query_result.*` (output formats), `selectable_catalog.*` (tpch/sample),
  `execute_query_prompt.*`/`execute_query_loop.*` (REPL).
- `formatter/internal/` — SQL formatter internals (`token`, `chunk`, `layout`,
  `parsed_file`, `fusible_tokens`, `chunk_grouping_strategy`, `range_utils`). The
  public API is `public/sql_formatter.*` / `public/lenient_formatter.*`.

### `googlesql/proto/`
Shared protobuf definitions used across components: `options.proto`,
`function.proto`, `simple_catalog.proto`, `simple_property_graph.proto`,
`module_options.proto`, `script_exception.proto`, `internal_error_location.proto`,
`internal_fix_suggestion.proto`, `anon_output_with_report.proto`,
`placeholder_descriptor.proto`.

### `googlesql/examples/`
- `tpch/` — TPC-H catalog (`catalog/tpch_catalog.*`), generated dataset, and
  `all_queries.sql`.
- `pipe_queries/` — pipe-syntax example scripts.

---

## `java/` & `javatests/`
Java bindings (~96 `.java` files) wrapping the C++ library **via the
local_service gRPC/JNI bridge**: `Client.java`/`ClientChannelProvider.java`
(stub/channel), type & catalog wrappers (`SimpleCatalog`, `SimpleTable`, `Type`,
`Value`, `FunctionSignature`, …), `PreparedExpression`/`PreparedQuery`, and the
generated `resolvedast/` classes. Tests in `javatests/` exercise these against a
running service. See [tooling-and-bindings.md](components/tooling-and-bindings.md).
