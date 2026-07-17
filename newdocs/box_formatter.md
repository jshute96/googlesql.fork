# The Box Formatter

`googlesql/parser/box_formatter.{h,cc}` renders a SQL statement as an HTML
"box" layout: it throws away the original whitespace and computes a fresh,
width-aware line layout, then decorates that layout with nested colored boxes
that mirror the parse-tree structure. It is used by the analyze-mode **query
viewer** in `execute_query --web` (see
[tooling-and-bindings.md](components/tooling-and-bindings.md)), where hovering
or clicking a box shows resolver information for that piece of the query.

The single entry point is:

```cpp
absl::StatusOr<std::string> SqlToBoxHtml(
    absl::string_view sql,          // the original text
    const ASTNode* root,           // a parse of `sql`
    const LanguageOptions& options,
    int width = 80,                // target line width
    BoxAnnotator annotate = nullptr);  // optional resolver-info callback
```

> **Not the production formatter.** This is experimental and separate from
> `tools/formatter/` (`public/sql_formatter.h`, `FormatSql`), which is the
> real, token-based SQL reformatter. The box formatter exists to *visualize*
> the AST, so it optimizes for showing structure, not for being a
> general-purpose pretty-printer.

The output is one `<div class="formatted-sql boxed">…</div>` containing text
(with real newlines and spaces) interleaved with `<div>` wrappers. It is meant
to be rendered with `white-space: pre`, so the computed spaces and line breaks
appear literally and the `<div>`s are pure inline decoration on top of them.

---

## The core idea: reconstruct from the source, don't re-serialize

The formatter **never turns the AST back into SQL**. It does not know how to
print an `ASTFunctionCall` or an `ASTBinaryExpression` as text. Instead, for
every node it takes that node's **source byte-range** `[start, end)` and splits
it into an alternating sequence of two kinds of piece:

- **child pieces** — the source spans covered by the node's child AST nodes,
  which are formatted recursively; and
- **gap pieces** — the literal source text *between* the children (keywords,
  operators, punctuation, whitespace, comments).

Everything that is actual SQL — identifiers, literals, keywords like `WHERE`,
operators like `=`, comments — is copied straight out of the original `sql`
string. The formatter's only freedom is **what whitespace to put between the
pieces** and **what decorative `<div>`s to wrap around them**. That is the
whole trick, and it is what keeps the content faithful: the formatter is
physically incapable of inventing a column name or dropping a predicate,
because it only ever re-emits substrings of the input.

```
Node source range:  SELECT a ,   b   FROM t
                    └──────────── one ASTSelect ───────────┘

Split into pieces (children = ●, gaps = ░░):

  ░░░░░░░ ● ░░░ ● ░░░░░░ ●
  SELECT  a  ,   b   FROM  t
  (gap)  (c) (g) (c) (gap) (c)

Each ● recurses; each ░ is re-emitted verbatim (whitespace collapsed).
```

---

## Splitting a node into pieces (`Pieces`)

`Pieces()` is where the source range is tiled. Given a node spanning
`[ns, ne)`:

1. **Collect children.** Walk `node->child(i)`, skipping nulls, skipping nodes
   whose `format` attribute marks them `skip` (see below), and skipping any
   child whose range is invalid or falls outside `[ns, ne)`.
2. **Sort children by source start offset.** *This is what makes AST field
   order irrelevant.* A node may declare its fields in any order, and some are
   genuinely out of source order relative to the text; sorting by byte offset
   puts them back into reading order before anything is emitted.
3. **Walk left to right**, keeping a `cursor` at `ns`. For each child starting
   at `s` and ending at `e`:
   - if `s > cursor`, emit a **gap** piece `[cursor, s)` (the text before this
     child);
   - emit the **child** piece `[s, e)`;
   - advance `cursor = e`.
   - if `s < cursor` (the child *overlaps* one already emitted), **drop it** —
     its text is already covered.
4. Emit a final trailing gap `[cursor, ne)` if any text remains.
5. **Trim** a leading/trailing gap that is only whitespace (the parent supplies
   separation around this node).

The important invariant this produces:

> The pieces **tile `[ns, ne)` with no holes and no overlaps** — every byte of
> the node's source is emitted exactly once, either as gap text or inside
> exactly one child. Recursing into each child preserves the same invariant all
> the way down.

That tiling invariant is the real "content preservation guarantee." It is
structural, not a semantic proof, but it is strong: no token can be silently
dropped (holes become gaps and are printed) and none can be duplicated
(overlapping children are skipped).

