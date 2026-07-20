# Pipe-syntax SQLBuilder

How `SQLBuilder` (`googlesql/resolved_ast/sql_builder.cc`) re-emits a Resolved
AST as **pipe-syntax** SQL, and where that output is *not* 1:1 with the pipe
operators that the query started from.

See [resolved-ast.md](components/resolved-ast.md) for the SQLBuilder in general.
This doc is specific to the pipe target mode
(`TargetSyntaxMode::kPipe`, `IsPipeSyntaxTargetMode()`).

## The 1:1 goal

A pipe query is a `FROM` source followed by a chain of `|>` operators:

```sql
FROM t
|> WHERE x > 0
|> EXTEND x + 1 AS y
|> AGGREGATE COUNT(*) AS n GROUP BY y
|> ORDER BY n
```

The analyzer turns each pipe operator into (roughly) one `ResolvedScan`, and the
SQLBuilder turns each `ResolvedScan` back into one `|>` operator. When that holds
end to end, the round trip

```
pipe SQL → analyzer → Resolved AST → SQLBuilder → pipe SQL
```

reproduces the same operator chain. The **goal is to keep this as close to 1:1
as possible**: one input pipe operator → one `ResolvedScan` → one emitted pipe
operator. The cases below are where we deviate, either because pipe syntax can't
express the query at all, or because the SQLBuilder collapses / expands /
reorders scans relative to operators.

## Two rendering strategies

The deviations come from the SQLBuilder using two different code paths to build
pipe output:

1. **Accumulate-then-serialize** — the legacy standard-SQL path, reused for
   pipe. Several scans deposit clauses (`WHERE`, `GROUP BY`, `ORDER BY`,
   `SELECT`, `LIMIT`) into a *single* `QueryExpression`. `GetPipeSQLQuery()`
   (`query_expression.cc:734`) then serializes those clauses in a **fixed
   canonical order** — `FROM → WHERE → AGGREGATE → set-op → ORDER BY → SELECT →
   LIMIT` — regardless of the order the scans appeared. This is the main source
   of non-1:1 output: the operator sequence reflects the clause slots that got
   filled, not the scans that filled them.

2. **Append-as-you-go** — the newer path. A scan renders its input as pipe text
   via `GetInputPipeSQL` (`sql_builder.cc:11873`) and appends exactly one
   operator via `AppendPipeOperator` (`sql_builder.cc:11921`). This is the
   nearly-1:1 path; it is used for `WHERE`, `ORDER BY`, `JOIN`, array
   `JOIN`/`UNNEST`, set operations, `LIMIT`/`OFFSET`, `TABLESAMPLE`, and
   aggregates **inside subpipelines** (where the target mode is forced to pipe).

---

## A. Cases where we can't generate pipe syntax (fallback to standard)

In these cases the accumulated query cannot be expressed as a pipe chain, so
`GetPipeSQLQuery()` falls back to `GetStandardSQLQuery(...)` — emitting a
standard `SELECT` block (possibly wrapped as a subquery) with **zero** pipe
operators, even though the input had several.

| # | Case | Where | Why |
|---|------|-------|-----|
| A1 | **Query has SELECT hints** | `CanFormPipeSQLQuery()` `query_expression.cc:692` | Pipe syntax has no place for `SELECT @{hint}`. |
| A2 | **Scan kind unsupported in pipe syntax** | `IsScanUnsupportedInPipeSyntax()` `sql_builder.cc:367`; wrapped at `:3288`, `:3806` | `ResolvedAnonymizedAggregateScan` and `ResolvedAggregationThresholdAggregateScan` have no pipe spelling; the consumer wraps them as a parenthesized standard subquery via `WrapForPipeSyntaxMode`. |

A1 is detected on the assembled `QueryExpression`; A2 is detected per-scan by
kind. Both are genuine language limitations — there is nothing to "fix" without
new pipe syntax.

These are the only fallbacks. In particular, group-by keys never force a
fallback: every key is materialized in the select list with a unique alias, so a
`|> AGGREGATE … GROUP BY <key>` can always reference it.

- A key absent from the scan's output (pruned, or never materialized by a
  side-effect/collapsed scan — e.g. `SELECT f(x) FROM t GROUP BY k` where `k` is
  not output) is appended to the select list with a generated unique alias by
  `SQLBuilder::GetSelectList` (`sql_builder.cc:2630`) and emitted as
  `|> AGGREGATE … GROUP BY <expr> AS <alias>`. The extra output column is pruned
  by a later operator (and by the resolver on re-analysis), preserving the round
  trip.
