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
   block for the statement, and a `data-node-id` (`r<n>`) per scan box. Scalar
   fields render as escaped text. A non-scan child that *contains* a scan (e.g. a
   `WHERE` filter holding a scalar subquery) is **descended into**
   (`SubtreeContainsScan`, `resolved_node.cc`) so the subquery's scans become
   their own boxes with their own `r<n>` ids — rather than being buried in the
   expression's flat `DebugString` text — which is what lets the inner operators
   correspond across panes. Scan-free expressions are still rendered as compact
   text.
4. **SQLBuilder pane**: run `SQLBuilder` in `TargetSyntaxMode::kPipe` with
   `record_pipe_operator_markers = true`. The SQLBuilder stamps an inline
   `/*S<n>*/` comment marker at the **head of each emitted pipe operator's own
   text** (`MakeScanMarker`, `sql_builder.cc`); because the marker is part of the
   operator's text it rides through every `StrCat`/`Wrap`/paren-nesting to its
   exact final textual position. `pipe_operator_markers()` maps each marker index
   to the producing `ResolvedScan`. The tool then keeps **two strings**: the
   *marked* SQL (with markers) and the *display* SQL (markers removed by
   `StripScanMarkers`, then `LenientFormatSql`). `SegmentPipeSqlToHtml`
   **recursively** segments both — splitting at top-level `|>` *and* descending
   into nested pipe-subqueries (paren groups that contain a `|>`: a scalar
   subquery, a join RHS, a set-op input) — emitting one `.rscan` box per operator
   inside `.rscan-sub` blocks for the nesting. Each box owns **exactly** the scan
   of its first depth-0 marker (`data-corresp="r<n>"`), so the mapping is exact
   and 1:1 at every nesting level, not inferred by counting. Operators with no
   producing scan (notably the synthesized final output-renaming `|> SELECT`) are
   honestly left unlinked. Marker stripping is byte-equivalent to the normal
   output, so non-visualizer SQLBuilder behaviour is unchanged.
5. After the three (pre-rewrite) panes are built, run the configured rewriters
   on the tree; if they change it, include the **post-rewrite** Resolved AST as
   a separate read-only section (no cross-pane correspondence). Hand a
   `VisualizationData` to the writer. Text writers print the labeled sections;
   the web writer fills `viz_*_html` template params.

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
- **Resolved AST ↔ SQLBuilder SQL**: the mapping is **exact**, not inferred.
  `SegmentPipeSqlToHtml` recursively segments the regenerated pipe SQL into
  `.rscan` boxes (`data-node-id="s<n>"`, nested in `.rscan-sub`) and gives each
  box a `data-corresp="r<n>"` for the single scan that produced that operator —
  read from the inline `/*S<n>*/` marker the SQLBuilder stamped at the operator's
  head (above). Because the marker travels with the operator's own text to its
  exact final position, this works even for operators built out of order or
  nested inside a join RHS or scalar subquery — each maps to its **one** scan,
  not a range. Clicking a `JoinScan` highlights its `|> JOIN` box (and nothing
  else); clicking an inner subquery operator highlights its own box. The earlier
  positional/range scheme (counting `|>`s and mapping the whole FROM+JOIN block
  to a coarse range) is gone.

Clicking a node marks it `.viz-selected` (primary) and highlights its **direct**
`data-corresp` neighbours (one hop) in the *other* panes as `.viz-corresp`
(secondary). The walk is deliberately **not** transitive: chasing the edges to
their full connected component would merge every source `TableScan`/`JoinScan`
(all linked through the shared source-block SQLBuilder segments) plus all their
input tables into one "reflective" blob, so clicking one table lit up the whole
initial query everywhere. One hop keeps each click's highlight to its direct
correspondents. (The cost is that input ↔ SQLBuilder is no longer linked through
the Resolved AST in a single click; that two-hop path was the source of the
blob.) Hidden panes are not visible; the classes are still applied so they show
on re-add.

### Known limitations of the segment approach

The coarse FROM+JOIN *range* mapping and the lack of nested boxes are **gone** —
nested pipe operators are now broken out as their own `.rscan-sub` boxes and each
maps to exactly one scan. What remains:

