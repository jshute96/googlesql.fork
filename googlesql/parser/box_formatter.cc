//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "googlesql/parser/box_formatter.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/parser/ast_node.h"
#include "googlesql/parser/parse_tree_format.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/parse_location.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {
namespace {

std::string EscapeHtml(absl::string_view text) {
  return absl::StrReplaceAll(text, {{"&", "&amp;"},
                                    {"<", "&lt;"},
                                    {">", "&gt;"},
                                    {"\"", "&quot;"}});
}

// Removes blank lines that contain only whitespace and HTML tags (artifacts of
// a comment forcing a line break that the surrounding layout also breaks),
// moving their tags onto the following line so div nesting stays balanced.
// Blank lines with no tags (e.g. inside a multi-line string literal or block
// comment) are preserved.
std::string CoalesceBlankLines(absl::string_view html) {
  std::vector<absl::string_view> lines = absl::StrSplit(html, '\n');
  std::vector<std::string> out;
  std::string carry;  // tags carried forward from removed blank lines
  for (size_t li = 0; li < lines.size(); ++li) {
    bool has_visible = false;
    bool has_tag = false;
    std::string tags;
    bool in_tag = false;
    for (char c : lines[li]) {
      if (c == '<') {
        in_tag = true;
        has_tag = true;
        tags += c;
      } else if (in_tag) {
        tags += c;
        if (c == '>') in_tag = false;
      } else if (!absl::ascii_isspace(static_cast<unsigned char>(c))) {
        has_visible = true;
      }
    }
    const bool is_last = (li + 1 == lines.size());
    if (!has_visible && has_tag && !is_last) {
      carry += tags;  // drop the blank line, keep its tags
    } else {
      out.push_back(absl::StrCat(carry, lines[li]));
      carry.clear();
    }
  }
  if (!carry.empty() && !out.empty()) out.back() += carry;
  return absl::StrJoin(out, "\n");
}

// ---------------------------------------------------------------------------
// Pretty-printing document IR (Wadler/Oppen style).
//
// A Doc is rendered either "flat" (single line) or "broken". A Group is
// rendered flat if it fits in the remaining columns, otherwise broken. Line
// nodes are a space when flat and a newline+indent when broken; a HardLine is
// always a newline (and forces every enclosing Group to break). Text carries
// its HTML rendering and its *visible* width (HTML tags have width 0 so the
// div wrappers don't affect layout).
// ---------------------------------------------------------------------------

struct Doc;
using DocPtr = std::shared_ptr<const Doc>;

enum class Kind {
  kNil,
  kText,
  kConcat,
  kLine,
  kNest,
  kGroup,
  kColorPush,  // open a background-colour region (html = CSS class)
  kColorPop,   // close the innermost colour region
};

struct Doc {
  Kind kind;
  std::string html;       // kText
  int width = 0;          // kText: visible width
  std::string flat_html;  // kLine: emitted when flat
  int flat_width = 0;     // kLine
  bool hard = false;      // kLine
  int nest = 0;           // kNest
  std::vector<DocPtr> children;
};

DocPtr Nil() {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kNil;
  return d;
}
DocPtr Text(std::string html, int width) {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kText;
  d->html = std::move(html);
  d->width = width;
  return d;
}
// Zero-width text: HTML markup (div tags) that must not affect layout.
DocPtr Tag(std::string html) { return Text(std::move(html), 0); }
// Visible original text, HTML-escaped.
DocPtr Esc(absl::string_view raw) {
  return Text(EscapeHtml(raw), static_cast<int>(raw.size()));
}
DocPtr Line(std::string flat = " ") {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kLine;
  d->flat_width = static_cast<int>(flat.size());
  d->flat_html = std::move(flat);
  return d;
}
DocPtr HardLine() {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kLine;
  d->hard = true;
  return d;
}
DocPtr Nest(int n, DocPtr child) {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kNest;
  d->nest = n;
  d->children = {std::move(child)};
  return d;
}
DocPtr Group(DocPtr child) {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kGroup;
  d->children = {std::move(child)};
  return d;
}
DocPtr Concat(std::vector<DocPtr> parts) {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kConcat;
  d->children = std::move(parts);
  return d;
}
// Opens a rectangular region (a single inline-block box) with CSS class
// "rect <cls>". Nested regions render as nested rectangles; the box shrink-
// wraps to its widest line and its contents are indented relative to it.
DocPtr ColorPush(absl::string_view cls) {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kColorPush;
  d->html = std::string(cls);
  return d;
}
DocPtr ColorPop() {
  auto d = std::make_shared<Doc>();
  d->kind = Kind::kColorPop;
  return d;
}
// Wraps `inner` in a colour region.
DocPtr Region(absl::string_view cls, DocPtr inner) {
  return Concat({ColorPush(cls), std::move(inner), ColorPop()});
}
// A hidden data carrier attached inside a region. The content is supplied as
// ready-made HTML and emitted verbatim (zero layout width, hidden via CSS); the
// client-side viewer script reads it to build the click-to-open info panel.
DocPtr NodeInfoBox(absl::string_view html) {
  return Concat({Tag("<div class=\"node-info\">"), Tag(std::string(html)),
                 Tag("</div>")});
}

constexpr int kInf = 1 << 29;

bool ContainsHard(const DocPtr& d) {
  switch (d->kind) {
    case Kind::kLine:
      return d->hard;
    case Kind::kText:
    case Kind::kNil:
      return false;
    default:
      for (const auto& c : d->children) {
        if (ContainsHard(c)) return true;
      }
      return false;
  }
}

// Flat (single-line) visible width, capped at kInf. A hard line is unflattenable
// so it makes the width effectively infinite.
int FlatWidth(const DocPtr& d) {
  switch (d->kind) {
    case Kind::kNil:
      return 0;
    case Kind::kText:
      return d->width;
    case Kind::kLine:
      return d->hard ? kInf : d->flat_width;
    default: {
      int sum = 0;
      for (const auto& c : d->children) {
        sum += FlatWidth(c);
        if (sum >= kInf) return kInf;
      }
      return sum;
    }
  }
}

class Renderer {
 public:
  explicit Renderer(int width) : width_(width) {}

  std::string Render(const DocPtr& doc) {
    out_.clear();
    col_ = 0;
    Emit(doc, 0, /*flat=*/false);
    return out_;
  }

