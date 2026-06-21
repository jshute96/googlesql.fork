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
| A4 | **Scan kind unsupported in pipe syntax** | `IsScanUnsupportedInPipeSyntax()` `sql_builder.cc:367`; wrapped at `:3288`, `:3806` | `ResolvedAnonymizedAggregateScan` and `ResolvedAggregationThresholdAggregateScan` have no pipe spelling; the consumer wraps them as a parenthesized standard subquery via `WrapForPipeSyntaxMode`. |

A1 is detected on the assembled `QueryExpression`; A4 is detected per-scan by
kind. Both are genuine language limitations — there is nothing to "fix" without
new pipe syntax.

### A2 / A3 (resolved): group-by keys without/with duplicate aliases

Two former fallbacks have been **removed** — these queries now generate pipe
syntax via generated aliases:

- **A2 — group-by key without an alias.** A `|> AGGREGATE … GROUP BY <key>` must
  reference every key by an alias, which means the key has to be materialized in
  the select list. A key the resolver pruned from the scan output (or that a
  side-effect/collapsed scan never materialized — e.g.
  `SELECT f(x) FROM t GROUP BY k` where `k` is not output) used to have no alias
  and forced a fallback. `SQLBuilder::GetSelectList` (`sql_builder.cc:2630`) now
  appends any such key to the select list with a generated unique alias
  (`GenerateUniqueAliasName`) and records its ordinal, so the key is emitted as
  `|> AGGREGATE … GROUP BY <expr> AS <alias>`. The extra output column is pruned
  by a later operator (and by the resolver on re-analysis), so the round trip is
  preserved.

- **A3 — duplicate group-by aliases.** `GetSelectList` already assigns a
  *distinct* alias to every distinct group-by column (`UpdateColumnAlias`
  suffixes collisions), so two different keys never share an alias. A column
  intentionally repeated in `ROLLUP`/`CUBE`/`GROUPING SETS` (e.g.
  `GROUP BY ROLLUP(x, y, x)`) reuses its own alias, which is valid pipe syntax.
  The duplicate-alias fallback check was therefore obsolete and over-conservative
  and has been deleted.

`CanFormPipeSQLQuery` (`query_expression.cc:689`) now keeps only a defensive
`AllGroupByColumnsHaveAliases()` guard (it should always hold given the
materialization above; if some unforeseen path leaves a key unaliased, it falls
back rather than crashes while rendering).

> **Implementation note — the ordinal/SQL ambiguity.** A group-by list entry
> stores *either* a select-list ordinal *or* the key's raw expression SQL in the
> same string field. These are indistinguishable for an integer-valued key
> expression (`GROUP BY 1+2`, `GROUP BY CAST(1 AS INT64)` whose SQL is `"1"`), so
> the materialization tracks *explicitly* which keys already have a select
> ordinal (`materialized_group_by_columns`) instead of trying to parse the stored
> value. The `select_list->empty()` placeholder-`NULL` branch is likewise
> conditioned on the select list, not on `column_list`, so a key materialized for
> an otherwise-empty column list does not trip the dummy-`NULL` path.

---

## B. Cases where ResolvedScans aren't 1:1 with pipe operators

### B.1 Many scans → fewer (or zero) operators (collapse)

| # | Case | Where | Status |
|---|------|-------|--------|
| 1 | **Degenerate / pass-through `ProjectScan` → 0 operators.** A `ProjectScan` that only forwards its input columns (no computed columns, no hints, same column list) is dropped. | `IsDegenerateProjectScan()` `sql_builder.cc:3209`; short-circuit at `:3295` | **Fixable — should emit it.** |
| 2 | **Group-by-key `ProjectScan` folded into `AGGREGATE`.** A `ProjectScan` that computes the grouping-key expressions directly below an `AggregateScan` is recorded in `scans_to_collapse_` and inlined into `|> AGGREGATE … GROUP BY <expr>`, dropping its `EXTEND`. 2 scans → 1 operator. | `scans_to_collapse_` `sql_builder.cc:292`, `:3303`, `:5677`; forced-extend computation `:5583` | **Fixable — should emit the ProjectScan.** |
| 3 | **Leaf `TableScan` absorbed into the `FROM` head.** A table scan becomes the bare `FROM <table>` root that the next operator appends onto, instead of being wrapped under its own alias. | `TryUseLeafTableScanColumns()` `sql_builder.cc:11904` | **Expected.** `FROM Table` ↔ a `TableScan` is the natural correspondence, not a deviation. |
| 4 | **`ResolvedSubpipelineInputScan` → 0 operators.** The subpipeline input is an empty pipe head (marked `MarkAsPipeOperatorChain`), not an operator. | `query_expression.h:278`; chain handling in `GetInputPipeSQL` `sql_builder.cc:11880` | **Expected.** The subpipeline input is the head the operators attach to; it has no operator of its own. |

