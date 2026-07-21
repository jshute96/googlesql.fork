# Known issues

## SQLBuilder emits `|> STATIC_DESCRIBE` inside a subquery after the 2026-07 upstream sync

**Status:** open — surfaced while catching branches up to the upstream sync
(`google/googlesql` master, merge `7f489285`).

**Symptom:** `analyzer_pipe_static_describe_test` fails with

```
ERROR while analyzing SQLBuilder output:
  Syntax error: Expected keyword JOIN but got "|>"
```

The SQLBuilder regenerates SQL in which a `|> STATIC_DESCRIBE` pipe operator is
placed **inside a parenthesized subquery**, where pipe operators are not legal.
Two `testdata/pipe_static_describe.test` cases trigger it — `STATIC_DESCRIBE`
applied after a join:

- `FROM TestExtraValueTable vt CROSS JOIN KeyValue kv |> STATIC_DESCRIBE`
- `FROM KeyValue kv1 FULL JOIN KeyValue kv2 USING (key) |> STATIC_DESCRIBE`

**Root cause (narrowed, not fixed):** this is an interaction between the
`STATIC_DESCRIBE` SQLBuilder emission (branch commit `0568dadf`, "SQLBuilder:
emit |> STATIC_DESCRIBE instead of dropping it") and upstream's changes to how
the SQLBuilder wraps/joins queries. At `0568dadf` (pre-sync) the golden had **no**
such error; it appears only after merging upstream. It is *not* a merge-conflict
resolution error — the C++ merged without conflicts on this branch.

**Scope:** any branch carrying `0568dadf` plus the upstream sync. Confirmed on
`claude/epic-hypatia-K5jqS-leaf-columns` (#9). **Very likely also affects
`claude/epic-hypatia-K5jqS` (#6)**, which carries the same feature commit + the
sync; `pipe_static_describe` did not conflict during that branch's catch-up, so
the test was not run there — needs verification.

**Fix direction:** in `googlesql/resolved_ast/sql_builder.cc`, the
`STATIC_DESCRIBE` handling must not let its `|>` land inside a subquery — either
render the enclosing query in pipe target-syntax mode (as generalized-query
statements do) or avoid the subquery wrap for this shape. Needs someone familiar
with the STATIC_DESCRIBE SQLBuilder work.

The `pipe_static_describe.test` golden has been regenerated to reflect the
current (failing) output so the diff is coherent; the test remains red until the
code is fixed.