- The display SQL is re-lexed with a **hand-rolled** quote/paren/escape scanner
  (`SplitTopLevelPipeSegments`, `FindPipeParenGroups`) to find top-level `|>` and
  nested pipe-subqueries. It can mis-split on lexer corners (comments,
  raw/triple-quoted strings, backtick-identifier escapes). A robust fix
  re-segments using the real tokenizer (`GetParseTokens`). *(The scan
  attribution itself is exact — it comes from the markers, not from this
  scanner.)*
- The SQLBuilder pane carries **no NameLists** of its own (that would require
  re-analyzing the regenerated SQL). Clicking a box shows the SQLBuilder pane's
  **own pipe-operator hierarchy** — the chain of enclosing operator boxes, each
  titled by its operator keyword line with its SQL text as the body
  (`collectSqlBuilderHierarchy` in `query_viewer.js`) — not the AST scan's
  NameList.
- A leaf table scan used as a join/subquery operand is rendered by the SQLBuilder
  as three boxes (`FROM t`, `|> SELECT <cols>`, `|> AS a`), all mapping to the
  same `r<n>`. The deferred "locality" refactor would collapse these to a single
  `FROM t AS a` box — see [pipe-sql-builder.md](pipe-sql-builder.md) §C.

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

## Node-mapping framework (`viz_node_mapping.{h,cc}`)

The id/cross-reference machinery above is factored into a small, pane-independent
module (`tools/execute_query/viz_node_mapping.{h,cc}`, branch
`visual-graph-new-mapping`) so the rule "every node has an id; each pane records
edges to ids" lives in one place and can be extended without each pane
re-deriving it. Three pieces:

- **`ResolvedNodeIds`** — the shared identity space. A dense id per node, typed
  over `const ResolvedNode*` (not `ResolvedScan*`), so non-scan nodes can be
  added to the mapping as coverage grows. The Resolved AST pane is the id
  authority (`AssignInOrder` over its emission-order `scan_order`); every other
  pane and the graph builder take a `const ResolvedNodeIds&` and `Lookup`.
- **`NodeRefMarkers`** — accumulates, per parse `ASTNode`, the **set** of resolved
  ids it corresponds to, and emits the hidden `.ni-ref` span (own id +
  cross-references). `Add` unions (so several markers resolving to one operator
  node no longer clobber each other — it was last-writer-wins before); `Inherit`
  lets a container layer (a query/subpipeline box) borrow its result operator's
  correspondences.
- **`RidTokens`** — formats an id set as the `data-corresp` token string.

**Many:many is now first-class.** `data-corresp` holds a *space-separated set*
of ids (`"r3 r7"`), not a single id, and the JS already splits on whitespace
(`collectNodes`/`buildAdj` in `query_viewer.js`). A single element can therefore
correspond to many resolved nodes and vice versa; the 1:1 case is just a
one-element set, so existing output (`data-corresp="r8"`) is unchanged. This
fixes the previously-dropped edges where one node carried several markers, and
generalizes the container-borrows-result-operator case to a set.

This is the first phase of the more robust framework. It deliberately
**preserves** the current behavior and DOM contract while replacing the ad-hoc
`flat_hash_map<const ResolvedScan*,int> scan_ids` + `vector<int> marker_to_rid`
plumbing with the explicit identity space + relation model. The id space is
still *populated* only from scans and still lives in pointer-keyed maps over the
single shared analysis (so it does not yet survive rewrites).

### Subquery/subpipeline container correspondence (q-namespace, implemented)

A subquery's *result scan* plays two roles: it is the chain's **last operator**
and it is the **whole subquery** (the field that holds it). Both used to share
the same id `r<n>`, so selecting a Subquery just highlighted its last operator
in the other panes. They are now separated into two namespaces over the same
dense id `n`:

- **`r<n>`** — the operator/scan view (the `.rscan` box).
- **`q<n>`** — the container view (the subquery/subpipeline as a whole).

