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
   block for the statement, and a `data-node-id` (`r<n>`) per scan box. Scalar fields and
   non-scan children render as escaped text in their box.
4. **SQLBuilder pane**: run `SQLBuilder` in `TargetSyntaxMode::kPipe` on the
   Resolved AST and lenient-format the result. The SQLBuilder records, as a
   passive side-channel (`pipe_operator_scan_order()`), the original
   `ResolvedScan` responsible for each emitted `|>` (in output order).
   `SegmentPipeSqlToHtml` then splits the formatted SQL at each `|>` and wraps
   each segment in a `.rscan` box — the *same* markup the Resolved AST pane uses
   — with its own `data-node-id` (`s<n>`) and a `data-corresp` cross-reference to
   that scan's `r<n>`. So the SQLBuilder pane lines up visually with the AST pane
   and the correspondence highlighting works across all three panes. The leading
   (FROM/source) segment is cross-referenced to the source scan `r0`.
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

## Correspondence model (ids + cross-references)

Every correspondence node carries a stable id in `data-node-id`, and a node that
*references* another carries a `data-corresp` list of the ids it links to
directly (one hop). Ids are namespaced per pane: `r<n>` for Resolved AST scan
boxes, `s<n>` for SQLBuilder segments, `a<n>` for input-pane nodes. The
`ResolvedScan` (a linear pipe operator) is still the natural hub, and ids are
computed from it via pointer-keyed maps over the single shared analysis.

- **Resolved AST pane**: each `.rscan` box has `data-node-id="r<n>"` (emission
  order; the deepest source scan is `r0`).
- **Input SQL ↔ Resolved AST**: PR #8's `ASTNodeResolvedInfoMap` gives each input
  AST node the `const ResolvedScan*` it produced; the input box gets a hidden
  `.ni-ref` marker with its own `data-node-id="a<n>"` and `data-corresp="r<n>"`.
- **Resolved AST ↔ SQLBuilder SQL**: the `SQLBuilder` (pipe mode) records the
  original `ResolvedScan` for each emitted `|>` in output order
  (`pipe_operator_scan_order()`); `SegmentPipeSqlToHtml` gives each segment box
  `data-node-id="s<n>"` and `data-corresp="r<n>"`. A scan that emits two `|>`s
  cross-references both segments to the same `r<n>`.

Clicking a node marks it `.viz-selected` (primary) and walks the `data-corresp`
edges (treated as undirected) to find the corresponding set; nodes reached in
*other* panes are marked `.viz-corresp` (secondary). Transitive links (input →
SQLBuilder, two hops through the Resolved AST) fall out of the walk. Nodes
reached back in the clicked node's *own* pane are the "reflective" (tertiary)
set and are deliberately left un-highlighted for now (see Highlight states).
Hidden panes are not visible; the classes are still applied so they show on
re-add.

### Known limitations of the segment approach

- Segments are split at every `|>` in the formatted text, regardless of paren
  nesting, so a nested subquery's operators are split at the top level rather
  than rendered as a nested block. The `data-corresp` per operator is still
  correct (emission order matches textual `|>` order); only the visual grouping
  is flat.
- The leading source segment is always cross-referenced to the source scan `r0`
  (the deepest source); for queries whose top-level FROM is a subquery this is
  approximate.
- The SQLBuilder pane no longer carries NameLists, so it has no click-details
  (only correspondence highlighting). Re-adding details would require
  re-analyzing the regenerated SQL (as an earlier iteration did).

### Structured ids + cross-references (implemented)

The id + cross-reference model in "Correspondence model" above is implemented,
replacing the original single-integer `data-scan-id` keying. Recap of the agreed
design now in place: direct one-hop cross-ref lists per node (intra- or
inter-tree); transitive links computed on the fly; ids assigned at emission time
via pointer-keyed maps keyed on the shared `ResolvedScan*`.

Still deferred (heavier, graph-phase): having the SQLBuilder build a true
**structured list/tree of output elements** instead of a flat string segmented
at `|>` (a `QueryExpression`-level refactor that would make the SQLBuilder pane
respect nesting), and shifting the panes to client-side rendering from the model.
Full path-hierarchy ids (e.g. `r.0.2.1`) and per-(non-scan)-node coverage also
land with that structured model; the current ids are flat-namespaced per pane.

### Where the ids live: emission-time, via pointer-keyed maps

