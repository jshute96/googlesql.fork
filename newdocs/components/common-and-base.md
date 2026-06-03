# Component: Common & Base (`googlesql/common/`, `googlesql/base/`)

Two foundational layers used by everything else. `base/` is generic
infrastructure; `common/` is GoogleSQL-specific shared logic (and, notably, the
builtin function *definitions*).

---

## `googlesql/base/` — vendored foundation library

**Role:** A self-contained, Abseil-/Google-internal-style utility library so
GoogleSQL doesn't depend on unreleased Google code. **Not SQL-specific.** Files
ending `_oss` are the open-source variants of internal-only implementations.

Grouped by theme (see [file-index.md](../file-index.md#googlesqlbase) for the
full list):

- **Status & errors** — `status.h`, `status_builder.*`, `status_macros.h`
  (`RETURN_IF_ERROR`, `ASSIGN_OR_RETURN`), `ret_check.*` (`RET_CHECK`),
  `canonical_errors.h`, `status_payload.*`, `check.h`. **These macros are used
  everywhere** in the codebase.
- **Memory** — `arena.*` / `arena_allocator.*` (fast bump allocation; the parser
  allocates AST nodes from an arena), `no_destructor.h`,
  `compact_reference_counted.h`.
- **Containers** — `flat_set.h`, `general_trie.h`, `map_util.h`, `map_view.h`,
  `stl_util.h`, `optional_ref.h`.
- **Numeric/bits** — `exactfloat.*` (exact float for precise comparisons),
  `mathutil.*`, `bits.*`, `castops.h`, `lossless_convert.h`, `endian.h`.
- **Strings/system** — `string_numbers.*`, `case.*`, `edit_distance.h`
  (suggestions for misspelled names), `logging.*`, `clock.*`, `path.*`,
  `file_util.h`, `time_proto_util.*`, `source_location.h`, `varsetter.h`.
- **`net/`** — `ipaddress`, `public_suffix`, `idn` — backing the SQL `NET.*`
  functions.
- **`testing/`** — `status_matchers`, `proto_matchers`, gtest main.

You rarely *change* `base/`; you *use* its macros and helpers.

---

## `googlesql/common/` — shared analyzer/runtime helpers

**Role:** GoogleSQL-specific code shared across the analyzer, reference impl, and
public API. Two big responsibilities stand out.

### 1. The builtin function library (definitions)
The **signatures** of GoogleSQL's standard functions are defined here (split into
many files purely to keep compile times reasonable):
- `builtin_function_internal*.{h,cc}`, `builtin_function_internal_1/2/3.cc` —
  core categories (array, string, date/time, math).
- `builtin_function_array.cc`, `builtin_function_comparison.cc`,
  `builtin_function_range.cc`, `builtin_function_time_series.cc`,
  `builtin_function_measure.cc`, `builtin_function_distance.cc`,
  `builtin_function_graph.cc`, `builtin_function_differential_privacy.cc`,
  `builtin_function_match_recognize.cc`, `builtin_function_sketches.cc` —
  per-domain definitions.
- `builtin_function_map.cc` — central registration into a catalog.
- `builtin_tvfs.*`, `builtin_enum_type.cc`, `builtins_output_properties.h`.

> Definitions (signatures) live here; **implementations** live in
> [`public/functions/`](public-api.md) and
> [`reference_impl/`](reference-impl.md). `public/builtin_function.*` is the API
> that calls into these definitions.

### 2. Error construction & analyzer helpers
- `errors.*` — factory functions for parse/analysis errors with location
  payloads; the standard way to produce a `Status` with a SQL position.
- `status_payload_utils.*`, `scope_error_catalog.*`, `warning_sink.*`.
- `type_visitors.*`, `type_and_argument_kind_visitor.*`, `constant_utils.*`,
  `internal_analyzer_options.h`, `resolution_scope.h`, `function_utils.*`,
  `options_utils.*` — analyzer-side helpers.
- `measure_utils.*`, `measure_analysis_utils.*`, `graph_element_utils.*`,
  `internal_property_graph.h` — feature-specific helpers.

### 3. Values, numbers, text
- `internal_value.*` (internal `Value` helpers), `multiprecision_int*.*`
  (big-integer math behind NUMERIC), `json_parser.*`/`json_util.*`,
  `unicode_utils.*`/`utf_util.*`, `canonicalize_signed_zero_to_string.*`,
  `float_margin.h`.

### 4. Proto / evaluation glue
- `proto_helper.*`, `proto_from_iterator.*`, `reflection_helper.*`,
  `initialize_required_fields.*` — proto descriptor & reflection support (protos
  are first-class SQL types).
- `evaluator_registration_utils.*`, `simple_evaluator_table_iterator.*`,
  `lazy_resolution_catalog.*`, `evaluator_test_table.h` — evaluation/catalog glue.
- `thread_stack.*` (recursion-depth guard), `timer_util.h`, `box_glyphs.h`.

### Subdirectories
- **`match_recognize/`** — the NFA engine for SQL `MATCH_RECOGNIZE` row-pattern
  matching: `nfa.*`, `nfa_builder.*`, `compiled_nfa.*`, `epsilon_remover.*`,
  edge/row matchers, `nfa_match_partition.*`. Consumed by the analyzer's
  match-recognize rewriter and the reference impl's `pattern_matching_op`.
- **`search/`** — full-text search token-list utilities (`TokenList`).
- **`testing/`** — proto/status-payload gtest matchers.

## Connections

`base/` underpins all of `googlesql/`. `common/` sits between `base/`/`public/`
and the [analyzer](analyzer.md)/[reference_impl](reference-impl.md): the analyzer
pulls builtin function definitions and error helpers from here; the reference
impl pulls value/proto/NFA helpers.
