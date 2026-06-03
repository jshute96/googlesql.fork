# Component: Reference Implementation (`googlesql/reference_impl/`)

**Role:** GoogleSQL's own **execution engine** — an interpreter that runs a
[Resolved AST](resolved-ast.md) and produces result `Value`s. It exists to (a)
make queries actually executable for debugging via
[`execute_query`](tooling-and-bindings.md) and the [evaluator](public-api.md),
and (b) serve as the **oracle** for the [compliance suite](compliance-and-testing.md).

It optimizes for **correctness and completeness, not speed**. Production engines
do *not* use it; they translate the Resolved AST into their own plans.

## Execution pipeline

```
 Resolved AST  (from analyzer + rewriters)
      │
      ▼  algebrizer.cc :  Algebrize{Statement,QueryStatementAsRelation,Expression}
 Operator tree
   ├─ RelationalOp   (produces tuples: scan, filter, join, sort, aggregate, …)
   └─ ValueExpr      (produces a scalar Value: literal, column ref, call, cast, …)
      │
      ▼  SetSchemasForEvaluation()   (plan slot layout)
      ▼  CreateIterator() / Eval()   (with an EvaluationContext)
 TupleIterator  → TupleData rows → Values
      │
      ▼
 result rows
```

Two-phase: first **algebrize** (Resolved AST → operators), then **evaluate**
(operators → rows) using a pull-based ("Volcano") iterator model.

## Key pieces

### Algebrizer
- **`algebrizer.{h,cc}`** — the big translator: Resolved AST → operator tree.
  Entry points `AlgebrizeStatement`, `AlgebrizeQueryStatementAsRelation`,
  `AlgebrizeExpression`. Handles scans, filters, joins, aggregation, window
  functions, DML, subqueries.
- `algebrizer_graph.cc` — graph/GQL operators.

### Operators
- **`operator.{h,cc}`** — base classes: `AlgebraNode` and the `AlgebraArg`
  family (`ExprArg`, `RelationalArg`, `KeyArg`, `AggregateArg`, `AnalyticArg`,
  `ColumnFilterArg`).
- **`value_expr.cc`** — `ValueExpr` scalar operators; `Eval(params, context, …)`
  returns a `Value`.
- **`relational_op.cc`** — `RelationalOp` relational operators; `CreateIterator()`
  returns a `TupleIterator` that streams rows. Covers TableScan/Filter/Join/Sort/
  Project/Union/etc.
- **`aggregate_op.cc`** — `GROUP BY` and aggregate functions (accumulator model).
- **`analytic_op.cc`** — window functions (PARTITION BY / ORDER BY / frames).
- **`pattern_matching_op.cc`** — `MATCH_RECOGNIZE` (uses the NFA engine in
  [`common/match_recognize`](common-and-base.md)).

### Data representation
- **`tuple.{h,cc}`** — `TupleSchema`, `TupleData`, `TupleSlot`, `TupleIterator`:
  the in-memory row model. Slots are indexed by `VariableId`.
- `tuple_comparator.*` — ordering for sorts/joins.
- `variable_id.h`, `variable_generator.*` — unique ids for intermediate values.

### Function evaluation
- **`function.{h,cc}`** — runtime implementations of builtin functions (the
  largest file in the directory). Dispatches by function kind.
- `functions/` — pluggable/specialized impls registered separately: `hash`,
  `json`, `like`, `compression`, `string_with_collation`, `range`, `map`,
  `uuid`, `graph`, with `register_all.*` as the dispatcher.
- `uda_evaluation.*` (user-defined aggregates), `tvf_evaluation.*`
  (table-valued functions), `measure_evaluation.*` (measures).

### Context & integration
- **`evaluation.{h,cc}`** — `EvaluationContext`: tables, parameters, current
  timestamp, RNG, memory accounting, deadlines, cancellation. Threaded through
  every `Eval`/iterator.
- **`statement_evaluator.{h,cc}`** — high-level "run this statement" API (used by
  [scripting](scripting.md)).
- **`reference_driver.{h,cc}`** — implements the compliance `TestDriver` so the
  reference impl can act as the correctness oracle.
- Utilities: `common.*` (collation), `proto_util.*`, `type_helpers.*`,
  `parameters.h`, `rewrite_flags.*`, `expected_errors.*`.

## How it connects

- **Input:** a [Resolved AST](resolved-ast.md) (typically after rewriters, so it
  only sees the lowered core).
- **Depends on:** [public/](public-api.md) — `Value`, `Type`, `Catalog`,
  `Function`, `EvaluatorTableIterator`, `LanguageOptions`.
- **Used by:** [`public/evaluator`](public-api.md) (`PreparedExpression`/`Query`),
  [`tools/execute_query`](tooling-and-bindings.md) (via `reference_driver`),
  [compliance](compliance-and-testing.md) (as the reference engine),
  [scripting](scripting.md) (statement execution).

## Tests

Per-operator tests (`relational_op_test.cc`, `value_expr_test.cc`,
`aggregate_op_test.cc`, `analytic_op_test.cc`, `pattern_matching_op_test.cc`),
plus `algebrizer_test.cc`, `function_test.cc`, `tuple_test.cc`,
`evaluation_context_test.cc`, `type_helpers_test.cc`, `parameters_test.cc`. Much
of the real coverage comes from the [compliance suite](compliance-and-testing.md)
running through `reference_driver`.