**Decision:** ids are **hierarchical** and assigned during emission, carried in
the emitted output (data attributes today; a JSON model later). They are
computed via **external pointer-keyed maps** (`flat_hash_map<const ResolvedScan*
/ const ResolvedNode* / const ASTNode*, id>`) rather than fields on the C++
nodes — this works because the visualizer analyzes once and hands the *same*
Resolved AST to the HTML emitter and the SQLBuilder, so the pointers match, and
it keeps the id machinery out of the core IR. Storing the id *on* the node
(a `ResolvedNode`/`ASTNode` field, filled by a pass) only becomes worthwhile if
an id must persist *inside* the tree — e.g. the SQLBuilder emitting an inline id
reference during generation, or keeping ids stable across a rewriter pass that
reorders scans. Not needed yet (rewriters are disabled for visualization).

## Next phase: graphical tree / graph view (`visual-graph` branch)

The pipe-based *textual* presentation reads well for a **linear** operator
sequence, but not for trees or DAGs (joins, set ops, CTEs, expression
subqueries). The next phase draws the query as an actual node-link **graph**:
things that today appear as nested/inline subqueries become separate nodes
connected by edges. Rendering still follows the nested-box model we already
have, but some boxes move *out-of-line* and are connected with edges. This is a
large change and will live on a separate **`visual-graph`** branch as a child
PR. The requirements below are captured to steer current choices; they are not
yet finalized.

### Two view modes

- **Query mode** — each query / subquery is a node.
- **Operator mode** — each pipe operator / `ResolvedScan` is a node.

A view selector (drop-down) in each column's title bar / tab picks **text /
query graph / operator graph**. The setting applies to *all* visualize panes in
the window (it joins the shared linked `state`). Initially it applies to the
**Resolved AST** column only, until the SQL AST also gets a tree representation.

### Out-of-line placeholders

Where a field could have shown an inline subquery, show a short **placeholder**
that links to the out-of-line node, with a short identifier:
`<table:TableName>`, `<cte:CTEName>`, `<subquery:1>`, `<subpipeline:2>`.

### Layout constraints

- Data always flows **downward**; inputs are above outputs. Edges carry
  arrowheads, always pointing down.
- Secondary inputs/outputs go to the **right**, diagonally above or below.
- Operator chains render as **aligned vertical columns** beneath the initial
  leaf scan that starts the chain (e.g. a `ResolvedTableScan`).
- **Join**: the second (rhs) input comes from above-and-right; lhs and rhs are
  both part of the same query box.
- **TEE**: extra outputs to the right and below, horizontally adjacent.
- **FORK**: the N outputs spread horizontally below the FORK operator.
- **UNION / set ops**: N input queries horizontally adjacent (the inverse of
  FORK).
- **FROM joining several tables**: a chain of `JoinScan`s, each taking its pipe
  input from the vertical column above and its rhs input from up-and-right
  (a `TableScan` or a subquery).
- **WITH**: each CTE drawn as a labelled box; each reference draws an edge up to
  the CTE. CTEs lay out horizontally, but a CTE that depends on an earlier one
  is drawn *below* it (so its source edge comes from above). CTEs can sit inside
  a box representing the `WithScan`, inside a subquery.
- **Expression subqueries**: inline boxes inside the `ProjectScan` (or other
  scan) that holds the expression.

### SQL AST graph view

Same layout rules, but representing **pure syntactic containment** — ASTs have
no resolved back-pointers (e.g. no CTE reference edges); everything is strict
SQL containment, just moving subqueries/subpipelines out-of-line for clearer
tree flow.

- Inside standard-syntax `SELECT` blocks, split out **only** table names /
  subqueries in `FROM` — nothing else.
- Operator-tree mode only splits operators out inside a query when they are
  **pipe** operators.
- Set-op / FORK subqueries draw above/below the operator node, edge-connected;
  the inline form is replaced with a `<name>` placeholder + id.

### Interaction

- Clicking finds the **innermost** node (same hit-testing as AST boxes today).
- The details box shows the **parent-box hierarchy** of the clicked node, each
  entry clickable to select the outer node (as the AST hierarchy does today).
- **Cross-column correspondence** is preserved across all modes (text ↔ query
  graph ↔ operator graph). When operators are not 1:1 and a *sequence* of N
  corresponding operators must be highlighted, wrap them in a **temporary
  dashed box**. Fallback if that proves hard (or the set is non-contiguous /
  not in one hierarchy — not expected): highlight the N nodes individually.