The Resolved AST pane's `.rscan-query` wrapper is the **hub**: it carries
`data-node-id="q<n>"` (where `n` is the result scan's id, via `ScanChainTopId`).
The other panes link to it as `data-corresp="q<n>"`: the SQLBuilder query/
subpipeline layer via `NodeRefMarkers::InheritAsContainer`, and the input pane
via `ResolvedScanInfo::is_pipe_operator` — a scan-producing AST node that is *not*
a pipe operator is the container view, so it gets `AddContainer` (`q`) while
operators/leaf table scans get `Add` (`r`). The JS collects
`.rscan-query[data-node-id]` and `collectScanHierarchy` reads the wrapper's
`data-node-id`. Net: selecting a Subquery (the wrapper, or the "Subquery"
hierarchy step) highlights the *container* layer 1:1 across panes, distinct from
selecting its last operator.

### ProvenanceText: clean SQL + (byte-offset → node) spans (implemented)

The SQLBuilder now exposes its node→text mapping as a clean **provenance** API
instead of making the tool parse marker comments. `SQLBuilder::ProvenanceText`
holds `{sql, marked_sql, spans}` where `sql` is the clean (marker-free) SQL and
`spans` is a list of `{byte_offset, const ResolvedNode*}` in emission order;
`GetSqlWithProvenance()` produces it (gated on `record_pipe_operator_markers`).
The visualizer's primary `RenderSqlBuilderBoxHtml` re-parses `prov.sql` and, for
each span, assigns it to the smallest covering AST' node
(`FindSmallestSegmentNode`) → `node_ids.Lookup(span.node)` → `data-corresp`.
The tool no longer strips markers or threads a `marker_to_rid` vector through the
primary path (the text-fallback segmenter still segments `marked_sql` in place).

The marker storage is now typed over `const ResolvedNode*` (not `ResolvedScan*`),
so non-scan nodes can be marked too; all current call sites still pass scans.

Internally the byte positions are still carried by the inline `/*S<n>*/` markers
(they ride along through SQL assembly for free) and collapsed into spans at the
API boundary. This deliberately avoids rewriting SQLBuilder's core `std::string`
assembly into a true segmented rope — that path is shared by every SQLBuilder
caller (`UnanalyzeQuery`, the analyzer golden-test round-trips), so a blind core
rewrite is high-risk; the recording-off path stays byte-identical. A future
change could swap the inline carrier for a real rope without changing the
`ProvenanceText` contract or the visualizer.

### Roadmap: rewrite-stable ids

Two further tiers turn this into a fully general, rewrite-stable mapping. Not
implemented yet; scoped here as the next phases.

1. **Intrinsic, rewrite-stable node ids.** Rewriters are copy-based
   (`ResolvedASTRewriteVisitor`/`ResolvedASTDeepCopyVisitor`), so pointer
   identity never survives a rewrite. To keep ids stable across rewrites:
   - *Tier 1 (automatic, common case):* propagate the id at the copy visitor's
     join point (old node → new node) via a side-map carried across the copy —
     no field on the generated node classes, no proto-serialization change.
     Unchanged/moved/field-mutated nodes keep their id; genuinely new nodes get a
     fresh one.
   - *Tier 1.5 (free):* `column_id` (from `ColumnFactory`) already survives
     rewrites; use it for data-flow correspondence orthogonally to node ids.
   - *Tier 2 (opt-in):* a rewriter doing structural surgery (e.g. WITH-extraction)
     records explicit `new_id ← {old_ids}` provenance edges for the nodes it
     synthesizes.
   - *Tier 3 (fallback):* structural diff (`node_kind` + column-id signature +
     structural hash) proposes correspondences where Tiers 1–2 leave a gap.

2. **Resolved AST rewriter debugger.** With Tier-1 id propagation, the old and
   new trees share ids across the untouched majority, so a side-by-side diff
   keys on the id: same id + equal fields = unchanged; same id, different
   parent = moved; same id, different fields = mutated; id only in old/new =
   deleted/inserted. The visualizer's panes and the debugger's two trees are
   then the same operation (pick a node, traverse a relation) over different
   relations.

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
- [x] Resolved AST / SQLBuilder pane details content: clicking a Resolved AST
      box or SQLBuilder segment shows the scan's parent hierarchy + fields in the
      info box (SQLBuilder via its corresponding scan), entirely client-side.
- [x] **Exact, structural** Resolved AST ↔ SQLBuilder correspondence via inline
      `/*S<n>*/` markers (replacing the old positional `pipe_operator_scan_order`
      zip): the SQLBuilder stamps a marker at each operator's head
      (`record_pipe_operator_markers` / `MakeScanMarker` /
      `pipe_operator_markers()`), the tool keeps a *marked* and a marker-stripped
      *display* string, and `SegmentPipeSqlToHtml` attributes each operator to the
      single scan of its marker. Backed by a round-trip guard in
      `run_analyzer_test` (build with markers, strip, assert equal to unmarked) so
      markers can never perturb the generated SQL — see
      [pipe-sql-builder.md](pipe-sql-builder.md).
- [x] Nested pipe operators rendered as their own boxes: `SegmentPipeSqlToHtml`
      recurses into parenthesized pipe-subqueries (scalar subquery, join RHS,
      set-op input) and emits each operator as a nested `.rscan-sub` box owning
      its own scan — so an inner `|> EXTEND` in a `|> WHERE (subquery)`
      corresponds 1:1 across all three panes. The AST pane descends into
      expression subqueries to surface those scans as boxes
      (`SubtreeContainsScan`); the input pane links them via the resolver's
      per-node scan info.
- [x] SQLBuilder-pane details show the pane's **own** pipe-operator hierarchy
      (`collectSqlBuilderHierarchy`), and info-box ancestor headings are
      **clickable** to re-select that level in place (bold + body swap + cross-pane
      highlight, without rebuilding the stack); the statement-level box is also a
      selectable target.
- [x] Cleaner SQLBuilder-pane formatting: a nested subquery renders `|> WHERE (`
      with its body indented under it and `)` on its own line (the `.rscan-sub`
      box supplies indentation), and the box formatter takes a
      `break_pipe_operators` option so the input pane always uses the multi-line
      pipe form (`SqlToBoxHtml`, `box_formatter.cc`).
- [ ] Robust SQLBuilder-pane *segmentation*: the scan attribution is now exact
      (markers), but `SegmentPipeSqlToHtml` still finds the top-level `|>` and
      nested pipe-subqueries by re-lexing the formatted SQL with a hand-rolled
      quote/paren/escape scanner (`SplitTopLevelPipeSegments`,
      `FindPipeParenGroups`), which can mis-split on lexer corners (comments,
      raw/triple-quoted strings, backtick-identifier escapes). A robust fix
      re-segments using the real tokenizer (`GetParseTokens`). (Note:
      `LenientFormatSql` re-flows the text, so boundary offsets can't simply be
      recorded at emission time and reused — but the markers are part of the text
      and survive the reflow.)
- [ ] **Deferred: pipe SQLBuilder "locality" refactor.** Today a leaf table scan
      used as a join/subquery operand renders as three boxes
      (`FROM t` / `|> SELECT <cols>` / `|> AS a`), all → the same scan. The
      locality refactor would emit a single `FROM t AS a` (and prefer `JOIN ON`
      over `USING`), giving a much cleaner SQLBuilder pane. Validated for regular
      SQL but blocked on GQL graph joins + alias reservation — see
      [pipe-sql-builder.md](pipe-sql-builder.md) §C. The markers already attribute
      the collapsed `FROM t AS a` to its scan, so the visualizer needs no change.
- [x] Non-query statements: any statement is visualized like a query (the gate
      now skips only non-statement roots such as bare expressions in expression
      mode). Scripts get best-effort per-statement visualization — each
      top-level statement is framed as its own block and visualized against its
      OWN source text (re-parsed standalone from its byte range, so its panes
      show just that statement, not the whole script; statements that can't be
      isolated — e.g. they use a macro defined earlier in the script — fall back
      to the full-script text). Statements that don't analyze standalone (e.g.
      ones that reference script variables, or control-flow constructs) are
      skipped.