This is also how the formatter copes with parse trees that are not tidy:

| Awkward situation | How the tiling handles it |
|---|---|
| **Fields out of source order** | Children are sorted by start offset, so declaration order doesn't matter. |
| **Children don't cover all tokens** | Uncovered spans become gap pieces and are emitted verbatim — keywords like `FROM`/`WHERE` live only in gaps, never in a child. |
| **A child's range overlaps a sibling's** | The overlapping child is skipped; its text is emitted once, by whichever piece owns that range. |
| **A node's range deliberately spans its siblings** | It is marked `skip` and excluded (e.g. the set-operation metadata list, whose range covers the operands; or `PipeJoinLhsPlaceholder`, whose range spans the whole join it stands in for). |
| **Bad / negative offsets** | Filtered out defensively. |

A handful of layouts don't trust the piece list for structure and instead read
the node's children directly (sorted by offset) — set operations and joins do
this, because their operand ranges legitimately overlap the metadata the parser
attaches. Even then, the *text* they print still comes from source substrings.

---

## What is copied vs. what is synthesized

Almost everything is copied. The few synthesized strings are structural
punctuation that the special layouts re-emit rather than dig out of a gap:

- **Copied verbatim** (with internal whitespace collapsed to single spaces by
  `CollapseWs`, keeping token spacing like `" = "`): all gap text — keywords,
  operators, punctuation — every leaf's text, and all comments.
- **Synthesized** to match the source: the `,` between list items; the `(` and
  `)` around a subquery or a function-call argument list; the canonical
  `CASE`/`WHEN`/`THEN`/`ELSE`/`END` keywords in a `CASE`; the space after a bare
  `|>` marker. These are re-emitted (not derived from meaning) and mirror what
  the source already had — with one cosmetic exception: `CASE` keywords are
  normalized to upper case.

Everything is HTML-escaped on the way out. Anything the formatter doesn't
recognize falls back to an inline flow of its original text, which is lossless.
The header comments call this out as **best-effort**: the goal is faithful
*visualization*, so it does not promise byte-exact round-tripping the way a
dedicated formatter would.

---

## Computing the layout: a Wadler/Oppen pretty-printer

Piece-splitting says *what* to print; a small pretty-printer decides *where the
line breaks go*. The builder first turns the tree into a **document** (`Doc`)
made of layout primitives, then a `Renderer` walks that document to produce the
final string. This is the classic Wadler/Oppen design: a construct is printed
**flat** (on one line) if it fits in the remaining width, otherwise **broken**
across indented lines.

The `Doc` primitives:

| Primitive | Meaning |
|---|---|
| `Text(html, width)` | Literal output carrying its **visible** width. HTML tags are emitted as `Tag(...)` = zero width, so decoration never affects the fit math. |
| `Line(flat=" ")` | A break point: prints `flat` (usually a space) when its group is flat, or a newline + indent when broken. |
| `HardLine()` | Always a newline, and **forces every enclosing group to break**. |
| `Nest(n, doc)` | Adds `n` columns to the indent used by breaks inside `doc`. |
| `Group(doc)` | Prints `doc` flat if it fits, else broken. The unit of the flat/break decision. |
| `Concat`, `Nil` | Sequence; nothing. |
| `ColorPush(cls)` / `ColorPop()` | Open/close a colored box region (below). |

The fit decision, made when the renderer reaches a `Group`:

```
group is flat  ⇔  the group contains no HardLine
                  AND  current_column + FlatWidth(group) ≤ width
```

