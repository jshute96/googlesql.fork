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
#include "googlesql/public/language_options.h"
#include "googlesql/public/parse_location.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
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

enum class Kind { kNil, kText, kConcat, kLine, kNest, kGroup };

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
      case Kind::kLine:
        if (flat && !d->hard) {
          out_ += d->flat_html;
          col_ += d->flat_width;
        } else {
          out_ += '\n';
          out_.append(indent, ' ');
          col_ = indent;
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

  int width_;
  std::string out_;
  int col_ = 0;
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
// How each AST node lays its children out as a box. This is the declarative
// part of the formatter: the framework below is generic, and a node's layout
// is just a choice from this small vocabulary. Most nodes need no entry -- the
// layout is inferred from structure (a leading keyword + children => a clause;
// comma-separated children => a list; otherwise a flat flow). Add an entry here
// only to override that inference or to select a special layout.
//
// The layouts:
//   kFlow      Children/keywords flow inline with no break points. Default for
//              expressions, function calls, paths, leaves.
//   kFlowWrap  Like kFlow, but may break before each operator and continue on
//              the next line at the same indent. Used for AND/OR chains.
//   kList      Comma-separated items: inline if they fit, else one per line,
//              each comma tucked inside its item's box.
//   kClause    A leading keyword followed by content (the content uses its own
//              layout); content drops to an indented line when it doesn't fit.
//   kVertical  Children each start a new line (a query's clause / pipe-op list).
//   kSelect    SELECT/AGGREGATE: keyword + select list on one line, remaining
//              clauses stacked, GROUP BY forced to its own aligned line.
//   kParen     A parenthesized subquery/expression: inline "(...)" if it fits,
//              else its contents indent between the parentheses.
//   kPipeOp    A "|>" pipe operator.
//   kStatements  A statement list (scripts): one statement per line.
// ===========================================================================
enum class Layout {
  kFlow,
  kFlowWrap,
  kList,
  kClause,
  kVertical,
  kSelect,
  kParen,
  kPipeOp,
  kStatements,
};

// Per-node-kind layout overrides. Node kinds are GetNodeKindString() values
// (the AST class name without the "AST" prefix). Pipe operators (kind starting
// with "Pipe") are mapped to kPipeOp separately, by prefix.
const absl::flat_hash_map<absl::string_view, Layout>& LayoutConfig() {
  static const auto* const config =
      new absl::flat_hash_map<absl::string_view, Layout>{
          {"Query", Layout::kVertical},
          {"Select", Layout::kSelect},
          {"ExpressionSubquery", Layout::kParen},
          {"TableSubquery", Layout::kParen},
          {"StatementList", Layout::kStatements},
          {"AndExpr", Layout::kFlowWrap},
          {"OrExpr", Layout::kFlowWrap},
      };
  return *config;
}

class Builder {
 public:
  explicit Builder(absl::string_view sql) : sql_(sql) {}

  // ctx.flatten_query: whether a Query may be laid out inline (true inside a
  // subquery's parentheses) vs. always stacked (false at the top level).
  // ctx.clause_cont: extra indentation for a clause's wrapped content.
  struct Ctx {
    bool flatten_query = false;
    int clause_cont = kDefaultIndent;
  };

  DocPtr Build(const ASTNode* node, Ctx ctx, DocPtr trailer = nullptr) {
    const std::string kind = node->GetNodeKindString();
    DocPtr inner = BuildInner(node, kind, ctx);
    std::vector<DocPtr> parts;
    parts.push_back(Tag(absl::StrCat("<div class=\"ast ast-", kind, "\">")));
    parts.push_back(inner);
    if (trailer != nullptr) parts.push_back(trailer);
    parts.push_back(Tag("</div>"));
    return Concat(std::move(parts));
  }

 private:
  std::vector<Piece> Pieces(const ASTNode* node) {
    const int ns = node->start_location().GetByteOffset();
    const int ne = node->end_location().GetByteOffset();
    std::vector<const ASTNode*> kids;
    for (int i = 0; i < node->num_children(); ++i) {
      const ASTNode* c = node->child(i);
      if (c == nullptr) continue;
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
        if (!code.empty()) parts.push_back(Esc(code));
        break;
      }
      std::string code = CollapseWs(g.substr(i, next - i));
      if (!code.empty()) parts.push_back(Esc(code));
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

  // True if the gap (collapsed) begins with a letter -- i.e. it is a leading
  // keyword like "WHERE ", "GROUP BY ", "AGGREGATE ".
  bool IsKeywordGap(const Piece& p) const {
    std::string c = CollapseWs(GapText(p));
    return !c.empty() && absl::ascii_isalpha(static_cast<unsigned char>(c[0]));
  }

  // Are the separator gaps between children commas? (a comma-separated list)
  bool IsCommaList(const std::vector<Piece>& ps, size_t first_child) const {
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

  // Chooses the layout for `kind`: a configured override if present, the pipe
  // operator layout for "Pipe*", otherwise inferred from the node's structure.
  Layout ResolveLayout(const std::string& kind, const std::vector<Piece>& ps) {
    if (StartsWith(kind, "Pipe")) return Layout::kPipeOp;
    const auto& config = LayoutConfig();
    auto it = config.find(kind);
    if (it != config.end()) return it->second;
    // Structural inference. A leading keyword gap with child content is a
    // clause. (Requiring a child avoids misclassifying a leaf whose text
    // happens to start with a letter, e.g. an identifier.)
    if (!ps.empty() && ps[0].child == nullptr && IsKeywordGap(ps[0]) &&
        HasAnyChild(ps)) {
      return Layout::kClause;
    }
    if (IsCommaList(ps, 0)) return Layout::kList;
    return Layout::kFlow;
  }

  DocPtr BuildInner(const ASTNode* node, const std::string& kind, Ctx ctx) {
    std::vector<Piece> ps = Pieces(node);
    switch (ResolveLayout(kind, ps)) {
      case Layout::kVertical:
        return BuildVertical(ps, ctx);
      case Layout::kSelect:
        return BuildSelect(ps, ctx);
      case Layout::kParen:
        return BuildParen(ps, ctx);
      case Layout::kStatements:
        return BuildStatementList(ps, ctx);
      case Layout::kPipeOp:
        return BuildPipe(ps, ctx);
      case Layout::kClause:
        return BuildClause(ps, ctx);
      case Layout::kList:
        return BuildListBody(ps, 0, ctx);
      case Layout::kFlowWrap:
        return BuildFlow(ps, ctx, /*wrap=*/true);
      case Layout::kFlow:
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
  DocPtr BuildParenQuery(const ASTNode* query, Ctx ctx) {
    Ctx inner = ctx;
    inner.flatten_query = true;
    return Group(Concat(
        {Esc("("), Nest(kDefaultIndent, Concat({Line(""), Build(query, inner)})),
         Line(""), Esc(")")}));
  }

  // keyword + content, where content stays on the keyword line if it fits,
  // else breaks onto indented lines.
  DocPtr BuildClause(const std::vector<Piece>& ps, Ctx ctx) {
    std::string keyword = CollapseWs(GapText(ps[0]));
    keyword = std::string(absl::StripTrailingAsciiWhitespace(keyword));
    DocPtr content;
    if (IsCommaList(ps, 1)) {
      content = BuildListBody(ps, 1, ctx);
    } else {
      content = BuildInnerContent(ps, 1, ctx);
    }
    return Concat({Esc(keyword),
                   Group(Nest(ctx.clause_cont, Concat({Line(" "), content})))});
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
  DocPtr BuildSelect(const std::vector<Piece>& ps, Ctx ctx) {
    std::string keyword;
    size_t first = 0;
    if (!ps.empty() && ps[0].child == nullptr && IsKeywordGap(ps[0])) {
      keyword = std::string(
          absl::StripTrailingAsciiWhitespace(CollapseWs(GapText(ps[0]))));
      first = 1;
    }
    std::vector<const ASTNode*> kids;
    for (size_t i = first; i < ps.size(); ++i) {
      if (ps[i].child != nullptr) kids.push_back(ps[i].child);
    }
    std::vector<DocPtr> parts;
    if (!keyword.empty()) parts.push_back(Esc(keyword));
    bool attached_list = false;
    for (size_t k = 0; k < kids.size(); ++k) {
      const std::string ck = kids[k]->GetNodeKindString();
      if (k == 0 && ck == "SelectList") {
        // List stays on the keyword line, wrapping under it when long.
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
        // The leading "|>" marker (and any stray punctuation/keywords).
        std::string c = CollapseWs(GapText(p));
        if (!c.empty()) parts.push_back(Esc(c));
      }
    }
    return Concat(std::move(parts));
  }

  // Parenthesized subquery: inline "(...)" if it fits, else the contents indent
  // on their own lines between the parentheses.
  DocPtr BuildParen(const std::vector<Piece>& ps, Ctx ctx) {
    Ctx inner_ctx = ctx;
    inner_ctx.flatten_query = true;
    // The first child is the parenthesized query/expression; any later children
    // (e.g. a table alias) follow the closing paren.
    size_t body_idx = ps.size();
    for (size_t i = 0; i < ps.size(); ++i) {
      if (ps[i].child != nullptr) {
        body_idx = i;
        break;
      }
    }
    if (body_idx == ps.size()) return BuildFlow(ps, ctx, /*wrap=*/false);

    DocPtr body = Build(ps[body_idx].child, inner_ctx);
    // Consume the closing-paren gap right after the body, if present.
    size_t after = body_idx + 1;
    if (after < ps.size() && ps[after].child == nullptr &&
        absl::StrContains(GapText(ps[after]), ")")) {
      ++after;
    }
    // Inline "(...)" if it fits; otherwise the contents indent on their own
    // lines between the parentheses.
    DocPtr paren = Group(Concat(
        {Esc("("), Nest(kDefaultIndent, Concat({Line(""), body})), Line(""),
         Esc(")")}));
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
  DocPtr BuildVertical(const std::vector<Piece>& ps, Ctx ctx) {
    std::vector<DocPtr> parts;
    bool first = true;
    for (const Piece& p : ps) {
      if (p.child != nullptr) {
        if (!first) parts.push_back(ClauseSep(ctx));
        parts.push_back(Build(p.child, ctx));
        first = false;
      } else if (GapHasComment(p)) {
        parts.push_back(GapWithComments(p));
      }
    }
    // No Group here: at the top level the clause separators (HardLine) always
    // break; inside a subquery the enclosing parenthesis group governs whether
    // the whole query is inline or broken (so a subquery is either fully on one
    // line or fully indented -- never half-broken).
    return Concat(std::move(parts));
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
};

}  // namespace

absl::StatusOr<std::string> SqlToBoxHtml(absl::string_view sql,
                                         const ASTNode* root,
                                         const LanguageOptions& language_options,
                                         int width) {
  GOOGLESQL_RET_CHECK_NE(root, nullptr);
  const int root_start = root->start_location().GetByteOffset();
  const int root_end = root->end_location().GetByteOffset();
  GOOGLESQL_RET_CHECK_GE(root_start, 0);
  GOOGLESQL_RET_CHECK_LE(root_start, root_end);
  GOOGLESQL_RET_CHECK_LE(root_end, static_cast<int>(sql.size()));
  (void)language_options;  // Comments are detected directly from gap text.

  Builder builder(sql);
  Builder::Ctx ctx;
  ctx.flatten_query = false;
  DocPtr doc = builder.Build(root, ctx);

  Renderer renderer(width);
  std::string body = renderer.Render(doc);
  return absl::StrCat("<div class=\"formatted-sql boxed\">", body, "</div>");
}

}  // namespace googlesql
