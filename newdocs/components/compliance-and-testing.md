# Component: Compliance & Shared Testing

Covers `googlesql/compliance/`, plus the shared test infrastructure in
`googlesql/testing/` and `googlesql/testdata/`.

---

## `googlesql/compliance/` — conformance test framework

**Role:** A reusable, **engine-agnostic** test suite that validates any SQL
engine behaves identically to GoogleSQL's semantics. This is one of GoogleSQL's
main deliverables: an engine implementer runs these tests against their engine to
prove conformance. The [reference implementation](reference-impl.md) is the
oracle the tests compare against.

### How it works

```
 .test golden file            TestDriver (engine-specific)
 (SQL + directives +   ──────▶  - create tables / load protos
  expected results)            - run statement on the engine under test
        │                      - run same statement on reference impl
        ▼                              │
  SQLTestBase orchestration            ▼
  - prepare DB, parse params      compare results
  - check known_errors            (skip if listed as a known error)
```

- **`test_driver.h`** — the abstract **`TestDriver`** interface an engine
  implements: table/proto setup, primary-key modes, statement execution, result
  comparison. (`reference_impl/reference_driver.*` is the built-in
  implementation.)
- **`sql_test_base.{h,cc}`** — `SQLTestBase`: drives both file-based and
  code-based tests; handles per-file setup (`[prepare_database]`,
  `[load_proto_files]`, …) and per-statement execution.
- **`compliance_test_cases.{h,cc}` + `functions_testlib*.cc`** — code-based test
  cases, especially exhaustive **function** tests (cast, format, intervals,
  arithmetic, distance, …) defined in C++.
- **`compliance_label_extractor.*`** — extract labels from cases for
  filtering/coverage reporting.
- **`known_errors/*.textproto`** — per-engine lists mapping test labels to
  expected failures (e.g. features not yet open-sourced). Lets an engine track
  what it doesn't yet support without the suite going red.
- **`testdata/*.test`** — ~100 golden files (aggregation, analytic, joins,
  arrays, etc.). Format: directives, SQL, and expected typed results.

### When you'd touch it
Adding/altering a language feature usually means adding `.test` cases here and
re-generating expected outputs; if the reference impl can't yet do something, it
goes in a `known_errors` list.

### Regenerating golden `.test` files

All these golden files (compliance `testdata/*.test` **and** the analyzer's
`googlesql/analyzer/testdata/*.test`) run through `file_based_test_driver`. There is
**no turnkey "update goldens" command** in the open-source export (`extract_test_output.py`
is not shipped). When the expected output legitimately changes:

- The driver writes the *regenerated* golden **into the test log** (flag
  `--file_based_test_driver_generate_test_output`, default `true`), between
  `****TEST_OUTPUT_BEGIN****` / `****TEST_OUTPUT_END****` markers (first block tagged
  `NEW_TEST_RUN <path>`; long lines split with a `***MERGE_TOO_LONG_LINE***` sentinel).
  The log is at `bazel-testlogs/<pkg>/<target>/test.log`; the `test.outputs/` dir is **empty**.
- **Don't use `patch`** — hunks are numbered per test-case so `@@` line numbers don't match
  the real file. Reassemble content-wise instead.
- For analyzer goldens (target `//googlesql/analyzer:analyzer_<name>_test`), the
  **`regen-analyzer-goldens` skill** automates this (find target → run → triage real bugs
  vs. benign churn → extract → verify), including a validated `extract_golden.py`.

---

## `googlesql/testing/` & `googlesql/testdata/` — shared test infrastructure

Used by unit tests across the whole repo (analyzer, reference impl, compliance).

**`testing/`** — helpers:
- `test_function.{h,cc}` — `QueryParamsWithResult` and friends: declare a
  function test case (inputs, expected output, expected error). The function
  compliance tests are built from these.
- `test_value.*` (`ValueConstructor`), `test_catalog.*` (minimal catalogs),
  `sql_types_test.*`, `type_util.*`, `test_module_contents_fetcher.*`.

**`testdata/`** — sample schemas & data:
- **`sample_catalog*.{h,cc}`** — `SampleCatalog`: a large, pre-populated
  `SimpleCatalog` (hundreds of tables/types/functions) that most analyzer tests
  resolve against. `sample_catalog_impl.*` is the bulk definition.
- `populate_sample_tables.*` — fills those tables with rows for execution tests.
- `test_schema.proto` (+ many other `.proto` files like `test_proto3.proto`,
  `bad_test_schema.proto`) — proto message/enum types exercised as SQL types
  (e.g. `KitchenSinkPB`).
- `sample_annotation.*`, `sample_system_variables.*`, `error_catalog.*`,
  `modules/`, `proto_dag_like/`.

### When you'd touch it
If a new test needs a table/type/proto that doesn't exist, you add it to the
sample catalog or a test proto here so multiple tests can share it.