 private:
  void Emit(const DocPtr& d, int indent, bool flat) {
    switch (d->kind) {
      case Kind::kNil:
        return;
      case Kind::kText:
        out_ += d->html;
        col_ += d->width;
        return;
      case Kind::kColorPush:
        // A region is a single inline-block box (see CSS .rect) that shrink-
        // wraps to its widest line. Its contents are indented relative to where
        // the box opens, so nested regions render as nested rectangles.
        out_ += absl::StrCat("<div class=\"rect ", d->html, "\">");
        region_base_.push_back(col_);
        return;
      case Kind::kColorPop:
        out_ += "</div>";
        if (!region_base_.empty()) region_base_.pop_back();
        return;
      case Kind::kLine:
        if (flat && !d->hard) {
          out_ += d->flat_html;
          col_ += d->flat_width;
        } else {
          Newline(indent);
        }
        return;
      case Kind::kNest:
        Emit(d->children[0], indent + d->nest, flat);
        return;
      case Kind::kConcat:
        for (const auto& c : d->children) Emit(c, indent, flat);
        return;
      case Kind::kGroup: {
        const DocPtr& body = d->children[0];
        const bool group_flat =
            !ContainsHard(body) && col_ + FlatWidth(body) <= width_;
        Emit(body, indent, group_flat);
        return;
      }
    }
  }

  // Emits a line break. Indentation is emitted relative to the innermost open
  // region's left edge (the column where it opened), because each region is an
  // inline-block box that provides its own left offset; emitting absolute
  // indentation would double-count it. `col_` still tracks the absolute column
  // for layout/fit decisions.
  void Newline(int indent) {
    out_ += '\n';
    const int base = region_base_.empty() ? 0 : region_base_.back();
    int rel = indent - base;
    if (rel < 0) rel = 0;
    out_.append(rel, ' ');
    col_ = indent;
  }

  int width_;
  std::string out_;
  int col_ = 0;
  // Absolute column where each currently-open region was opened.
  std::vector<int> region_base_;
};

// ---------------------------------------------------------------------------
// AST -> Doc.
// ---------------------------------------------------------------------------

struct Piece {
  const ASTNode* child = nullptr;  // null => literal gap [begin, end)
  int begin = 0;
  int end = 0;
};

bool StartsWith(absl::string_view s, absl::string_view prefix) {
  return absl::StartsWith(s, prefix);
}

// Default continuation indent (columns) for wrapped clause content, lists and
// parenthesized blocks. Pipe operators are the exception: their content lines
// up under the operator name instead (see BuildPipe).
constexpr int kDefaultIndent = 2;

// ===========================================================================
//                         FORMATTER CONFIGURATION
//
// How each AST node lays its children out as a box is declared *declaratively*
// on the AST nodes and fields themselves, via NodeFormat / FieldFormat
// attributes in parser/gen_parse_tree.py. The generator resolves attribute
// inheritance through the class hierarchy and emits a per-node-kind table
// (parse_tree_format_generated.cc); this formatter is a generic engine that
// consults it via GetASTNodeFormat().
//
// Most nodes declare nothing: the layout is inferred here from structure (a
// leading keyword + children => a clause; comma-separated children => a list;
// otherwise a flat flow). The ASTFormatLayout values, field roles, and their
// meanings are documented in parser/parse_tree_format.h.
// ===========================================================================

class Builder {
 public:
  Builder(absl::string_view sql, BoxAnnotator annotate)
      : sql_(sql), annotate_(std::move(annotate)) {}

  // ctx.flatten_query: whether a Query may be laid out inline (true inside a
  // subquery's parentheses) vs. always stacked (false at the top level).
  // ctx.clause_cont: extra indentation for a clause's wrapped content.
  struct Ctx {
    bool flatten_query = false;
    int clause_cont = kDefaultIndent;
    int subquery_depth = 0;  // nesting depth of enclosing subqueries
  };

  DocPtr Build(const ASTNode* node, Ctx ctx, DocPtr trailer = nullptr) {
    const std::string kind = node->GetNodeKindString();
    DocPtr inner = BuildInner(node, ctx);
    std::vector<DocPtr> parts;
    parts.push_back(Tag(absl::StrCat("<div class=\"ast ast-", kind, "\">")));
    parts.push_back(inner);
    if (trailer != nullptr) parts.push_back(trailer);
    parts.push_back(Tag("</div>"));
    DocPtr result = Concat(std::move(parts));
    // If this node's formatting parent designated it as a named colour region
    // (a field with a `region` attribute, e.g. a table reference's name path),
    // wrap it in that region here -- so the rule lives on the parent's field
    // declaration rather than in a per-node builder.
    const ASTNode* parent = node->parent();
    if (parent != nullptr) {
      const ASTNodeFormat& pf = GetASTNodeFormat(parent->node_kind());
      if (pf.region_child != nullptr && pf.region_child(*parent) == node) {
        return RegionAnnotated(pf.region_class, node, std::move(result));
      }
    }
    return result;
  }

 private:
  // Wraps `inner` in a colour region for `node`, attaching a hover box if the
  // annotator has HTML for that node.
  DocPtr RegionAnnotated(absl::string_view cls, const ASTNode* node,
                         DocPtr inner) {
    std::string hover = annotate_ ? annotate_(node) : "";
    if (hover.empty()) return Region(cls, std::move(inner));
    return Region(cls, Concat({std::move(inner), NodeInfoBox(hover)}));
  }

  std::vector<Piece> Pieces(const ASTNode* node) {
    const int ns = node->start_location().GetByteOffset();
    const int ne = node->end_location().GetByteOffset();
    std::vector<const ASTNode*> kids;
    for (int i = 0; i < node->num_children(); ++i) {
      const ASTNode* c = node->child(i);
      if (c == nullptr) continue;
      // Nodes marked skip (e.g. the pipe-JOIN LHS placeholder, whose source
      // range spans the whole join it stands in for) are never rendered by
      // their parent, as doing so would duplicate sibling text.
      if (GetASTNodeFormat(c->node_kind()).skip) continue;
      const int s = c->start_location().GetByteOffset();
      const int e = c->end_location().GetByteOffset();
      if (s < 0 || e <= s || s < ns || e > ne) continue;
      kids.push_back(c);
    }
    std::sort(kids.begin(), kids.end(), [](const ASTNode* a, const ASTNode* b) {
      return a->start_location().GetByteOffset() <
             b->start_location().GetByteOffset();
    });
    std::vector<Piece> out;
    int cursor = ns;
    for (const ASTNode* c : kids) {
      const int s = c->start_location().GetByteOffset();
      const int e = c->end_location().GetByteOffset();
      if (s < cursor) continue;  // overlap; skip
      if (s > cursor) out.push_back({nullptr, cursor, s});
      out.push_back({c, s, e});
      cursor = e;
    }
    if (cursor < ne) out.push_back({nullptr, cursor, ne});
    // Trim whitespace at the very start/end of the node; the parent provides
    // the separation around this node. This also drops pure-whitespace edge
    // gaps. (A leading keyword gap like "WHERE " keeps its trailing space; that
    // is handled where the keyword is extracted.)
    if (!out.empty() && out.front().child == nullptr) {
      while (out.front().begin < out.front().end &&
             absl::ascii_isspace(
                 static_cast<unsigned char>(sql_[out.front().begin]))) {
        ++out.front().begin;
      }
      if (out.front().begin >= out.front().end) out.erase(out.begin());
    }
    if (!out.empty() && out.back().child == nullptr) {
      while (out.back().end > out.back().begin &&
             absl::ascii_isspace(
                 static_cast<unsigned char>(sql_[out.back().end - 1]))) {
        --out.back().end;
      }
      if (out.back().begin >= out.back().end) out.pop_back();
    }
    return out;
  }

