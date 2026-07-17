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
  The generated headers aren't in the **source tree** — to *change* a node/field, edit
  `gen_resolved_ast.py`; to *read* the actual generated C++, look under `bazel-bin/`
  after a build (e.g. `bazel-bin/googlesql/resolved_ast/resolved_ast.h`,
  `bazel-bin/googlesql/parser/parse_tree_generated.h`).
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
- **`ResolvedColumn::name()` / `table_name()` return `std::string` *by value***
  (`resolved_column.h` — they call `IdString::ToString()`). Never bind the result
  to an `absl::string_view` (it dangles immediately) — this has caused a real
  use-after-free in `SQLBuilder`. `IdString::ToStringView()` and protobuf `.name()`
  accessors are safe (they return into stable storage).
- **Analyzer golden tests:** each `googlesql/analyzer/testdata/<name>.test` has its own
  target `//googlesql/analyzer:analyzer_<name>_test`. The framework re-round-trips
  SQLBuilder output (standard **and** pipe) and fails on invalid SQL. There is **no
  turnkey "update goldens" command** — see the `regen-analyzer-goldens` skill.

## Build & run (Bazel)

```bash
bazel build ...                                              # build everything
bazel test //googlesql/parser:parser_set_test               # run a test
bazel run //googlesql/tools/execute_query:execute_query -- --web   # interactive tool
```

`tzdata` is required. A `Dockerfile` provides a pinned toolchain. The Bazel
version is pinned in `.bazelversion`. See `README.md` for full instructions and
`execute_query.md` for the tool.

Some hosts need more than these plain commands — the conditional subsections
below (ecryptfs, low-resource, restricted-network) each state the condition
they apply under. Skip any whose condition doesn't describe your machine.

### What a build actually needs

- **Bazel** at the `.bazelversion` version (install it if the container has
  none — e.g. the `bazel-<ver>-linux-x86_64` release binary).
- A **C++20** compiler. The default `--config=clang` (see `.bazelrc`) builds
  with an LLVM toolchain that `toolchains_llvm` downloads, so a host clang is
  not strictly required, but `tzdata` is.
- A host **Go** toolchain and host **autotools** (`make`, `cmake`, `ninja`,
  `pkg-config`, `autoconf`, `automake`, `m4`). `MODULE.bazel` is configured to
  use the host's Go (`go_sdk.host()`) and the preinstalled `rules_foreign_cc`
  toolchains rather than downloading them — the Go tool `textmapper` (lexer gen)
  and the ICU build (via `rules_foreign_cc`) depend on these being installed.
- First build is **slow**: the generated `resolved_ast` sources and ICU
  (autoconf + make) dominate. Expect tens of minutes from a cold cache.

### Iterating: rebuild costs and gotchas

- **Edits to `resolved_ast/*.{h,cc}` are expensive** — they force the generated
  sources to recompile, and that dominates the rebuild (tens of minutes on a
  modest host, versus seconds for a tool / JS / CSS edit). Batch AST-header
  edits together before kicking off a build rather than iterating one at a time.
- **`execute_query`'s web assets are embedded at build time.** Editing
  `tools/execute_query/web/*.{js,css}` needs a full relink *and* a server
  restart — a reload won't pick it up. To confirm the new binary really contains
  the change:
  `grep -a -c "<marker-string>" bazel-bin/googlesql/tools/execute_query/execute_query`
- **Bazel's JVM writes crash dumps (`hs_err_pid*.log`, `replay_pid*.log`) into
  the repo root.** They show up as untracked files; delete them before committing.

### Building on an ecryptfs home directory

If Bazel's cache/output base lives on an **ecryptfs mount with filename
encryption** (check: `mount | grep ecryptfs` shows an `ecryptfs_fnek_sig=`
option), encrypted filenames expand past the 255-byte limit and builds fail with
`build-runfiles failed: File name too long`, plus spurious "reading symlink …
No such file or directory" errors. Two flags avoid staging a runfiles tree:

```bash
bazel build --config=clang --nobuild_runfile_links --dynamic_mode=off <target>
bazel test  --config=clang --nobuild_runfile_links --dynamic_mode=off \
            --test_output=errors <target>
```

`--nobuild_runfile_links` alone isn't enough for test targets — they link a
`.so` solib tree, which `--dynamic_mode=off` (static link) avoids.

For the same reason, **run tools by their built binary rather than `bazel run`**
(which stages runfiles and fails):
`./bazel-bin/googlesql/tools/execute_query/execute_query --web --port=8080`.
The web assets are embedded, so no runfiles are needed. Binaries that read
tzdata directly may need `TZDIR=/usr/share/zoneinfo`.

### Low-resource hosts (no swap, or a near-full disk)

Lower `--jobs` (e.g. `--jobs=2`) — the default fans out to core count and the
heavy C++ translation units spike memory and scratch space together.

Interpret failures accordingly: a **slow clang SIGSEGV/SIGBUS/ICE deep in the
build, on a file unrelated to your change, is resource pressure, not a compiler
bug** — despite LLVM's "please file a bug" message. Lower `--jobs` and rerun;
Bazel caches successful actions, so retries converge. By contrast a *fast*
failure with a clear diagnostic is a real compile error worth reading.

### Building behind a restricted-network sandbox (e.g. Claude Code on the web)

If the egress proxy uses a TLS-inspecting allowlist (GitHub + a few hosts
allowed; `bcr.bazel.build`, `mirror.bazel.build`, `go.dev`/`dl.google.com`
blocked), two non-committed settings get a build working — put them in
`~/.bazelrc` (or a `SessionStart` hook):

```
# Trust the proxy's TLS-inspection CA in Bazel's server JVM (avoids PKIX errors).
# The OS-managed system cacerts already contains it.
startup --host_jvm_args=-Djavax.net.ssl.trustStore=/etc/ssl/certs/java/cacerts
startup --host_jvm_args=-Djavax.net.ssl.trustStorePassword=changeit

# bcr.bazel.build is blocked; use the GitHub-hosted mirror of the registry.
common --registry=https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/
```

The host-Go / host-tool settings above (in `MODULE.bazel`) are the committed
half of the same story: they avoid the blocked Go-SDK and tool-source mirrors.

## Background reading

The README links external papers worth skimming: *GoogleSQL: A SQL Language as a
Component* (CDMS 2022), *Spanner: Becoming a SQL System* (SIGMOD 2017, §6), and
*SQL Has Problems. We Can Fix Them: Pipe Syntax in SQL* (VLDB 2024).
