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
3. **Resolved AST pane**: `ResolvedNode::DebugString` with `linear_mode = true`.
   *(Currently rendered as escaped `<pre>` — a structured per-scan emitter is
   pending; see TODOs.)*
4. **SQLBuilder pane**: run `SQLBuilder` in `TargetSyntaxMode::kPipe` on the
   Resolved AST, lenient-format the result, then **re-parse and re-analyze** it
   so its NameLists are available, and render it with the same box formatter.
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
- **Resolved AST ↔ SQLBuilder SQL**: the `SQLBuilder` will be instrumented (pipe
  mode only) to record the output byte range each `ResolvedScan` produces (each
  scan → 1–2 pipe operators). Non-pipe-input scan fields mark query boundaries
  (→ subqueries); a trailing final-column-list `ProjectScan` belongs to the
  enclosing query.
- **SQLBuilder SQL ↔ its NameLists**: via re-analysis (already done) — used for
  the details box, not for correspondence.

Clicking selects the item and highlights corresponding ranges in the other
visible panes; when there is no 1:1 match we fall back to the nearest enclosing
query block. Hidden panes are skipped (except transitively through the AST).

## Status / TODOs

- [x] Milestone 1: `Visualize` tool mode; text output; web 3-pane scaffold.
- [x] Milestone 3 (UI mechanics): single-column layout, hide/re-add, draggable
      dividers + details resizer, linked state, click→details box.
- [ ] Milestone 2: structured linear Resolved AST emitter (one `.rscan` box per
      `ResolvedScan`, alternating colors, query/statement nesting,
      `data-scan-id`) replacing the `<pre>` placeholder.
- [ ] Milestone 4: correspondence data + cross-pane highlighting.
  - [ ] Extend `ResolvedScanInfo`/`TableScanInfo` with `const ResolvedScan*`.
  - [ ] Instrument `SQLBuilder` (pipe mode) to emit per-scan output ranges.
  - [ ] JS correspondence engine over a render-time scan-id map.
- [ ] Resolved AST pane details/info content (deferred by design).
- [ ] Handle non-query statements and scripts (currently query-only).
- [ ] Revisit rewriter handling: visualization currently disables rewriters; a
      toggle may be wanted.