  bool HasAnyChild(const std::vector<Piece>& ps) const {
    for (const Piece& p : ps) {
      if (p.child != nullptr) return true;
    }
    return false;
  }

  absl::string_view GapText(const Piece& p) const {
    return sql_.substr(p.begin, p.end - p.begin);
  }

  // Collapses internal whitespace runs to single spaces, keeping (single)
  // leading/trailing spaces if the original had any whitespace there. This
  // preserves correct token spacing (e.g. " = ", ", ", ".", "(").
  std::string CollapseWs(absl::string_view g) const {
    std::string out;
    bool pending_space = false;
    for (char c : g) {
      if (absl::ascii_isspace(static_cast<unsigned char>(c))) {
        pending_space = true;
      } else {
        if (pending_space) out += ' ';  // keep leading/internal single space
        pending_space = false;
        out += c;
      }
    }
    if (pending_space) {
      if (out.empty()) return " ";  // pure whitespace -> single separator
      out += ' ';                   // keep trailing single space
    }
    return out;
  }

  DocPtr ClauseSep(Ctx ctx) {
    return ctx.flatten_query ? Line(" ") : HardLine();
  }

  bool GapHasComment(const Piece& p) const {
    absl::string_view g = GapText(p);
    return absl::StrContains(g, "--") || absl::StrContains(g, "/*") ||
           absl::StrContains(g, "#");
  }

  // Renders a literal gap as inline escaped text (with comments wrapped).
  DocPtr GapInline(const Piece& p) {
    if (!GapHasComment(p)) {
      std::string c = CollapseWs(GapText(p));
      return c.empty() ? Nil() : Esc(c);
    }
    return GapWithComments(p);
  }

  // Splits a gap into code / comment fragments, wrapping comments in their own
  // div. A line comment or a comment containing a newline forces a hard break
  // afterwards so the original line structure of comments is preserved.
  DocPtr GapWithComments(const Piece& p) {
    absl::string_view g = GapText(p);
    std::vector<DocPtr> parts;
    size_t i = 0;
    while (i < g.size()) {
      size_t dashes = g.find("--", i);
      size_t hash = g.find('#', i);
      size_t line = std::min(dashes, hash);
      size_t block = g.find("/*", i);
      size_t next = std::min(line, block);
      if (next == absl::string_view::npos) {
        std::string code = CollapseWs(g.substr(i));
        if (!code.empty() && code != " ") parts.push_back(Esc(code));
        break;
      }
      std::string code = CollapseWs(g.substr(i, next - i));
      if (!code.empty() && code != " ") parts.push_back(Esc(code));
      bool is_line = (next == line);
      size_t cend;
      if (is_line) {
        cend = g.find('\n', next);
        if (cend == absl::string_view::npos) cend = g.size();
      } else {
        cend = g.find("*/", next);
        cend = (cend == absl::string_view::npos) ? g.size() : cend + 2;
      }
      absl::string_view comment = g.substr(next, cend - next);
      parts.push_back(Tag("<div class=\"sql-comment\">"));
      parts.push_back(Esc(comment));
      parts.push_back(Tag("</div>"));
      if (is_line || absl::StrContains(comment, "\n")) {
        parts.push_back(HardLine());
      }
      i = cend;
    }
    return parts.empty() ? Nil() : Concat(std::move(parts));
  }

  // Renders only the comments in a gap, each as " <div sql-comment>...</div>",
  // with no surrounding code or line breaks. Used between stacked items (e.g.
  // query clauses / pipe operators) where the container already provides the
  // line break, so a trailing comment attaches to the current line instead of
  // introducing blank lines.
  DocPtr InlineComments(const Piece& p) {
    absl::string_view g = GapText(p);
    std::vector<DocPtr> parts;
    size_t i = 0;
    while (i < g.size()) {
      size_t dashes = g.find("--", i);
      size_t hash = g.find('#', i);
      size_t block = g.find("/*", i);
      size_t line = std::min(dashes, hash);
      size_t next = std::min(line, block);
      if (next == absl::string_view::npos) break;
      const bool is_line = (next == line);
      size_t cend;
      if (is_line) {
        cend = g.find('\n', next);
        if (cend == absl::string_view::npos) cend = g.size();
      } else {
        cend = g.find("*/", next);
        cend = (cend == absl::string_view::npos) ? g.size() : cend + 2;
      }
      parts.push_back(Esc(" "));
      parts.push_back(Tag("<div class=\"sql-comment\">"));
      parts.push_back(Esc(g.substr(next, cend - next)));
      parts.push_back(Tag("</div>"));
      i = cend;
    }
    return parts.empty() ? Nil() : Concat(std::move(parts));
  }

  // If the gap begins with a keyword -- a run of letters and internal single
  // spaces (e.g. "WHERE", "GROUP BY") that is separated from any following
  // content by whitespace, a comment, or the end of the gap -- returns true and
  // sets *kw_end to the byte offset just past the keyword. Returns false for
  // gaps where punctuation is glued to the letters, such as "STRUCT(" or "f(",
  // so those are not treated as clauses.
  bool LeadingKeyword(const Piece& p, int* kw_end) const {
    int i = p.begin;
    while (i < p.end && absl::ascii_isspace(static_cast<unsigned char>(sql_[i]))) {
      ++i;
    }
    if (i >= p.end || !absl::ascii_isalpha(static_cast<unsigned char>(sql_[i]))) {
      return false;
    }
    int last_letter = i;
    while (i < p.end) {
      const char ch = sql_[i];
      if (absl::ascii_isalpha(static_cast<unsigned char>(ch))) {
        last_letter = i;
        ++i;
        continue;
      }
      if (ch == ' ' || ch == '\t') {
        int j = i;
        while (j < p.end && (sql_[j] == ' ' || sql_[j] == '\t')) ++j;
        if (j < p.end &&
            absl::ascii_isalpha(static_cast<unsigned char>(sql_[j]))) {
          i = j;  // internal space between keyword words
          continue;
        }
        *kw_end = last_letter + 1;  // separated by whitespace
        return true;
      }
      if (ch == '\n' || ch == '\r') {
        *kw_end = last_letter + 1;
        return true;
      }
      if ((ch == '-' && i + 1 < p.end && sql_[i + 1] == '-') ||
          (ch == '/' && i + 1 < p.end && sql_[i + 1] == '*') || ch == '#') {
        *kw_end = last_letter + 1;  // separated by a comment
        return true;
      }
      return false;  // glued punctuation, e.g. "STRUCT("
    }
    *kw_end = last_letter + 1;  // keyword runs to the end of the gap
    return true;
  }