- `GetSelectList` gives distinct columns distinct aliases (`UpdateColumnAlias`
  suffixes collisions), so two keys never share an alias. A column repeated in
  `ROLLUP`/`CUBE`/`GROUPING SETS` (e.g. `GROUP BY ROLLUP(x, y, x)`) reuses its
  own alias, which is valid pipe syntax.

`CanFormPipeSQLQuery` (`query_expression.cc:689`) keeps a defensive
`AllGroupByColumnsHaveAliases()` guard: it should always hold given the
materialization above, but if some path leaves a key unaliased it falls back
rather than crashing while rendering.

> **Note — the ordinal/SQL ambiguity.** A group-by list entry stores *either* a
> select-list ordinal *or* the key's raw expression SQL in the same string field,
> and the two are indistinguishable for an integer-valued key expression
> (`GROUP BY 1+2`, `GROUP BY CAST(1 AS INT64)` whose SQL is `"1"`). The
> materialization in `GetSelectList` therefore tracks *explicitly* which keys
> already have a select ordinal (`materialized_group_by_columns`) instead of
> parsing the stored value, and the placeholder-`NULL` branch is conditioned on
> `select_list` being empty rather than on `column_list`.

---

## B. Cases where ResolvedScans aren't 1:1 with pipe operators

### B.1 Many scans → fewer operators (collapse)

Two collapses remain, both expected:

| # | Case | Where | Status |
|---|------|-------|--------|
| 1 | **Leaf `TableScan` absorbed into the `FROM` head.** A table scan becomes the bare `FROM <table>` root that the next operator appends onto, instead of being wrapped under its own alias. | `TryUseLeafTableScanColumns()` `sql_builder.cc:11904` | Expected. `FROM Table` ↔ a `TableScan` is the natural correspondence. |
| 2 | **`ResolvedSubpipelineInputScan` → pipe head.** The subpipeline input is an empty pipe head (marked `MarkAsPipeOperatorChain`), the thing operators attach to, not an operator itself. | `query_expression.h:278`; `GetInputPipeSQL` `sql_builder.cc:11880` | Expected. |

In Pipe syntax mode every `ResolvedProjectScan` maps to a pipe operator,
including a **degenerate (pass-through) projection** — one that forwards its input
columns unchanged (no computed columns, no hints, same column list). It is
emitted as a `|> SELECT` of its columns rather than being elided, keeping the
scan↔operator mapping 1:1 (the `|> SELECT` is a no-op, and the columns also flow
under fresh aliases via the wrap that precedes it — see B.3). The elision is
still applied in Standard syntax mode, where a redundant nested `SELECT` would be
pure noise. See `VisitResolvedProjectScan` and `IsDegenerateProjectScan`
(`sql_builder.cc:3237`, `:3328`).

**There is no group-by-key `ProjectScan` collapse.** Group-by key expressions are
not separate scans — they live in the `AggregateScan`'s `group_by_list` and are
inlined into the single `|> AGGREGATE … GROUP BY <expr> AS <alias>` operator,
which is the 1:1 rendering of that one scan. A `ProjectScan` that genuinely
computes a value feeding an aggregate (e.g. `|> EXTEND x+1 AS k |> AGGREGATE …
GROUP BY k`) emits its own `|> SELECT`/`|> EXTEND`. The `scans_to_collapse_`
mechanism (`sql_builder.cc:292`) collapses only **side-effect-bearing
`AggregateScan`s** — those carrying a `ResolvedDeferredComputedColumn` from e.g.
`IFERROR` — so a dependent scan is nested correctly; it never contains a
`ProjectScan` (enforced by a `RET_CHECK` at `sql_builder.cc:3331`).

### B.2 One scan → multiple operators (expand)

