# Component: Tooling & Language Bindings

Covers the user-facing tools and the out-of-process Java bindings:
`googlesql/tools/execute_query/`, `googlesql/tools/formatter/`,
`googlesql/local_service/`, `java/`, and `googlesql/examples/`.

---

## `tools/execute_query/` — the interactive playground

**Role:** A CLI + web tool to parse, analyze, and execute SQL using the
[reference implementation](reference-impl.md). The primary way to *see what
GoogleSQL does* with a query. (User guide: `execute_query.md` at repo root.)

It exposes the whole pipeline as selectable **modes**:
`--parse` (parse tree), `--unparse` (AST→SQL), `--resolve` (Resolved AST),
`--unanalyze` (Resolved AST→SQL), `--explain` (evaluator plan), `--execute` (run).
**Input modes:** query, single expression, or multi-statement script.

Key files:
- `execute_query.cc` — the binary; flags including `--web` (start the web server,
  default port 8080).
- `execute_query_tool.{h,cc}` — `ExecuteQueryConfig` and the core
  parse/analyze/execute driver tying the modes together.
- `execute_query_web*.{h,cc}` + `web/` (`page_body.html`, `style.css`,
  `embedded_resources.*`) — the web UI (assets embedded in the binary).
- `execute_query_writer.*`, `output_query_result.*`,
  `execute_query_proto_writer.*`, `execute_query_internal_*.cc` — output
  formatting (table / JSON / textproto / CSV / binproto).
- `execute_query_prompt.*`, `execute_query_loop.*`, `homedir.*` — the REPL.
- `selectable_catalog.*` — switch among built-in catalogs (e.g. `tpch`, `sample`).

Run it: `bazel run //googlesql/tools/execute_query:execute_query -- --web`.

---

## `tools/formatter/` — SQL formatter

**Role:** Reformat SQL text while preserving semantics. Public API is
[`public/sql_formatter.*`](public-api.md) (`FormatSql`) and
`public/lenient_formatter.*` (keeps comments / tolerates unparseable input).

Internals (`tools/formatter/internal/`): tokenize (`token.*`), group tokens into
`chunk.*` using `fusible_tokens.*` + `chunk_grouping_strategy.*`, compute
indentation/line-breaks (`layout.*`), driven by `parsed_file.*` and
`range_utils.*`.

---

## `local_service/` — gRPC/JNI bridge

**Role:** Wrap the C++ library as a service so **other languages (Java) can use
GoogleSQL out-of-process**. The Java client talks to this; it is *not* needed by
C++ callers.

- `local_service.proto` — defines `GoogleSqlLocalService` with RPCs: `Prepare`,
  `Evaluate`, `EvaluateStream`, `PrepareQuery`/`EvaluateQuery`,
  `PrepareModify`/`EvaluateModify`, `Analyze`, `GetTableFromProto`, etc.
- `local_service.{h,cc}` — `GoogleSqlLocalServiceImpl`: unpacks requests, calls
  the [analyzer](analyzer.md)/[evaluator](public-api.md), repacks results.
  Resolved ASTs, types, and values cross the wire via their protobuf forms.
- `local_service_grpc.*` — gRPC server glue.
- `local_service_jni.{h,cc}` — JNI entry point for in-process Java↔C++ calls.
- `state.h` — server-side state for prepared expressions/queries.

---

## `java/` & `javatests/` — Java bindings

**Role:** A Java API mirroring the C++ public API, implemented by **calling into
`local_service`** (gRPC or JNI). Java objects are thin wrappers; the real
analysis/evaluation happens in C++.

```
 Java code → SimpleCatalog/PreparedExpression/… → Client.getStub()
           → gRPC/JNI → local_service (C++) → analyzer / evaluator
```

- `com/google/googlesql/Client.java`, `ClientChannelProvider.java` — the gRPC
  stub & channel.
- Type/catalog/function wrappers: `SimpleCatalog`, `SimpleTable`, `SimpleColumn`,
  `Type`/`StructType`/`ProtoType`, `Value`, `TypeFactory`, `LanguageOptions`,
  `FunctionSignature`, `TVFSignature`, …
- `PreparedExpression.java`, `PreparedQuery.java` — evaluation entry points.
- `resolvedast/` — generated Java Resolved AST classes (from the same
  `gen_resolved_ast.py` as C++ — see [resolved-ast.md](resolved-ast.md)).
- `javatests/` — tests exercising these against a running service.

---

## `examples/`

Runnable demos used by `execute_query` and the docs:
- `tpch/` — a TPC-H `tpch_catalog.*`, a generated dataset, and `all_queries.sql`.
- `pipe_queries/` — examples of GoogleSQL's **pipe syntax** (`|>`), and TPC-H
  queries rewritten in pipe form.