  bool IsKeywordGap(const Piece& p) const {
    int kw_end;
    return LeadingKeyword(p, &kw_end);
  }

  // True if the pieces from `first` onward are a pure comma-separated list:
  // they start and end with a child (no surrounding delimiters like the
  // brackets of an array constructor, which would be dropped by the list
  // builder) and the children are separated by commas.
  bool IsCommaList(const std::vector<Piece>& ps, size_t first_child) const {
    if (first_child >= ps.size() || ps[first_child].child == nullptr ||
        ps.back().child == nullptr) {
      return false;
    }
    int child_count = 0;
    int comma_seps = 0;
    bool prev_child = false;
    for (size_t i = first_child; i < ps.size(); ++i) {
      if (ps[i].child != nullptr) {
        ++child_count;
        prev_child = true;
      } else {
        absl::string_view c =
            absl::StripLeadingAsciiWhitespace(GapText(ps[i]));
        if (prev_child && StartsWith(c, ",")) ++comma_seps;
        prev_child = false;
      }
    }
    return child_count >= 2 && comma_seps >= child_count - 1;
  }

  // Builds the contents of a comma-separated list from pieces[first..end_child],
  // attaching each comma inside the preceding item's box.
  DocPtr BuildListBody(const std::vector<Piece>& ps, size_t first, Ctx ctx) {
    // Gather child indices.
    std::vector<size_t> child_idx;
    for (size_t i = first; i < ps.size(); ++i) {
      if (ps[i].child != nullptr) child_idx.push_back(i);
    }
    std::vector<DocPtr> parts;
    for (size_t k = 0; k < child_idx.size(); ++k) {
      const bool last = (k + 1 == child_idx.size());
      DocPtr trailer = last ? nullptr : Esc(",");
      parts.push_back(Build(ps[child_idx[k]].child, ctx, trailer));
      if (!last) parts.push_back(Line(" "));
    }
    return Group(Concat(std::move(parts)));
  }

  // Resolves the layout for `node`: its declared ASTFormatLayout (from the
  // generated format table) if any, otherwise inferred from structure. Never
  // returns kDefault (that is the "infer" sentinel, resolved here).
  ASTFormatLayout ResolveLayout(const ASTNode* node,
                                const std::vector<Piece>& ps) {
    ASTFormatLayout layout = GetASTNodeFormat(node->node_kind()).layout;
    if (layout != ASTFormatLayout::kDefault) return layout;
    // Structural inference. A leading keyword gap with child content is a
    // clause. (Requiring a child avoids misclassifying a leaf whose text
    // happens to start with a letter, e.g. an identifier.)
    if (!ps.empty() && ps[0].child == nullptr && IsKeywordGap(ps[0]) &&
        HasAnyChild(ps)) {
      return ASTFormatLayout::kClause;
    }
    if (IsCommaList(ps, 0)) return ASTFormatLayout::kList;
    return ASTFormatLayout::kFlow;
  }

  DocPtr BuildInner(const ASTNode* node, Ctx ctx) {
    DocPtr inner = BuildLayout(node, ctx);
    // A node with an `info_region` attribute (e.g. a function call or a
    // statement) gets its own clickable region carrying its info -- but only
    // when the annotator has info for it, so parse-mode rendering (no
    // annotator) is unaffected.
    const ASTNodeFormat& fmt = GetASTNodeFormat(node->node_kind());
    if (annotate_ && !fmt.info_region.empty()) {
      std::string info = annotate_(node);
      if (!info.empty()) {
        return Region(fmt.info_region,
                      Concat({std::move(inner), NodeInfoBox(info)}));
      }
    }
    return inner;
  }

  DocPtr BuildLayout(const ASTNode* node, Ctx ctx) {
    std::vector<Piece> ps = Pieces(node);
    switch (ResolveLayout(node, ps)) {
      case ASTFormatLayout::kStack:
        return BuildVertical(node, ps, ctx);
      case ASTFormatLayout::kKeywordStack:
        return BuildSelect(node, ps, ctx);
      case ASTFormatLayout::kParen:
        return BuildParen(node, ps, ctx);
      case ASTFormatLayout::kStatements:
        return BuildStatementList(ps, ctx);
      case ASTFormatLayout::kPipeOp:
        return BuildPipe(ps, ctx);
      case ASTFormatLayout::kClause:
        return BuildClause(ps, ctx);
      case ASTFormatLayout::kList:
        return BuildListBody(ps, 0, ctx);
      case ASTFormatLayout::kFlowWrap:
        return BuildFlow(ps, ctx, /*wrap=*/true);
      case ASTFormatLayout::kCall:
        return BuildCall(ps, ctx);
      case ASTFormatLayout::kSetChain:
        return BuildSetOp(node, ctx);
      case ASTFormatLayout::kCase:
        return BuildCase(node, ps, ctx);
      case ASTFormatLayout::kJoin:
        return BuildJoin(node, ps, ctx);
      case ASTFormatLayout::kFlow:
        return BuildFlow(ps, ctx, /*wrap=*/false);
      case ASTFormatLayout::kDefault:
      case ASTFormatLayout::kCustom:
        // kDefault is resolved away by ResolveLayout; kCustom has no builder
        // registered yet, so both fall back to a lossless inline flow.
        return BuildFlow(ps, ctx, /*wrap=*/false);
    }
    return BuildFlow(ps, ctx, /*wrap=*/false);
  }

