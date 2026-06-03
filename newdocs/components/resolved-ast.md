# Component: Resolved AST (`googlesql/resolved_ast/`)

**Role:** The Resolved AST is GoogleSQL's central **intermediate representation**
and the **contract between the library and embedding engines**. The
[analyzer](analyzer.md) produces it; the [reference implementation](reference-impl.md),
the rewriters, the SQLBuilder, and external engines consume it.

It is a fully **type-checked, name-resolved, engine-neutral** tree. Where the
parse tree is "what the user typed," the Resolved AST is "what it means."

User-facing node reference: `docs/resolved_ast.md` (generated).

## Key properties

- **~150 node classes** rooted at `ResolvedNode`, in three broad families:
  - `ResolvedStatement` — top-level statements (query, DML, DDL, CALL, …).
  - `ResolvedScan` — relational/table-producing nodes (table scan, join, filter,
    aggregate, project, set ops, …).
  - `ResolvedExpr` — scalar expressions (literal, column ref, function call,
    cast, subquery, …).
  - plus `ResolvedArgument` and many helper/argument nodes.
- **Every expression has a `Type`**; **every column is a `ResolvedColumn`** with a
  globally unique integer id (so column identity is unambiguous across the tree).
- **Immutable once built** (built via generated builders).

## Code generation (the important part)

Almost everything here is **generated** from one Python definition file:

- **`gen_resolved_ast.py`** — defines every node, its fields, field
  *ignorability* markers, and inheritance. Rendering the `*.template` files
  produces (for **both C++ and Java**):
  - `resolved_ast.{h,cc}` — node classes.
  - `resolved_ast_builder.h` — fluent builders for constructing nodes.
  - `resolved_ast_visitor.h` — visitor interface.
  - `resolved_ast_deep_copy_visitor.{h,cc}` — clone a subtree.
  - `resolved_ast_rewrite_visitor.{h,cc}` — the base class rewriters subclass.
  - `resolved_ast_comparator.{h,cc}` — tree comparison (tests).
  - `resolved_node_kind.{h,proto}` — the `ResolvedNodeKind` enum.
  - `resolved_ast.proto` — protobuf serialization messages.
  - `resolved_ast.md` — the published node-hierarchy doc.

**To add/change a resolved node you edit `gen_resolved_ast.py`.** This single
change ripples to the C++ classes, Java classes, builders, visitors, serializer,
validator hooks, and docs.

## Hand-written infrastructure

- **`resolved_node.{h,cc}`** — the base class: node-kind introspection,
  `Is<T>()`/`GetAs<T>()`, visitor dispatch, debug-string printing.
- **`resolved_column.{h,cc}` / `column_factory.{h,cc}`** — `ResolvedColumn`
  (id + name + type + source) and the factory that hands out unique column ids.
- **`resolved_collation.{h,cc}`** — collation metadata carried on nodes.
- **`validator.{h,cc}`** — the **`Validator`** checks a tree against a large set
  of invariants (every referenced column is in scope, types line up, node-specific
  constraints hold). Run after analysis/rewrites to catch malformed trees. This
  is a key correctness backstop when writing rewriters or constructing trees.

## Producing SQL back out

- **`sql_builder.{h,cc}`** — the **`SQLBuilder`** visitor turns a Resolved AST
  back into SQL text (the inverse of the analyzer). Used for debugging
  (`--unanalyze`), round-trip testing, and engines that re-emit SQL. Large and
  intricate because it must regenerate valid, equivalent SQL.
- **`query_expression.{h,cc}`** — a mutable scratch structure (SELECT/FROM/WHERE/
  GROUP BY/… clauses) the SQLBuilder assembles into final SQL.

## Transformation helpers

- **`rewrite_utils.{h,cc}`** — the toolkit rewriters depend on: remap/rename
  columns, build correlated references, copy subtrees with fresh column ids,
  construct common node patterns. If you write a rewriter, you live here.
- `resolved_ast_helper.*`, `make_node_vector*.h`, `node_sources.h`,
  `target_syntax.h` — assorted helpers.

## How it connects

- **Produced by:** [analyzer](analyzer.md) (`Resolver`) → `ResolvedStatement`.
- **Transformed by:** [analyzer/rewriters](analyzer.md#rewriters) via the
  generated `RewriteVisitor` + `rewrite_utils`.
- **Consumed by:** [reference_impl](reference-impl.md) (algebrizer), `SQLBuilder`,
  and external engines. Serializable via the generated protobuf for cross-process
  use (e.g. [local_service](tooling-and-bindings.md)).

## Tests

`resolved_ast_test.cc`, `resolved_node_test.cc`, `validator_test.cc` (large),
`column_factory_test.cc`, builder/visitor/deep-copy/rewrite tests,
`rewrite_utils_test.cc`, `resolved_collation_test.cc`, `resolved_column_test.cc`.