**Fix direction for #1 and #2.** Both are *simplifications* the SQLBuilder
performs on the resolved output in pipe mode, and both work against the 1:1
goal. We do not need them:

- **#1:** stop eliding trivial `ProjectScan`s in pipe mode — just emit the
  `|> SELECT`/`|> EXTEND`. (`IsDegenerateProjectScan` already declines to fire
  in one pipe sub-case at `:3215`; the general degenerate short-circuit at
  `:3295` is the part to drop for pipe.)
- **#2:** stop deferring the group-by-key `ProjectScan` so it can be inlined into
  the `AggregateScan`. Emit a `ProjectScan` (`|> EXTEND`) for it if the query had
  a pipe projection operator, and let the `AggregateScan` reference the resulting
  columns by name. This removes the `scans_to_collapse_` machinery for this case.

If we remove the extra code that performs these two simplifications, the emitted
pipe chain regains one operator per scan. The cost is more verbose output (an
explicit `|> SELECT`/`|> EXTEND` where today there is none), which is the
intended trade for round-trip fidelity.

### B.2 One scan → multiple operators (expand)

| # | Case | Where |
|---|------|-------|
| 5 | **`AggregateScan` → `|> AGGREGATE` plus extra columns / a trailing `|> SELECT`.** When the aggregate groups by columns the resolver pruned from its output, those keys are re-added to the select list and emitted as extra trailing `AGGREGATE` output columns, then dropped by a later projection. The aggregate's output projection is also appended as its own operator to restore column shape. | grouping-key re-add `sql_builder.cc:5677–5715`; output projection appended as an operator (see `ProcessAggregateScanBase`) |

This expansion exists to satisfy the rule that `|> AGGREGATE … GROUP BY` emits an
output column per grouping key. It is the inverse of #2 — once #2 stops
collapsing the key projection, much of this bookkeeping can be reconsidered.

### B.3 Reordering and synthetic operators (no positional 1:1)

These don't change the operator *count* per scan so much as break the
*correspondence* between scan order and operator order.

| # | Case | Where |
|---|------|-------|
| 6 | **Canonical clause ordering.** Because the accumulate path serializes clauses in a fixed order, the operator sequence follows `WHERE → AGGREGATE → ORDER BY → SELECT → LIMIT`, not the bottom-up scan order. | `GetPipeSQLQuery()` `query_expression.cc:743–793` |
| 7 | **Implicit final `|> SELECT`.** A trailing `SELECT` operator is emitted from the accumulated select list even when no `ProjectScan` produced it — and is *suppressed* when a `GROUP BY` already covers the output. | `query_expression.cc:781` |
| 8 | **Synthetic `|> AS <alias>` at wrap boundaries.** When a clause slot is already filled and the accumulate path must wrap, pipe mode injects `… |> AS <alias>`, an operator that corresponds to no scan. | `WrapImpl` `query_expression.cc:837` |

B.3 is inherent to the accumulate path. Moving more scans onto the
append-as-you-go path (strategy 2) is the structural fix; the per-case fixes for
#1 and #2 are the highest-value steps because they remove the two most common
collapses.

---

## Summary

- **A1, A4**: pipe syntax genuinely can't express the query (SELECT hints;
  anonymized / aggregation-threshold scans) → fall back to standard SQL. Not
  fixable without new syntax.
- **A2, A3** (resolved): group-by keys without aliases / with duplicate aliases
  now generate pipe syntax via generated, uniquified aliases — no more fallback.
- **B1 #1, #2**: SQLBuilder *simplifications* (elide trivial `ProjectScan`;
  inline group-by-key `ProjectScan` into `AGGREGATE`). **Remove them** — emit the
  `ProjectScan` instead — to get back to 1:1.
- **B1 #3, #4**: expected and correct (`FROM Table` ↔ `TableScan`; subpipeline
  input head).
- **B2 #5**: aggregate expansion, partly a consequence of #2.
- **B3 #6–#8**: ordering/synthetic-operator artifacts of the accumulate path;
  structurally addressed by preferring the append path.