  // Inline rendering: children + literal gaps. With `wrap`, an inter-child gap
  // that begins with whitespace (an operator such as " AND ") becomes a break
  // point -- a space when flat, a newline at the current indent when broken --
  // so a long operator chain breaks before each operator and the whole node is
  // grouped (all on one line, or each operator on its own line). Without `wrap`
  // there are no break points (except hard breaks from comments). A bare
  // parenthesized Query child (e.g. the subquery in `x IN (SELECT ...)`, whose
  // parens live in the surrounding gaps rather than a subquery node) is laid
  // out as a paren group so it stays inline if it fits and indents otherwise.
  DocPtr BuildFlow(const std::vector<Piece>& ps, Ctx ctx, bool wrap) {
    std::vector<DocPtr> parts;
    for (size_t i = 0; i < ps.size(); ++i) {
      const Piece& p = ps[i];
      if (p.child != nullptr) {
        if (IsParenthesizedQuery(ps, i)) {
          parts.push_back(BuildParenQuery(p.child, ctx));
        } else {
          parts.push_back(Build(p.child, ctx));
        }
      } else {
        const bool between_children =
            i > 0 && i + 1 < ps.size() && ps[i - 1].child != nullptr &&
            ps[i + 1].child != nullptr;
        const bool before_query = i + 1 < ps.size() &&
                                  IsParenthesizedQuery(ps, i + 1);
        const bool after_query = i > 0 && IsParenthesizedQuery(ps, i - 1);
        if (GapHasComment(p)) {
          parts.push_back(GapWithComments(p));
          continue;
        }
        std::string c = CollapseWs(GapText(p));
        // The "(" / ")" around a parenthesized Query are emitted by
        // BuildParenQuery, so strip them here to avoid duplication.
        if (before_query && !c.empty() && c.back() == '(') c.pop_back();
        if (after_query && !c.empty() && c.front() == ')') c.erase(c.begin());
        if (c.empty()) continue;
        if (wrap && between_children && c.front() == ' ') {
          // Break before the operator; keep the operator (and its trailing
          // space) attached to the following operand.
          parts.push_back(Line(" "));
          parts.push_back(Esc(c.substr(1)));
        } else {
          parts.push_back(Esc(c));
        }
      }
    }
    DocPtr body = Concat(std::move(parts));
    return wrap ? Group(body) : body;
  }

  // True if pieces[i] is a Query child whose parentheses live in the adjacent
  // gaps (e.g. the subquery in `x IN (SELECT ...)`), as opposed to a bare query
  // such as QueryStatement's child.
  bool IsParenthesizedQuery(const std::vector<Piece>& ps, size_t i) const {
    if (ps[i].child == nullptr ||
        ps[i].child->GetNodeKindString() != "Query") {
      return false;
    }
    if (i > 0 && ps[i - 1].child == nullptr) {
      std::string c = CollapseWs(GapText(ps[i - 1]));
      if (!c.empty() && c.back() == '(') return true;
    }
    if (i + 1 < ps.size() && ps[i + 1].child == nullptr) {
      std::string c = CollapseWs(GapText(ps[i + 1]));
      if (!c.empty() && c.front() == ')') return true;
    }
    return false;
  }

  // "(" + query + ")" as a single group: inline if it fits, else the query
  // indents on its own lines between the parentheses.
  // The colour class for a subquery at the given depth: blue and green
  // alternate by nesting depth so each level contrasts with its parent.
  const char* SubqueryColor(int depth) const {
    return (depth % 2 == 1) ? "subq-blue" : "subq-green";
  }

  DocPtr BuildParenQuery(const ASTNode* query, Ctx ctx) {
    Ctx inner = ctx;
    inner.flatten_query = true;
    inner.subquery_depth = ctx.subquery_depth + 1;
    // Only the query body is coloured; the parentheses and the leading
    // indentation stay the parent's colour.
    DocPtr body = RegionAnnotated(SubqueryColor(inner.subquery_depth), query,
                                  Build(query, inner));
    return Group(Concat({Esc("("), Nest(kDefaultIndent, Concat({Line(""), body})),
                         Line(""), Esc(")")}));
  }

  // keyword + content, where content stays on the keyword line if it fits,
  // else breaks onto indented lines.
  DocPtr BuildClause(const std::vector<Piece>& ps, Ctx ctx) {
    int kw_end = ps[0].end;
    LeadingKeyword(ps[0], &kw_end);
    std::string keyword = std::string(absl::StripAsciiWhitespace(
        CollapseWs(sql_.substr(ps[0].begin, kw_end - ps[0].begin))));
    // Content is the pieces after the keyword. If the keyword gap has leftover
    // text (e.g. a comment after SELECT), keep it as a leading content piece.
    std::vector<Piece> content_pieces(ps.begin() + 1, ps.end());
    if (HasNonWhitespace(kw_end, ps[0].end)) {
      content_pieces.insert(content_pieces.begin(),
                            Piece{nullptr, kw_end, ps[0].end});
    }
    // Join chain (FROM a JOIN b ON ...): keep the leading table on the keyword
    // line and align each "[LEFT] JOIN" under the keyword (no extra indent).
    if (content_pieces.size() == 1 && content_pieces[0].child != nullptr &&
        content_pieces[0].child->GetNodeKindString() == "Join" &&
        HasExplicitJoin(content_pieces[0].child)) {
      return Concat({Esc(keyword), Esc(" "),
                     Build(content_pieces[0].child, ctx)});
    }
    DocPtr content;
    if (IsCommaList(content_pieces, 0)) {
      content = BuildListBody(content_pieces, 0, ctx);
    } else {
      content = BuildInnerContent(content_pieces, 0, ctx);
    }
    return Concat({Esc(keyword),
                   Group(Nest(ctx.clause_cont, Concat({Line(" "), content})))});
  }

  bool HasNonWhitespace(int begin, int end) const {
    for (int i = begin; i < end; ++i) {
      if (!absl::ascii_isspace(static_cast<unsigned char>(sql_[i]))) return true;
    }
    return false;
  }

  // Builds a clause's content (the pieces after the keyword) using the layout
  // of the single content child when there is exactly one (so e.g. a WHERE
  // whose child is an AndExpr inherits the AND/OR wrapping), else a plain flow.
  DocPtr BuildInnerContent(const std::vector<Piece>& ps, size_t first,
                           Ctx ctx) {
    const ASTNode* only_child = nullptr;
    bool single = true;
    for (size_t i = first; i < ps.size(); ++i) {
      if (ps[i].child != nullptr) {
        if (only_child != nullptr) {
          single = false;
          break;
        }
        only_child = ps[i].child;
      }
    }
    if (single && only_child != nullptr) return Build(only_child, ctx);
    return BuildFlow(std::vector<Piece>(ps.begin() + first, ps.end()), ctx,
                     /*wrap=*/false);
  }