- **Placeholder ↔ target**: clicking a placeholder highlights it (primary) and
  highlights the referenced node as corresponding; clicking the referenced node
  highlights it (primary) and any placeholders pointing at it as corresponding.
  Either way the info pane shows details for the **actual** node (as if you had
  clicked the inlined subquery/operator), never the reference itself.
- **Scroll-into-view**: when corresponding nodes highlight in other columns,
  scroll those columns so the main corresponding node is visible, and try to
  vertically **align** it with the primary node that was clicked.

### Highlight states

1. **Primary selection** — the single clicked item, highlighted prominently.
2. **Secondary / corresponding** — one or more items corresponding to the
   primary, in other columns or the same column (e.g. SQL reference/content
   highlights).
3. **Tertiary "reflective"** — *noted but not initially built.* When the primary
   X maps to a corresponding set {Y₁,Y₂} in another column, those Y's reflect
   back to a set {X₁,X₂,X₃} in X's own column that is *larger* than X. The extra
   reflected nodes could be shown with a fainter tertiary highlight. Deferred
   because (a) its value is unclear and (b) the reflection can recurse —
   expanding back and forth across columns over multiple hops — so its scope
   needs bounding. The on-the-fly transitive walk (above) is the same machinery,
   so this is cheap to add later if it proves useful.

### Rendering approach — library study

The hard part is **layout geometry** (placing nodes per the constraints above
and routing edges), not rendering or interaction — we already have HTML box
markup plus click/highlight/correspondence JS that we want to keep. So the
recommended architecture **decouples layout from DOM**:

> **Use a layout engine only to compute node `x/y/size` and edge bend points;
> keep rendering our own HTML nodes (reuse the existing nested-box markup) and
> draw edges as an SVG overlay with downward arrowheads.** Our existing
> click/selection/correspondence code then works unchanged, and we avoid
> adopting a UI framework.

Engine options studied (all run client-side, no server dependency, fit the
offline static-asset web tool):

| Library | Layout strength for our constraints | Node rendering | Verdict |
|---|---|---|---|
| **elkjs** (Eclipse Layout Kernel) | **Best fit.** `layered` algorithm with **ports** (sides N/S/E/W + `FIXED_ORDER`), **layer/partition constraints**, orthogonal edge routing that respects port constraints — directly expresses "data down", "secondary input on the east/diagonal", "aligned operator columns", "set-op inputs side-by-side". Runs in a web worker. | We render HTML; ELK gives geometry only. | **Recommended.** |
| **@dagrejs/dagre** | Maintained (v3). Simple layered TB layout, but far fewer constraint knobs (no real port-side / partition control), so the exact diagonal/aligned look is harder. | We render HTML; geometry only. | Simpler fallback for a first cut. |
| **d3-dag** (Erik Brinkman) | Maintained. Sugiyama/Zherebko/grid; handles true DAGs (CTE back-edges) well. Fewer high-level port/side knobs than ELK — more hand-rolling. | We render (SVG/HTML); geometry only. | Pure-D3 alternative. |
| **React Flow / xyflow** | Best DX for custom HTML nodes + handles/ports + selection/click state; but does **not** lay out — you feed it elk/dagre positions. | First-class HTML nodes. | Great if we adopt React — but that's a framework rewrite of a vanilla-JS + mstch tool. Out of scope unless we decide to. |
| **Cytoscape.js / AntV G6** | Rich graph interaction + state APIs; layout via dagre/elk extensions. | Canvas-first; nested clickable HTML box content only via extensions and awkwardly. | Strong for big network graphs, poor fit for our HTML nested-box nodes. |
| **Graphviz wasm** (`@hpcc-js/wasm`) | `dot` engine gives excellent layered layout; **clusters** map naturally to "query box" grouping; ports + arrowheads; SVG output. | Layout **and** render are a black box; HTML-like labels can't hold our interactive nested boxes; retrofitting click-to-select on sub-elements + dynamic re-highlight means parsing/patching the generated SVG. | Great for *static* diagrams; awkward for our interactivity. |

**Decided:** **elkjs** is the chosen layout engine (geometry only) + our own
HTML nodes + an SVG edge overlay. Keep dagre in mind as a simpler fallback if
ELK's constraint config proves too fiddly for v1. Re-evaluate React Flow only if
we ever decide to move the web tool to a component framework.