| # | Case | Where |
|---|------|-------|
| 4 | **`AggregateScan` → `|> AGGREGATE` plus extra columns / a trailing `|> SELECT`.** When the aggregate groups by columns the resolver pruned from its output, those keys are re-added to the select list and emitted as extra trailing `AGGREGATE` output columns, then dropped by a later projection. The aggregate's output projection is also appended as its own operator to restore column shape. | grouping-key re-add `sql_builder.cc:5692–5715`; output projection in `ProcessAggregateScanBase` |
| 5 | **Constant / `ROLLUP`-`CUBE`-`GROUPING SETS` key → `|> EXTEND` + `|> AGGREGATE`.** A literal/constant key (rejected as a bare `GROUP BY` item) and any non-trivial key inside `ROLLUP`/`CUBE`/`GROUPING SETS` (where items can't carry aliases) are materialized by a preceding `|> EXTEND <expr> AS <alias>` and grouped by alias — one extra operator for the one `AggregateScan`. | forced-extend `ComputePipeGroupByForcedExtendColumns` `sql_builder.cc:5583`; `TryAppendGroupByClause` `query_expression.cc:408` |

These expansions satisfy pipe-syntax rules: `|> AGGREGATE … GROUP BY` emits an
output column per grouping key, a bare constant is not a valid grouping item, and
grouping-set constructs forbid inline aliases.

### B.3 Reordering and synthetic operators (no positional 1:1)

These don't change the operator *count* per scan so much as break the
*correspondence* between scan order and operator order.

| # | Case | Where |
|---|------|-------|
| 6 | **Canonical clause ordering + implicit final `|> SELECT`.** The accumulate path serializes clauses in a fixed order (`WHERE → AGGREGATE → ORDER BY → SELECT → LIMIT`), not the bottom-up scan order, and emits a trailing `SELECT` from the accumulated select list even when no `ProjectScan` produced it (suppressed when a `GROUP BY` already covers the output). | `GetPipeSQLQuery()` `query_expression.cc:743–793` |
| 7 | **Synthetic `|> AS <alias>` at wrap boundaries.** When a clause slot is already filled and the accumulate path must wrap, pipe mode injects `… |> AS <alias>`, an operator that corresponds to no scan. | `WrapImpl` `query_expression.cc:837` |

B.3 is inherent to the accumulate path. Moving more scans onto the
append-as-you-go path (strategy 2) is the structural fix.

---

## C. Column aliasing is *non-local* (and the deferred locality refactor)

### The current behaviour

A leaf `TableScan` used as a **join/subquery operand** (not the pipe spine) is
wrapped by `WrapQueryExpression` (`sql_builder.cc`) into the verbose, three-
operator form:

```sql
FROM Customer
|> SELECT Customer.C_CUSTKEY, Customer.C_NAME, ...   -- re-lists the table's own columns
|> AS customer_1
```

The column-listing `|> SELECT` exists mainly to give every column a globally
**unique, bare** alias (`GetColumnAlias` / `UpdateColumnAlias`, `sql_builder.cc:150`,
`:198`), so later operators can reference columns *unqualified*. When two scans
expose a column of the same name (a self-join, or `a JOIN b USING(key)`), one
side is renamed (`key` → `key_2`) so the bare names stay unique. That rename is
**non-local**: a scan's emitted column names depend on what the *other* operands
of the join contain.

`JOIN … USING` is the sharpest case. `USING(k)` *merges* the two `k` columns into
a single output column, so the SQLBuilder renames the right operand's columns to
the left's ids (`right_to_left_column_id_mapping`, `VisitResolvedJoinScan`) and
emits `USING(<renamed>)`. `USING` is emitted only to mirror that the *source*
query used `USING` (`build_using = node->has_using() && ValidateUsingScan(node)`);
there is already an `ON` fallback for shapes `USING` can't reconstruct.

### Readable aliases in pipe mode

In pipe target mode `GetColumnAlias` prefers a **readable** alias derived from the
column's own name (e.g. `k`, `int32`) over an opaque `a_<id>`, falling back to the
generated name for anonymous/`$`-prefixed/non-UTF-8 names
(`sql_builder.cc:150`). Uniqueness is preserved by suffixing
(`MakeUniqueColumnAlias`). *Fixed bug:* this branch read the column name through a
dangling `absl::string_view` bound to the by-value temporary `column.name()`
returns — a use-after-free that surfaced as a corrupted alias. It now owns the
name in a `std::string`.

### Deferred: the "locality" refactor

The goal of pipe rewrites is that **every operator can be emitted from itself
alone**, never depending on another operator's naming. That argues for:

1. **Leaf scans emit `FROM <table> AS <alias>`** directly, exposing columns as
   `alias.<col>` (range-var-qualified, locally determined) — dropping the
   redundant column-listing `SELECT`.
2. **Prefer `JOIN ON <qualified> = <qualified>` over `JOIN USING`**, so neither
   operand renames to match the other (the `ON` path already references columns
   qualified; no merge, no rename).
3. **Qualify the remaining bare references** (e.g. the TVF table-argument
   `SELECT`, `VisitResolvedTVFScan`) by `GetColumnPath` instead of the bare
   `GetColumnAlias`.

This was prototyped and **validated for regular SQL** (joins, `USING` joins,
TVFs, subqueries, set-ops all round-trip through re-analysis), but **deferred**.
Known blockers for a future dedicated effort:

- **GQL graph joins**: graph element columns (`GRAPH_NODE`/`GRAPH_EDGE`) have no
  `=` operator, so `USING` is *semantically required* there — step 2 must keep
  `USING` when the join columns are graph-element types.
- **Alias reservation**: step 1 must also reserve an alias per column the way
  `TryUseLeafTableScanColumns` does (`for col: GetColumnAlias(col)`,
  `sql_builder.cc`), so columns later derived from the scan stay globally unique.
  Omitting it is what exposed the use-after-free above.
- **Scope**: ~17 analyzer `[SQLBUILDER_OUTPUT]` goldens regenerate (pipe mode
  only; standard-mode output is untouched); the final semantic gate is the
  compliance suite.

A round-trip guard now backs this work: `run_analyzer_test` (default
`sqlbuilder_target_syntax_mode=both`) re-analyzes every SQLBuilder output, and
additionally builds it **with inline pipe-operator markers** enabled (see
[execute-query-visualizer.md](execute-query-visualizer.md)) and asserts the
marker-stripped result equals the unmarked one — catching any change that
perturbs the generated SQL structure.

---

## Validating a SQLBuilder change

The analyzer golden tests are the real correctness gate. **77** `testdata/*.test` files
carry `[default show_sqlbuilder_output]`, and the framework re-parses/re-analyzes the
generated SQL in *both* standard and pipe mode (`kSqlBuilderTargetSyntaxMode` defaults to
`"both"`) — so a SQLBuilder change that emits invalid or non-equivalent SQL fails loudly.
Distinguish the three failure signatures (don't conflate them):

| Test-log signature | Meaning |
|---|---|
| `BEGIN TEST DIFF` only, no error | benign golden churn — only the recorded `[SQLBUILDER_OUTPUT]` text changed |
| `ERROR while analyzing SQLBuilder output: …` | **real bug** — emitted invalid / non-equivalent SQL |
| `Pipe-operator markers changed the generated SQL structure` | **real bug** — the `/*S<n>*/` visualizer markers altered output |

Fast sweep across affected targets:
```bash
bazel test … $TARGETS --test_output=errors --keep_going 2>&1 \
 | grep -E 'ERROR while analyzing SQLBuilder output|markers changed the generated' \
 | sed -E 's/.*output: [A-Z_]+: //' | sort | uniq -c | sort -rn
```
Empty output = only golden diffs remain → regenerate them (see the `regen-analyzer-goldens`
skill). Watch for **pre-existing** baseline failures unrelated to your change (notably
`set_operation`, `pipe_with`, `pipe_tablesample`) — confirm with a `git stash` + rerun
before "fixing" them.

> **Implementation note — two `|>` emission paths.** Most operators emit their `|>` through
> the centralized `AppendPipeOperator` (which records `current_scan_`), but `AggregateScan`,
> `LimitOffsetScan`, the trailing `|> SELECT`, and `|> AS` wraps emit via
> `QueryExpression::GetPipeSQLQuery`/`WrapImpl` and are **not** recorded that way. Any
> correspondence built by counting textual `|>` against the recorded order drifts; the
> visualizer instead stamps inline `/*S<n>*/` markers. Converting `AggregateScan` to
> pipe-native `AppendPipeOperator` emission is **not** byte-equivalent (alias allocation +
> the "skip trailing `|> SELECT` when GROUP BY present" rule depend on lazy rendering) —
> don't retry that conversion.

## Summary

- **A1, A2**: pipe syntax genuinely can't express the query (SELECT hints;
  anonymized / aggregation-threshold scans) → fall back to standard SQL. Not
  fixable without new syntax. Group-by keys never fall back — they are always
  materialized with unique, uniquified aliases.
- **B.1**: leaf `TableScan` → `FROM` head and the subpipeline input head are
  expected. Every `ResolvedProjectScan` maps to a pipe operator, including a
  degenerate pass-through (emitted as a no-op `|> SELECT`); the elision applies
  only in Standard syntax mode. There is no group-by-key `ProjectScan` collapse.
- **B.2**: an `AggregateScan` can expand to more than one operator (extra grouping
  keys / output projection; forced `|> EXTEND` for constant and grouping-set
  keys) to satisfy pipe-syntax rules.
- **B.3**: canonical clause ordering, the implicit trailing `|> SELECT`, and
  synthetic `|> AS` wraps are artifacts of the accumulate path; the structural fix
  is to prefer the append path.
- **C**: join/subquery operands are rendered verbosely (`FROM t |> SELECT … |> AS
  a`) and their column names are assigned *non-locally* (cross-operand renaming
  for `USING`). A "locality" refactor (leaf `FROM t AS a`, prefer `ON` over
  `USING`, qualify references) is validated for regular SQL but **deferred** —
  blocked on GQL graph joins (need `USING`) and alias reservation. A
  use-after-free in `GetColumnAlias`'s readable-alias path was fixed.