  // SELECT / AGGREGATE: the keyword and its select list go on the first line
  // (the list wraps when too long); every other clause child (FROM, WHERE,
  // GROUP BY, HAVING, ...) drops to its own line aligned with the keyword.
  DocPtr BuildSelect(const ASTNode* node, const std::vector<Piece>& ps,
                     Ctx ctx) {
    // The field with role 'head' stays on the keyword line; all others stack.
    const ASTNodeFormat& fmt = GetASTNodeFormat(node->node_kind());
    const ASTNode* head = fmt.head_child != nullptr ? fmt.head_child(*node)
                                                     : nullptr;
    std::string keyword;
    size_t first = 0;
    DocPtr keyword_rest = nullptr;  // e.g. a comment right after SELECT
    if (!ps.empty() && ps[0].child == nullptr && IsKeywordGap(ps[0])) {
      int kw_end = ps[0].end;
      LeadingKeyword(ps[0], &kw_end);
      keyword = std::string(absl::StripAsciiWhitespace(
          CollapseWs(sql_.substr(ps[0].begin, kw_end - ps[0].begin))));
      if (HasNonWhitespace(kw_end, ps[0].end)) {
        keyword_rest = GapWithComments(Piece{nullptr, kw_end, ps[0].end});
      }
      first = 1;
    }
    std::vector<const ASTNode*> kids;
    for (size_t i = first; i < ps.size(); ++i) {
      if (ps[i].child != nullptr) kids.push_back(ps[i].child);
    }
    std::vector<DocPtr> parts;
    if (!keyword.empty()) parts.push_back(Esc(keyword));
    if (keyword_rest != nullptr) {
      parts.push_back(Esc(" "));
      parts.push_back(keyword_rest);
    }
    bool attached_list = false;
    for (size_t k = 0; k < kids.size(); ++k) {
      if (k == 0 && kids[k] == head) {
        // Head field stays on the keyword line, wrapping under it when long.
        parts.push_back(Group(Nest(
            ctx.clause_cont, Concat({Line(" "), Build(kids[k], ctx)}))));
        attached_list = true;
      } else {
        if (k == 0 && !attached_list && !keyword.empty()) {
          parts.push_back(Group(Nest(
              ctx.clause_cont, Concat({Line(" "), Build(kids[k], ctx)}))));
        } else {
          parts.push_back(ClauseSep(ctx));
          parts.push_back(Build(kids[k], ctx));
        }
      }
    }
    return Concat(std::move(parts));
  }

  // A pipe operator: "|> " at the margin, the operator name on the same line,
  // continuation indented under the operator name.
  DocPtr BuildPipe(const std::vector<Piece>& ps, Ctx ctx) {
    std::vector<DocPtr> parts;
    Ctx inner_ctx = ctx;
    inner_ctx.clause_cont = 1;  // +3 (pipe) + 1 = 4 columns of content indent
    for (const Piece& p : ps) {
      if (p.child != nullptr) {
        parts.push_back(Nest(3, Build(p.child, inner_ctx)));
      } else {
        // The leading "|>" marker (and any stray punctuation/keywords). Always
        // put a space after the bare "|>" before the operator; for most
        // operators the gap's trailing space supplies it, but for `|> JOIN` the
        // following Join node's range swallows the space.
        std::string c = CollapseWs(GapText(p));
        if (c == "|>") c = "|> ";
        if (!c.empty()) parts.push_back(Esc(c));
      }
    }
    return Concat(std::move(parts));
  }

  // Parenthesized subquery: inline "(...)" if it fits, else the contents indent
  // on their own lines between the parentheses.
  DocPtr BuildParen(const ASTNode* node, const std::vector<Piece>& ps,
                    Ctx ctx) {
    Ctx inner_ctx = ctx;
    inner_ctx.flatten_query = true;
    inner_ctx.subquery_depth = ctx.subquery_depth + 1;
    // The body is the field with role 'paren_body'; any later children (e.g. a
    // table alias) follow the closing paren. Fall back to the first child if no
    // paren_body role is declared.
    const ASTNodeFormat& fmt = GetASTNodeFormat(node->node_kind());
    const ASTNode* body_node = fmt.paren_body_child != nullptr
                                   ? fmt.paren_body_child(*node)
                                   : nullptr;
    size_t body_idx = ps.size();
    for (size_t i = 0; i < ps.size(); ++i) {
      if (ps[i].child != nullptr &&
          (body_node == nullptr || ps[i].child == body_node)) {
        body_idx = i;
        break;
      }
    }
    if (body_idx == ps.size()) return BuildFlow(ps, ctx, /*wrap=*/false);

    // Only the query body is coloured; the parentheses / alias / indentation
    // stay the parent's colour.
    DocPtr body = RegionAnnotated(SubqueryColor(inner_ctx.subquery_depth),
                                  ps[body_idx].child,
                                  Build(ps[body_idx].child, inner_ctx));
    // Consume the closing-paren gap right after the body, if present.
    size_t after = body_idx + 1;
    if (after < ps.size() && ps[after].child == nullptr &&
        absl::StrContains(GapText(ps[after]), ")")) {
      ++after;
    }
    // Inline "(...)" if it fits; otherwise the contents indent on their own
    // lines between the parentheses.
    DocPtr paren = Group(Concat({Esc("("),
                                 Nest(kDefaultIndent, Concat({Line(""), body})),
                                 Line(""), Esc(")")}));
    // Trailing pieces (alias, etc.) rendered inline after the parentheses.
    std::vector<DocPtr> parts = {paren};
    for (size_t i = after; i < ps.size(); ++i) {
      if (ps[i].child != nullptr) {
        parts.push_back(Esc(" "));
        parts.push_back(Build(ps[i].child, ctx));
      } else {
        DocPtr g = GapInline(ps[i]);
        if (g->kind != Kind::kNil) {
          parts.push_back(Esc(" "));
          parts.push_back(g);
        }
      }
    }
    return parts.size() == 1 ? paren : Concat(std::move(parts));
  }

  // Stack children, each on its own line (a query's clauses / pipe operators).
  // At the top level (flatten_query == false) the stacking is unconditional;
  // inside a subquery it is governed by the enclosing paren group.
  DocPtr BuildVertical(const ASTNode* node, const std::vector<Piece>& ps,
                       Ctx ctx) {
    // A pipe query (FROM ... |> ... |> ...) colours each segment (the leading
    // FROM query and each pipe operator) as one solid alternating-tinted unit,
    // including its "|>" marker. A standard query has no pipe segments and is
    // coloured as a single block (see the wrapper below).
    bool has_pipe = false;
    for (const Piece& p : ps) {
      if (p.child != nullptr &&
          GetASTNodeFormat(p.child->node_kind()).layout ==
              ASTFormatLayout::kPipeOp) {
        has_pipe = true;
        break;
      }
    }
    // Segment tints are a lighter/darker pair of the enclosing query's colour
    // family, chosen by subquery depth (depth 0 = grey, then blue/green
    // alternating to match the subquery wrapper colours).
    const char* family = ctx.subquery_depth == 0  ? "grey"
                         : ctx.subquery_depth % 2  ? "blue"
                                                   : "green";
    std::vector<DocPtr> parts;
    bool first = true;
    int seg = 0;
    for (const Piece& p : ps) {
      if (p.child != nullptr) {
        if (!first) parts.push_back(ClauseSep(ctx));
        if (has_pipe) {
          // Alternate darker/lighter, starting with the darker shade ("-b").
          std::string cls =
              absl::StrCat("seg-", family, "-", (seg++ % 2 == 0) ? "b" : "a");
          parts.push_back(RegionAnnotated(cls, p.child, Build(p.child, ctx)));
        } else {
          parts.push_back(Build(p.child, ctx));
        }
        first = false;
      } else if (GapHasComment(p)) {
        parts.push_back(InlineComments(p));
      }
    }
    // No Group here: at the top level the clause separators (HardLine) always
    // break; inside a subquery the enclosing parenthesis group governs whether
    // the whole query is inline or broken (so a subquery is either fully on one
    // line or fully indented -- never half-broken).
    DocPtr body = Concat(std::move(parts));
    // The top-level query gets its own region (the parent of all its clauses /
    // pipe segments). A standard query is one solid grey block; a pipe query's
    // segments fill it, so it groups the whole pipe chain into one clickable
    // node ("Query with pipe operators"). Nested queries are wrapped by their
    // enclosing subquery instead (see BuildParen/BuildParenQuery).
    if (!ctx.flatten_query && ctx.subquery_depth == 0) {
      body = RegionAnnotated("q-whole", node, body);
    }
    return body;
  }