`FlatWidth` is the visible width the group would occupy on one line (a
`HardLine` makes it effectively infinite, which is how "this contains a
mandatory break" propagates outward).

```
  width = 24
  column ────────────────────────────►
  0                                  24
  │                                   │
  SELECT alpha, beta, gamma           │   FlatWidth = 25 > 24  → BREAK
  │                                   │
  ▼ broken:
  SELECT
    alpha,      ◄─ Line() became newline + Nest indent
    beta,
    gamma

  SELECT a, b                             FlatWidth = 11 ≤ 24  → FLAT
```

A `Group` is all-or-nothing: either the whole thing fits on one line or every
`Line` inside it breaks. That is why a subquery is either fully inline or fully
expanded, never half-broken.

### Indentation and the region stack

The renderer tracks the absolute output column (`col_`). When a break emits
indentation it does **not** use the absolute indent directly: each colored
region (below) is a CSS *inline-block box* that already provides its own left
offset, so the renderer indents **relative to the innermost open region's left
edge**. It keeps a stack of region base columns for exactly this. This is an
implementation detail of the coloring, invisible in the plain-text layout but
essential to make nested boxes line up.

---

## The HTML / CSS model

The rendered string is text plus two families of `<div>`:

- **Node wrappers** — every AST node becomes
  `<div class="ast ast-<NodeKind>">…</div>`. These are inline, zero-width
  decoration; they let the viewer attach behavior to any node.
- **Region boxes** — `<div class="rect <cls>">…</div>`. Styled `inline-block`
  (in `tools/execute_query/web/style.css`), each shrink-wraps to its widest
  line, so nested regions render as visibly nested rectangles.

The region classes carry the color scheme:

| Class | What it boxes |
|---|---|
| `q-whole` | A whole standard (non-pipe) query — one solid grey block. |
| `seg-<family>-a` / `-b` | Alternating tints for each segment of a pipe query (the leading `FROM` query and each `\|>` operator). |
| `subq-blue` / `subq-green` | A subquery **body**, alternating by nesting depth so each level contrasts with its parent. The parentheses stay the parent's color. |
| `table` | A table reference's name path, so table names are individually highlightable. |
| `func` / `stmt` | A clickable info region around a function call / statement, present only when the annotator supplies info for it. |

Hidden `<div class="node-info">…</div>` carriers hold the annotator's HTML
(zero layout width, hidden by CSS); the viewer's JavaScript reads them to build
the click-to-open info panel. The `BoxAnnotator annotate` callback is how the
analyzer's resolver info reaches the formatter without the parser layer
depending on analyzer types — the caller (which has resolved the query) hands
back ready-made HTML per node.

One post-processing pass, `CoalesceBlankLines`, cleans up blank lines that
contain only tags — an artifact of a comment forcing a break that the
surrounding layout also breaks — by carrying those tags onto the next line so
the `<div>` nesting stays balanced.

---

## The layout catalog

Which layout a node uses is **declared on the AST**, not hard-coded here: node
and field `format` attributes in `gen_parse_tree.py` are resolved through the
class hierarchy and emitted to a lookup table
(`parse_tree_format_generated.cc`), which the formatter consults via
`GetASTNodeFormat()`. The vocabulary and its inheritance rules are documented in
`googlesql/parser/parse_tree_format.h`. Most nodes declare nothing and are
**inferred**: a leading-keyword gap with child content → a clause;
comma-separated children → a list; otherwise a flat inline flow.

The layouts, with sketches of what each produces:

### `flow` — inline, no break points (the default)

Children and gaps run together on one line. Used for expressions, paths,
leaves. Lossless: unrecognized nodes land here.

```
a.b + f(x) = 1
```

### `clause` — keyword + content

A leading keyword; its content stays on the keyword line if it fits, else drops
to an indented line. Inferred whenever a node starts with a keyword gap.

```
WHERE x > 1                 WHERE
                              aaaa > 1 AND bbbb < 2   ◄─ content dropped & indented
```

### `list` — comma-separated items

Inline if they fit, else one per line — with each comma tucked **inside** the
preceding item's box (`alpha,` not `alpha ,`), so items read naturally.

```
alpha, beta, gamma          ┌ alpha, ┐
                            ├ beta,  ┤   each box holds its own trailing comma
                            └ gamma  ┘
```

### `wrap` — break before infix operators (`AND` / `OR`)

Like `flow`, but each operator gap is a break point, so a long boolean chain
breaks before every operator at the current indent.

```
aaaa = 1 AND bbbb = 2 AND cccc = 3
                                        ┌ aaaa = 1
      (too wide) ───────────────────►  ├ AND bbbb = 2
                                        └ AND cccc = 3
```

### `keyword_stack` — `SELECT`: head on the keyword line, rest stacked

The keyword and the field with role `head` (the select list) share the first
line; every other clause child (`FROM`, `WHERE`, `GROUP BY`, …) drops to its own
line aligned under the keyword.

```
┌ q-whole ─────────────────────┐
│ SELECT a, b                   │   ◄ head (select list) on the SELECT line
│ FROM t                        │
│ WHERE x > 1                   │
└───────────────────────────────┘
```

### `stack` — `Query`: children each on their own line

Clauses / pipe operators stack unconditionally at the top level (inside a
subquery, the enclosing parenthesis group decides inline-vs-broken). A pipe
query tints each segment:

```
┌ q-whole (grey) ──────┐      ┌ seg-grey-b ┐
│ SELECT a             │      │ FROM t     │
│ FROM t               │      ├ seg-grey-a ┤
│ WHERE x > 1          │      │ \|> WHERE x │
└──────────────────────┘      ├ seg-grey-b ┤
    standard query            │ \|> SELECT │
                              └────────────┘
                                pipe query
```

### `pipe` — a `|>` operator

`|>` sits at the left margin, the operator name follows on the same line, and
continuation indents under the name. Set once on the abstract `ASTPipeOperator`
and inherited by every pipe operator.

```
\|> WHERE
      aaaa > 1
      AND bbbb < 2
```

### `paren` — a parenthesized subquery

`(...)` inline if it fits; otherwise the body (the field with role `paren_body`)
indents on its own lines between the parentheses. Only the body is colored; the
parens keep the parent's color.

```
FROM (SELECT 1) AS s

FROM (                       ◄ body too wide → indented between the parens
       ┌ subq-blue ────┐
       │ SELECT y      │
       │ FROM uuuuuu   │
       └───────────────┘
     ) AS s
```

### `call` — function call

Callee + a parenthesized argument list that breaks one argument per line when
it doesn't fit. Falls back to inline (lossless) if the arguments aren't a clean
comma list, so modifiers like `DISTINCT` are preserved.

```
f(a, b, c)          f(
                      aaaaaa,
                      bbbbbb,
                      cccccc
                    )
```

### `set_chain` — `UNION` / `INTERSECT` / `EXCEPT`

Operands stack with the operator text (read verbatim from the gap between them)
on its own line. Operands are read straight from the node because the metadata
child's range overlaps them.

```
SELECT 1
UNION ALL
SELECT 2
UNION ALL
SELECT 3
```

### `join` — a `FROM` table list

The left-nested join tree is flattened into operands and operator tokens in
source order. Comma joins break one table per line; each explicit `JOIN` keyword
starts a new line aligned under the leading table.

```
FROM aa AS a
JOIN bb AS b ON a.id = b.id
LEFT JOIN cc AS c ON b.k = c.k
```

### `case` — a keyworded arm construct

Each arm keyword (`WHEN`, `ELSE`) starts a new indented line; the footer (`END`)
dedents back to the header (`CASE`) column; other keywords (`THEN`) stay inline
as connectors. All the keywords come from the node's format spec, so the same
engine serves any construct of this shape.

```
CASE
    WHEN aaaa > 1 THEN 1
    ELSE 2
  END
```

### `statements` — a statement list (scripts)

One statement per line, `;` attached.

---

## Comments

Comments are found textually inside gaps (`--`, `#`, `/* … */`). A line comment,
or any comment containing a newline, forces a `HardLine` after it so the
original line structure of the comment survives. Between stacked items (query
clauses, pipe operators) a trailing comment is attached to the *current* line
instead of introducing a blank line, so `\|> WHERE x -- note` keeps its note on
the same line rather than pushing a gap.

---

## Putting it together

```
SqlToBoxHtml(sql, root, …)
   │
   ▼  Builder.Build(node)                     for every node, recursively:
   │    ├─ Pieces(node)         ── tile [start,end) into children + gaps
   │    ├─ GetASTNodeFormat()   ── look up (or infer) the layout
   │    ├─ Build<Layout>(…)     ── emit a Doc (Text / Line / Group / Nest / Region)
   │    └─ wrap in <div ast-Kind> and any region box
   ▼
   Renderer.Render(doc)          ── flat/break decisions → text with newlines
   ▼
   CoalesceBlankLines(...)       ── tidy tag-only blank lines
   ▼
   <div class="formatted-sql boxed"> … </div>
```

**In one sentence:** the box formatter tiles each node's source range into
verbatim gap text and recursively-formatted children, decides line breaks with
a width-aware pretty-printer, and paints boxes over the result — so it only ever
rearranges whitespace and adds decoration on top of the original SQL's own
bytes.

---

### Where to look in the code

| You want… | Look at |
|---|---|
| Entry point & API | `box_formatter.h`, `SqlToBoxHtml` |
| Source-range tiling | `Builder::Pieces` |
| The pretty-printer primitives & renderer | `struct Doc`, `class Renderer` |
| A specific layout | `Builder::Build<Layout>` (e.g. `BuildSelect`, `BuildParen`, `BuildJoin`) |
| The declarative layout vocabulary | `parse_tree_format.h`; attributes in `gen_parse_tree.py` |
| Colors / box CSS | `tools/execute_query/web/style.css` |
| The caller (analyze-mode viewer) | `tools/execute_query/execute_query_tool.cc` |
