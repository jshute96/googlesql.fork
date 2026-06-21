# execute_query query visualizer

A side-by-side visualization of a pipe-syntax query across its three
representations, built into the `execute_query` web tool (and available as
plain text in the CLI):

1. **Input SQL** — the original query.
2. **Resolved AST** — the analyzer output, rendered in *linear* (pipe-style)
   form so a scan sequence reads top-to-bottom like pipe operators.
3. **SQLBuilder SQL** — SQL regenerated from the Resolved AST by the
   `SQLBuilder` in pipe-syntax target mode.

The goal is an (approximately) 1:1 correspondence between an input pipe
operator, the `ResolvedScan`(s) it produced, and the regenerated pipe
operator(s) — and to let you click any item in one pane and see the
corresponding range highlighted in the others.

This doc explains how the pieces fit together and tracks remaining work. It is
prework that builds on four merged PRs (#6/#9 pipe SQLBuilder, #7 SQL box/HTML
formatters, #8 AST-node resolution info, #10 linear Resolved AST DebugString).

## Where the code lives

| Concern | File |
|---|---|
| Tool mode, generation of the three panes | `googlesql/tools/execute_query/execute_query_tool.cc` (`VisualizeQuery`) |
| `ToolMode::kVisualize` | `execute_query_tool.h` |
| Writer interface + text rendering | `execute_query_writer.h` (`VisualizationData`, `visualized()`) |
| Web template params | `execute_query_web_writer.h` (`visualized()` override) |
| Page markup (3-pane section, checkbox) | `web/page_body.html` |
| Layout / styling | `web/style.css` (the "Visualize mode" section) |
| UI behavior (resize, hide/show, click→details, correspondence) | `web/query_viewer.js` (the "Visualize mode" IIFE) |
| Input/SQLBuilder pane box HTML | `parser/box_formatter.h` (`SqlToBoxHtml` + `BoxAnnotator`) |
| Linear Resolved AST text | `resolved_ast/resolved_node.cc` (`DebugStringConfig::linear_mode`) |
| Per-AST-node resolver info (NameLists, titles) | `public/ast_node_resolved_info.h`, populated in `analyzer/resolver*.cc` |

## Pipeline

`VisualizeQuery(sql, ast, config, writer)` (in `execute_query_tool.cc`):

1. **Analyze** the parsed statement with rewriters disabled and parse locations
   recorded (`PARSE_LOCATION_RECORD_FULL_NODE_SCOPE`). Disabling rewriters keeps
   the Resolved AST structurally close to the input pipe shape; parse locations
   give each `ResolvedScan` a byte range back into the input SQL.
2. **Input pane**: `RenderBoxHtmlWithNodeInfo` runs `SqlToBoxHtml` over the input
   SQL with a `BoxAnnotator` that pulls each AST node's resolver info
   (`ASTNodeResolvedInfoMap`) into a hidden `.node-info` box (NameLists, titles).
3. **Resolved AST pane**: `ResolvedNode::DebugStringHtml()` — a structured
   linear emitter that produces one `.rscan` box per `ResolvedScan` (alternating
   `scan-a`/`scan-b` shading), nested `.rscan-query` blocks for non-pipe-input
   scan fields (subqueries, set-operation inputs), an enclosing `.rscan-stmt`
   block for the statement, and a `data-scan-id` per scan box. Scalar fields and
   non-scan children render as escaped text in their box.
4. **SQLBuilder pane**: run `SQLBuilder` in `TargetSyntaxMode::kPipe` on the
   Resolved AST and lenient-format the result. The SQLBuilder records, as a
   passive side-channel (`pipe_operator_scan_order()`), the original
   `ResolvedScan` responsible for each emitted `|>` (in output order).
   `SegmentPipeSqlToHtml` then splits the formatted SQL at each `|>` and wraps
   each segment in a `.rscan` box — the *same* markup the Resolved AST pane uses
   — tagged with that scan's `data-scan-id`. So the SQLBuilder pane lines up
   visually with the AST pane and the existing correspondence highlighting
   (keyed on `.rscan[data-scan-id]`) works across all three panes with no extra
   client code. The leading (FROM/source) segment is tagged with the source
   scan-id (0).
5. Hand a `VisualizationData` to the writer. Text writers print three labeled
   sections; the web writer fills `viz_*_html` template params.

## Web UI

When the **Visualize** output is active the page switches from the two-column
(editor | results) layout to a single stacked column so results use the full
window width (`html[data-visualize]` set by JS when a `.viz` block is present).

Each statement renders a `.viz` block:

- A row of up to three columns (`.viz-col` for `input` / `ast` / `sqlbuilder`),
  each with a header tab (`.viz-tab`) carrying an `×` hide button, and a
  scrollable body (`.viz-pane`). Columns are separated by draggable
  `.viz-divider`s.
- Hidden columns collapse; their re-add buttons (`+ Label`) appear in the
  top-right `.viz-readd-bar`, always in canonical order. At least one column
  must stay visible.
- A draggable `.viz-info-resizer` above a `.viz-info` details box. Clicking a
  node in any pane shows that node's info (and its enclosing nodes) here;
  otherwise it shows the placeholder "Details for selected node".

### Linked state across statements

Column visibility, column widths, and the details-box height are **linked**
across every `.viz` block on the page (a single `state` object in the JS,
pushed to all blocks by `applyState()`). Window resize: columns scale
proportionally automatically (flex weights); the details box shrinks
proportionally when the window shrinks but is not grown past ~10 lines by
window growth alone (manual drag may exceed that, up to half the viewport).

## Correspondence model (planned — Milestone 4)

The `ResolvedScan` (a linear pipe operator) is the correspondence hub:

- **Input SQL ↔ Resolved AST**: PR #8's `ASTNodeResolvedInfoMap` keys input
  pipe-operator / table AST nodes to scan info; this will be extended to carry
  the `const ResolvedScan*` so the link is exact. Parse-location byte ranges are
  the fallback.
- **Resolved AST ↔ SQLBuilder SQL**: the `SQLBuilder` (pipe mode) records the
  original `ResolvedScan` for each emitted `|>` in output order
  (`pipe_operator_scan_order()`); `SegmentPipeSqlToHtml` tags each output
  segment's `.rscan` box with that scan's id. Since both the AST and SQLBuilder
  panes use `.rscan[data-scan-id]`, correspondence is automatic. A scan that
  emits two `|>`s maps both segments to the same id.

Clicking selects the item (`.viz-selected`) and highlights the matching
`data-scan-id` boxes/markers in the other panes (`.viz-corresp`). Hidden panes
are simply not visible; the highlight classes are still applied so they show on
re-add.

### Known limitations of the segment approach

- Segments are split at every `|>` in the formatted text, regardless of paren
  nesting, so a nested subquery's operators are split at the top level rather
  than rendered as a nested block. The `data-scan-id` per operator is still
  correct (emission order matches textual `|>` order); only the visual grouping
  is flat.
- The leading source segment is always tagged scan-id 0 (the deepest source);
  for queries whose top-level FROM is a subquery this is approximate.
- The SQLBuilder pane no longer carries NameLists, so it has no click-details
  (only correspondence highlighting). Re-adding details would require
  re-analyzing the regenerated SQL (as an earlier iteration did).

## Status / TODOs

- [x] Milestone 1: `Visualize` tool mode; text output; web 3-pane scaffold.
- [x] Milestone 3 (UI mechanics): single-column layout, hide/re-add, draggable
      dividers + details resizer, linked state, click→details box.
- [x] Milestone 2: structured linear Resolved AST emitter
      (`ResolvedNode::DebugStringHtml`).
- [x] Milestone 4a: input SQL ↔ Resolved AST correspondence
      (`ResolvedScanInfo`/`TableScanInfo` carry `const ResolvedScan*`; input
      boxes tagged via `.ni-scan-id`; JS correspondence engine).
- [x] Milestone 4b: Resolved AST ↔ SQLBuilder SQL correspondence via passive
      emission-order instrumentation + `.rscan` segment divs.
- [ ] Resolved AST / SQLBuilder pane details content (deferred by design).
- [ ] Nested-subquery-aware segmentation in the SQLBuilder pane.
- [ ] Handle non-query statements and scripts (currently query-only).
- [ ] Revisit rewriter handling: visualization currently disables rewriters; a
      toggle may be wanted.