  // A function call: callee + a parenthesized argument list. If the arguments
  // are a clean comma list, they may break one-per-line between the
  // parentheses; otherwise the call is rendered inline (lossless, so modifiers
  // like DISTINCT are preserved).
  DocPtr BuildCall(const std::vector<Piece>& ps, Ctx ctx) {
    size_t open = ps.size(), close = ps.size();
    for (size_t i = 0; i < ps.size(); ++i) {
      if (ps[i].child == nullptr &&
          absl::StrContains(GapText(ps[i]), "(") && open == ps.size()) {
        open = i;
      }
      if (ps[i].child == nullptr && absl::StrContains(GapText(ps[i]), ")")) {
        close = i;
      }
    }
    if (open == ps.size() || close <= open) {
      return BuildFlow(ps, ctx, /*wrap=*/false);
    }
    std::vector<Piece> args(ps.begin() + open + 1, ps.begin() + close);
    if (!IsCommaList(args, 0)) return BuildFlow(ps, ctx, /*wrap=*/false);
    DocPtr callee = BuildFlow(std::vector<Piece>(ps.begin(), ps.begin() + open),
                              ctx, /*wrap=*/false);
    DocPtr arg_list = BuildListBody(args, 0, ctx);
    return Concat(
        {callee, Group(Concat({Esc("("),
                               Nest(kDefaultIndent, Concat({Line(""), arg_list})),
                               Line(""), Esc(")")}))});
  }

  // A set operation (UNION / INTERSECT / EXCEPT): the operands stack with the
  // operator on its own line between them. (The metadata-list child, marked
  // with role 'skip', overlaps the operand ranges, so the operands are taken
  // directly from the node and the operator text is read from the gaps between
  // them.)
  DocPtr BuildSetOp(const ASTNode* node, Ctx ctx) {
    const ASTNodeFormat& fmt = GetASTNodeFormat(node->node_kind());
    std::vector<const ASTNode*> operands;
    for (int i = 0; i < node->num_children(); ++i) {
      const ASTNode* c = node->child(i);
      if (c == nullptr) continue;
      if (fmt.is_skipped_child != nullptr && fmt.is_skipped_child(*node, *c)) {
        continue;
      }
      if (c->start_location().GetByteOffset() < 0) continue;
      operands.push_back(c);
    }
    std::sort(operands.begin(), operands.end(),
              [](const ASTNode* a, const ASTNode* b) {
                return a->start_location().GetByteOffset() <
                       b->start_location().GetByteOffset();
              });
    if (operands.size() < 2) return Build(node, ctx);  // unexpected; be safe
    std::vector<DocPtr> parts = {Build(operands[0], ctx)};
    for (size_t i = 1; i < operands.size(); ++i) {
      const int gb = operands[i - 1]->end_location().GetByteOffset();
      const int ge = operands[i]->start_location().GetByteOffset();
      std::string op;
      if (gb >= 0 && ge >= gb) {
        op = std::string(absl::StripAsciiWhitespace(
            CollapseWs(sql_.substr(gb, ge - gb))));
      }
      parts.push_back(ClauseSep(ctx));
      if (!op.empty()) {
        parts.push_back(Esc(op));
        parts.push_back(ClauseSep(ctx));
      }
      parts.push_back(Build(operands[i], ctx));
    }
    return Concat(std::move(parts));
  }

  // A keyworded arm construct (e.g. a searched CASE): each arm keyword (from
  // break_before_keywords, e.g. WHEN/ELSE) starts a new line indented under
  // the header keyword, and the footer keyword (e.g. END) dedents back to the
  // header column. Any other keyword gap (e.g. THEN) stays inline as a
  // connector. Stays on one line if short. All keywords come from the node's
  // format spec, so the same engine serves any construct of this shape.
  DocPtr BuildCase(const ASTNode* node, const std::vector<Piece>& ps, Ctx ctx) {
    const ASTNodeFormat& fmt = GetASTNodeFormat(node->node_kind());
    const absl::string_view header = fmt.header_keyword;
    const absl::string_view footer = fmt.footer_keyword;
    std::vector<DocPtr> body;
    for (size_t i = 0; i < ps.size(); ++i) {
      const Piece& p = ps[i];
      if (p.child != nullptr) {
        body.push_back(Build(p.child, ctx));
        continue;
      }
      std::string c =
          std::string(absl::StripAsciiWhitespace(CollapseWs(GapText(p))));
      if (i == 0 && !header.empty() && StartsWith(c, header)) {
        c = std::string(absl::StripAsciiWhitespace(c.substr(header.size())));
      }
      if (c.empty()) continue;
      if (!footer.empty() && (c == footer || StartsWith(c, absl::StrCat(footer, " ")))) {
        continue;  // footer is emitted by the wrapper below
      }
      bool is_arm = false;
      for (absl::string_view kw : fmt.break_before_keywords) {
        if (StartsWith(c, kw)) {
          body.push_back(Line(" "));
          body.push_back(Esc(absl::StrCat(c, " ")));
          is_arm = true;
          break;
        }
      }
      if (!is_arm) {
        // A connector keyword (e.g. THEN) stays inline with spaces on both
        // sides, since the surrounding operands render without them.
        body.push_back(Esc(absl::StrCat(" ", c, " ")));
      }
    }
    return Group(Concat({Esc(std::string(header)),
                         Nest(kDefaultIndent, Concat(body)), Line(" "),
                         Esc(std::string(footer))}));
  }