References: [elkjs](https://github.com/kieler/elkjs) ·
[ELK Layered](https://eclipse.dev/elk/reference/algorithms/org-eclipse-elk-layered.html) ·
[@dagrejs/dagre](https://github.com/dagrejs/dagre) ·
[d3-dag](https://github.com/erikbrinkman/d3-dag) ·
[React Flow](https://reactflow.dev/) ·
[Cytoscape.js](https://js.cytoscape.org/) ·
[@hpcc-js/wasm](https://github.com/hpcc-systems/hpcc-js-wasm).

### How this steers the *current* (textual) phase

These were resolved with the maintainer and are being **built now** (so we don't
refactor twice):

1. **Stable hierarchical ids + cross-ref lists** — *decided, now.* See
   "Evolution: structured ids + cross-reference lists" above. This is the shared
   substrate the graph view will also consume.
2. **Ids computed via pointer-keyed maps at emission time** — *decided.* See
   "Where the ids live" above (not stored on the C++ nodes yet).
3. **Correspondence is a set, not a single id** — *decided, now.* A selection
   maps to one **primary** id plus a **set** of corresponding ids (a query-mode
   node aggregates several scans; an N-operator highlight spans several boxes).
   The JS engine is generalized to "primary + corresponding set" so the graph
   phase is a drop-in.

Still TBD: shifting the panes to **client-side** rendering from a JSON model
(graph-phase); the **tertiary "reflective"** highlight (see Highlight states —
noted but not initially built).

## Status / TODOs

- [x] Milestone 1: `Visualize` tool mode; text output; web 3-pane scaffold.
- [x] Milestone 3 (UI mechanics): single-column layout, hide/re-add, draggable
      dividers + details resizer, linked state, click→details box.
- [x] Milestone 2: structured linear Resolved AST emitter
      (`ResolvedNode::DebugStringHtml`).
- [x] Milestone 4a: input SQL ↔ Resolved AST correspondence
      (`ResolvedScanInfo`/`TableScanInfo` carry `const ResolvedScan*`; input
      boxes tagged via `.ni-ref` markers; JS correspondence engine).
- [x] Milestone 4b: Resolved AST ↔ SQLBuilder SQL correspondence via passive
      emission-order instrumentation + `.rscan` segment divs.
- [x] Milestone 5: stable per-pane ids (`r<n>`/`s<n>`/`a<n>`) on Resolved AST
      scan boxes, SQLBuilder segments, and input markers, via pointer-keyed maps,
      emitted as `data-node-id`. (Full path-hierarchy ids and per-non-scan-node
      coverage are deferred to the structured JSON model.)
- [x] Milestone 5: direct cross-reference lists (`data-corresp`) linking input /
      SQLBuilder nodes to the scan they correspond to; transitive links resolved
      on the fly in JS.
- [x] Milestone 5: JS correspondence engine generalized to "primary +
      corresponding set" via an undirected `data-corresp` walk; the same-pane
      "reflective" set is deferred (tertiary highlight).
- [ ] Resolved AST / SQLBuilder pane details content (deferred by design).
- [ ] Nested-subquery-aware segmentation in the SQLBuilder pane.
- [ ] Handle non-query statements and scripts (currently query-only).
- [ ] Revisit rewriter handling: visualization currently disables rewriters; a
      toggle may be wanted.

### Next phase — graphical tree / graph view (`visual-graph` branch, child PR)

Coarse breakdown; see the **"Next phase"** section above for full requirements.

- [ ] Full structured node/edge/containment **JSON model** + client-side
      rendering (builds on the ids + cross-refs landing now in the textual
      phase); SQLBuilder emits a structured tree instead of segmented text.
- [ ] Graph/tree view via **elkjs** geometry + our HTML nodes + SVG edge
      overlay (arrowheads, downward dataflow); query mode and operator mode.
- [ ] View-mode selector in column tabs (text / query graph / operator graph),
      linked across all panes in the window.
- [ ] Out-of-line subqueries with placeholders (`<table:…>`, `<cte:…>`,
      `<subquery:N>`, `<subpipeline:N>`); CTE reference edges and stacking.
- [ ] Cross-view correspondence: N-operator dashed grouping box,
      placeholder ↔ target highlight, scroll-into-view with vertical alignment.
- [ ] SQL AST graph view (syntactic containment; split only FROM tables /
      subqueries and pipe operators).
- [ ] (If useful) tertiary "reflective" highlight (deferred; see Highlight
      states). Client-side rendering shift decided at graph-phase start.
