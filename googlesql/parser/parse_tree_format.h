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

#ifndef GOOGLESQL_PARSER_PARSE_TREE_FORMAT_H_
#define GOOGLESQL_PARSER_PARSE_TREE_FORMAT_H_

#include "googlesql/parser/ast_node_kind.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace googlesql {

class ASTNode;

// ===========================================================================
// Declarative formatting metadata for parse tree nodes.
//
// This is the C++ side of the formatting attributes declared on AST nodes and
// fields in gen_parse_tree.py (NodeFormat / FieldFormat).  The generator
// resolves attribute inheritance through the class hierarchy (e.g. a layout
// set on the abstract ASTPipeOperator applies to every pipe operator) and
// emits a per-node-kind table into parse_tree_format_generated.cc.
//
// Formatters (currently the box formatter) are generic engines that walk the
// parse tree and consult this table; they should not contain per-node-kind
// logic except for layouts explicitly marked kCustom.
// ===========================================================================

// How a node lays out its children.  A formatter renders each node by
// splitting its source range into child nodes and the literal text gaps
// between them; the layout says how those pieces flow onto lines.
//
// Most nodes do not set a layout and use kDefault inference, which handles
// clauses, comma-separated lists, and inline text without any configuration.
enum class ASTFormatLayout {
  // No explicit layout.  The formatter infers one from the node's structure:
  // a leading keyword gap followed by child content formats as a clause;
  // comma-separated children format as a list; anything else flows inline.
  kDefault,

  // Children and gap text flow inline with no break points.  (This is also
  // the fallback when kDefault inference finds no structure; set it
  // explicitly only to suppress inference.)
  kFlow,

  // Like kFlow, but each gap between two children (an infix operator such as
  // " AND ") is a break point: the whole node renders on one line if it
  // fits, otherwise it breaks before every operator at the current indent.
  // Used for AND/OR chains.
  kFlowWrap,

  // Comma-separated items: all inline if they fit, else one item per line
  // with each comma attached inside the preceding item's box.  (Usually
  // inferred by kDefault; set explicitly only when inference fails.)
  kList,

  // A leading keyword followed by content: the content stays on the keyword
  // line if it fits, else drops to indented lines.  (Usually inferred.)
  kClause,

  // Children stack, one per line, unconditionally at the top level.  Inside
  // parentheses the enclosing paren group decides whether the whole node is
  // inline or broken.  Used by ASTQuery for its clause / pipe-operator list.
  // Children that are pipe operators (layout kPipeOp) are rendered as
  // alternately-tinted segments.
  kStack,

  // Like kStack, but with a leading keyword, and the field marked with role
  // 'head' stays on the keyword line (wrapping under it when long) while all
  // other children stack below, aligned with the keyword.  Used by ASTSelect:
  // SELECT + select list on one line; FROM / WHERE / GROUP BY... below.
  kKeywordStack,

  // A parenthesized body: "(...)" inline if it fits, else the body indents
  // on its own lines between the parentheses.  The field with role
  // 'paren_body' is the body (colored as a subquery region); children after
  // the closing paren (e.g. an alias) render inline after it.
  kParen,

  // A function call: callee + parenthesized argument list.  If the arguments
  // form a clean comma list they may break one-per-line between the
  // parentheses; otherwise the call renders inline losslessly.
  kCall,

  // A chain of set-operation operands: operands stack with the operator text
  // (read from the gap between them, e.g. "UNION ALL") on its own line in
  // between.  Fields with role 'skip' (e.g. the metadata list, whose source
  // range overlaps the operands) are excluded.
  kSetChain,

  // A join tree: the (left-nested, same-kind) tree is flattened into
  // operands and operator tokens in source order.  Comma joins render as a
  // comma list; each explicit JOIN keyword starts a new line aligned with
  // the leading table.
  kJoin,

  // A keyworded arm construct like CASE: starts with header_keyword, breaks
  // before each arm keyword in break_before_keywords, and ends with
  // footer_keyword dedented back to the header column.  Inline if short.
  kCase,

  // A statement list (scripts): one statement per line, ";" attached.
  kStatements,

  // A pipe operator: "|> " at the left margin, the operator name on the same
  // line, continuation lines indented under the operator name.  Set on the
  // abstract ASTPipeOperator and inherited by all pipe operators.
  kPipeOp,

  // Escape hatch: this node needs bespoke formatting code registered in the
  // formatter itself.  Use only when the vocabulary above cannot express the
  // layout; the goal is that very few nodes ever need this.
  kCustom,
};

// Resolved formatting metadata for one node kind.  Produced by
// gen_parse_tree.py; see NodeFormat / FieldFormat there for the attribute
// syntax.  Field roles are exposed as typed accessor functions so formatters
// can identify which child plays which role without matching on node-kind
// strings.  All accessors are null when no field declares the role.
struct ASTNodeFormat {
  ASTFormatLayout layout = ASTFormatLayout::kDefault;

  // This node is not rendered by its parent at all (its source range
  // duplicates text owned by siblings).  E.g. ASTPipeJoinLhsPlaceholder.
  bool skip = false;

  // kCase only: the opening keyword (e.g. "CASE"), the arm keywords that
  // force a line break before them (e.g. WHEN / ELSE), and the closing
  // keyword (e.g. "END").
  absl::string_view header_keyword;
  absl::Span<const absl::string_view> break_before_keywords;
  absl::string_view footer_keyword;

  // CSS class of the clickable info region wrapped around this node when an
  // annotator supplies info HTML for it (e.g. "stmt" for statements, "func"
  // for function calls).  Empty = no info region.
  absl::string_view info_region;

  // Field with role 'head' (kKeywordStack): stays on the keyword line.
  const ASTNode* (*head_child)(const ASTNode&) = nullptr;

  // Field with role 'paren_body' (kParen): the parenthesized body.
  const ASTNode* (*paren_body_child)(const ASTNode&) = nullptr;

  // Field with attribute region='<class>': the child to wrap in a named
  // color region (e.g. the table-name path of a table reference), and the
  // region's CSS class.
  const ASTNode* (*region_child)(const ASTNode&) = nullptr;
  absl::string_view region_class;

  // True if `child` (a child of node `n`) is a field with role 'skip' and
  // must be excluded from formatting (typically because its source range
  // overlaps its siblings').
  bool (*is_skipped_child)(const ASTNode& n, const ASTNode& child) = nullptr;
};

// Returns the formatting metadata for `kind`.  Node kinds with no declared
// formatting attributes get a default-constructed spec (layout inference).
// The returned reference is valid forever.
const ASTNodeFormat& GetASTNodeFormat(ASTNodeKind kind);

}  // namespace googlesql

#endif  // GOOGLESQL_PARSER_PARSE_TREE_FORMAT_H_