  // Recursively collects the table operands of a comma-only join (which is
  // left-nested: `a, b, c` parses as Join(Join(a, b), c)). Returns false if any
  // join operator is not a comma (i.e. there is an explicit JOIN).
  bool CollectCommaJoin(const ASTNode* node, std::vector<const ASTNode*>* out) {
    std::vector<const ASTNode*> kids;
    for (int i = 0; i < node->num_children(); ++i) {
      if (node->child(i) != nullptr) kids.push_back(node->child(i));
    }
    std::sort(kids.begin(), kids.end(), [](const ASTNode* a, const ASTNode* b) {
      return a->start_location().GetByteOffset() <
             b->start_location().GetByteOffset();
    });
    bool any_comma = false;
    for (const ASTNode* c : kids) {
      const std::string k = c->GetNodeKindString();
      if (k == "Location") {
        const int s = c->start_location().GetByteOffset();
        const int e = c->end_location().GetByteOffset();
        if (std::string(absl::StripAsciiWhitespace(sql_.substr(s, e - s))) !=
            ",") {
          return false;  // an explicit JOIN
        }
        any_comma = true;
      } else if (k == "Join") {
        if (!CollectCommaJoin(c, out)) return false;
      } else {
        out->push_back(c);
      }
    }
    return any_comma;
  }

  // True if the join (sub)tree contains an explicit JOIN (a join operator other
  // than a comma).
  bool HasExplicitJoin(const ASTNode* node) const {
    for (int i = 0; i < node->num_children(); ++i) {
      const ASTNode* c = node->child(i);
      if (c == nullptr) continue;
      const std::string k = c->GetNodeKindString();
      if (k == "Join") {
        if (HasExplicitJoin(c)) return true;
      } else if (k == "Location") {
        const int s = c->start_location().GetByteOffset();
        const int e = c->end_location().GetByteOffset();
        if (std::string(absl::StripAsciiWhitespace(sql_.substr(s, e - s))) !=
            ",") {
          return true;
        }
      }
    }
    return false;
  }

  // Flattens a (left-nested) join tree into its children in source order.
  void FlattenJoinItems(const ASTNode* node,
                        std::vector<const ASTNode*>* items) {
    std::vector<const ASTNode*> kids;
    for (int i = 0; i < node->num_children(); ++i) {
      if (node->child(i) != nullptr) kids.push_back(node->child(i));
    }
    std::sort(kids.begin(), kids.end(), [](const ASTNode* a, const ASTNode* b) {
      return a->start_location().GetByteOffset() <
             b->start_location().GetByteOffset();
    });
    for (const ASTNode* c : kids) {
      const std::string k = c->GetNodeKindString();
      // Skip nodes marked skip (e.g. the pipe-JOIN LHS placeholder, the
      // implicit pipe input, whose range spans the whole join -- emitting it
      // would duplicate the join text).
      if (GetASTNodeFormat(c->node_kind()).skip) continue;
      if (k == "Join") {
        FlattenJoinItems(c, items);
      } else {
        items->push_back(c);
      }
    }
  }

  // A join chain: each "[LEFT/...] JOIN <table> ON ..." starts a new line,
  // aligned with the leading table (which sits on the FROM keyword's line).
  DocPtr BuildJoinChain(const ASTNode* node, Ctx ctx) {
    std::vector<const ASTNode*> items;
    FlattenJoinItems(node, &items);
    std::vector<DocPtr> parts;
    bool need_space = false;
    bool first = true;
    for (const ASTNode* item : items) {
      if (item->GetNodeKindString() == "Location") {
        const int s = item->start_location().GetByteOffset();
        const int e = item->end_location().GetByteOffset();
        std::string op =
            std::string(absl::StripAsciiWhitespace(sql_.substr(s, e - s)));
        if (op == ",") {
          parts.push_back(Esc(","));  // a comma stays on the previous line
        } else {
          // A leading JOIN keyword (a pipe `|> JOIN`, where there is no prior
          // table on this line) stays inline; otherwise each JOIN starts a new
          // line aligned under the leading table.
          if (!first) parts.push_back(HardLine());
          parts.push_back(Esc(op));
        }
        need_space = true;
      } else {
        if (need_space) parts.push_back(Esc(" "));
        parts.push_back(Build(item, ctx));
        need_space = true;
      }
      first = false;
    }
    return Concat(std::move(parts));
  }

  // A FROM table expression: a plain comma-separated table list breaks one per
  // line when it doesn't fit; an explicit JOIN chain puts each JOIN on its own
  // line (see also BuildClause, which keeps the JOINs aligned under FROM).
  DocPtr BuildJoin(const ASTNode* node, const std::vector<Piece>& ps, Ctx ctx) {
    if (HasExplicitJoin(node)) return BuildJoinChain(node, ctx);
    std::vector<const ASTNode*> tables;
    if (!CollectCommaJoin(node, &tables) || tables.size() < 2) {
      return BuildFlow(ps, ctx, /*wrap=*/false);
    }
    std::vector<DocPtr> parts;
    for (size_t k = 0; k < tables.size(); ++k) {
      const bool last = (k + 1 == tables.size());
      parts.push_back(Build(tables[k], ctx, last ? nullptr : Esc(",")));
      if (!last) parts.push_back(Line(" "));
    }
    return Group(Concat(std::move(parts)));
  }

  // A statement list (scripts): one statement per line, ";" attached.
  DocPtr BuildStatementList(const std::vector<Piece>& ps, Ctx ctx) {
    std::vector<DocPtr> parts;
    bool first = true;
    for (size_t i = 0; i < ps.size(); ++i) {
      const Piece& p = ps[i];
      if (p.child != nullptr) {
        if (!first) parts.push_back(HardLine());
        parts.push_back(Build(p.child, ctx));
        first = false;
      } else {
        std::string c = CollapseWs(GapText(p));
        if (StartsWith(c, ";")) parts.push_back(Esc(";"));
        else if (GapHasComment(p)) parts.push_back(GapWithComments(p));
      }
    }
    return Concat(std::move(parts));
  }

  absl::string_view sql_;
  BoxAnnotator annotate_;
};

}  // namespace

absl::StatusOr<std::string> SqlToBoxHtml(absl::string_view sql,
                                         const ASTNode* root,
                                         const LanguageOptions& language_options,
                                         int width, BoxAnnotator annotate) {
  GOOGLESQL_RET_CHECK_NE(root, nullptr);
  const int root_start = root->start_location().GetByteOffset();
  const int root_end = root->end_location().GetByteOffset();
  GOOGLESQL_RET_CHECK_GE(root_start, 0);
  GOOGLESQL_RET_CHECK_LE(root_start, root_end);
  GOOGLESQL_RET_CHECK_LE(root_end, static_cast<int>(sql.size()));
  (void)language_options;  // Comments are detected directly from gap text.

  Builder builder(sql, std::move(annotate));
  Builder::Ctx ctx;
  ctx.flatten_query = false;
  DocPtr doc = builder.Build(root, ctx);

  Renderer renderer(width);
  std::string body = CoalesceBlankLines(renderer.Render(doc));
  return absl::StrCat("<div class=\"formatted-sql boxed\">", body, "</div>");
}

}  // namespace googlesql