- [ ] Full-fidelity script visualization via script-executor integration
      (analyze each statement in its run-time variable context, rather than the
      current best-effort standalone analysis).
- [x] Post-rewrite Resolved AST section: after building the three (pre-rewrite)
      panes, the visualizer runs the configured rewriters on the tree and, when
      they change it, shows the post-rewrite Resolved AST as a separate
      read-only section below the panes (linear `.rscan` rendering, standalone).
      It carries no cross-pane correspondence — rewrites can transform the tree
      in ways disconnected from the input SQL. The three main panes stay
      pre-rewrite, so the input↔resolved correspondence is preserved. (We
      decided against an "apply rewriters" toggle that would have replaced the
      main panes and lost that correspondence.)
- [ ] Best-effort correspondence *through* the rewriter, so the post-rewrite
      Resolved AST (and a SQLBuilder regeneration of it) can map back to the
      input SQL. Complicated and potentially messy — rewrites can transform the
      mapping arbitrarily — so this is future work.

### Next phase — graphical tree / graph view (`visual-graph` branch, child PR)

Coarse breakdown; see the **"Next phase"** section above for full requirements.

**Structured model (JSON schema).** The graph view consumes an engine-neutral
`QueryGraph` (`tools/execute_query/query_graph.{h,cc}`): the *operator-mode*
model, from which query mode is a client-side collapse. Shape:

```jsonc
{
  "nodes":      [{"id":"r3","kind":"TableScan","container":"b1"}],
  "edges":      [{"from":"r0","to":"r1","kind":"pipe","label":""}],
  "containers": [{"id":"b0","kind":"QueryStmt","parent":""}]
}
```

Node `id`s are the same `r<n>` tags the textual/HTML panes emit for each scan
(so a selection in any pane maps 1:1 onto a graph node); container `id`s use a
distinct `b<n>` space. Edge `kind` is `"pipe"` (the linear pipe-input spine) or
`"input"` (a secondary input — join rhs, set-op input, subquery — with the
consuming field name in `label`); data flows `from`→`to`, drawn downward.

- [~] Full structured node/edge/containment **JSON model** + client-side
      rendering (builds on the ids + cross-refs landing now in the textual
      phase); SQLBuilder emits a structured tree instead of segmented text.
      *Done:* the `QueryGraph` schema + a Resolved-AST emitter
      (`BuildResolvedAstQueryGraph`, reusing the panes' scan-id order so ids
      align) + JSON serialization + unit tests; the JSON is embedded in the
      page inside the `.viz` block as
      `<script type="application/json" class="viz-graph-data" data-graph="ast">`
      and the client renders it (next item). *Pending:* the SQLBuilder
      structured tree, and expression-subquery / set-op coverage (the emitter
      currently walks direct child scans only, matching the linear panes).
- [~] Graph/tree view via elkjs geometry + our HTML nodes + SVG edge overlay
      (arrowheads, downward dataflow); query mode and operator mode. *Done:*
      hermetic **operator-mode** and **query-mode** renderers in
      `query_viewer.js` — HTML `.viz-gnode` boxes positioned by a small layered
      layout (rows = longest path from a source so data flows down; pipe spine
      in one column, secondary inputs branch right) with an SVG edge overlay
      (downward arrowheads; pipe edges solid, secondary inputs dashed). Each
      node shows the **same per-operator Resolved AST content the text pane
      shows** (head + scalar fields, cloned from the hidden `.rscan` boxes;
      nested child-scan boxes are omitted since they are separate graph
      nodes) — so the graph is the text view laid out and edge-connected, not
      just node titles. Nodes are sized to their content, so layout is a
      two-pass measure-then-place (each column as wide as its widest node, each
      row as tall as its tallest). Query mode is a client-side collapse of the
      operators sharing a container into one node per query box (which stacks
      that box's operators and corresponds to all the scans it aggregates).
      Clicking a node shows its scan hierarchy in the details box.
      *Pending / deferred:* **elkjs** for a constraint-based layout (vendoring
      it is a separate step — the restricted sandbox can't fetch it, so v1 is
      hermetic), and the richer join/set-op/CTE placement constraints. The
      hand-rolled layout is the "simpler fallback for a first cut" the library
      study calls out.
- [~] View-mode selector in column tabs (text / query graph / operator graph),
      linked across all panes in the window. *Done:* a `text` / `operator graph`
      / `query graph` `<select>` in the Resolved AST tab, linked via the shared
      `state.astView` across every `.viz` block. *Pending:* extending the
      selector to the other columns once they get tree views.
- [ ] Out-of-line subqueries with placeholders (`<table:…>`, `<cte:…>`,
      `<subquery:N>`, `<subpipeline:N>`); CTE reference edges and stacking.
- [~] Cross-view correspondence: N-operator dashed grouping box,
      placeholder ↔ target highlight, scroll-into-view with vertical alignment.
      *Done:* the correspondence engine is view-aware — graph nodes share the
      `r<n>` ids, and `collectNodes` highlights the *visible* element per id, so
      clicking a graph node highlights its corresponding input/SQLBuilder nodes
      (and vice-versa) in operator- and query-graph modes just as in text mode;
      and **scroll-into-view** now scrolls each other pane so its topmost
      corresponding node aligns vertically with the clicked node. *Pending:* the
      N-operator dashed grouping box and placeholder ↔ target highlight (the
      individual-highlight fallback is in place for both).
- [ ] SQL AST graph view (syntactic containment; split only FROM tables /
      subqueries and pipe operators).
- [ ] (If useful) tertiary "reflective" highlight (deferred; see Highlight
      states). Client-side rendering shift decided at graph-phase start.
