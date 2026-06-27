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

#include "googlesql/resolved_ast/resolved_node.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "googlesql/common/box_glyphs.h"
#include "googlesql/common/thread_stack.h"
#include "googlesql/public/constant.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function.pb.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_location_range.pb.h"
#include "googlesql/public/proto/type_annotation.pb.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_parameters.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_enums.pb.h"
#include "googlesql/resolved_ast/resolved_collation.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "googlesql/resolved_ast/serialization.pb.h"
#include "absl/container/flat_hash_set.h"
#include "googlesql/base/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "googlesql/base/map_util.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace googlesql {

#define RETURN_ERROR_IF_OUT_OF_STACK_SPACE() \
  GOOGLESQL_RETURN_IF_NOT_ENOUGH_STACK(      \
      "Out of stack space due to deeply nested query expression")

// ResolvedNode::RestoreFrom is generated in resolved_node.cc.template.

absl::Status ResolvedNode::Accept(ResolvedASTVisitor* visitor) const {
  return absl::OkStatus();
}

absl::Status ResolvedNode::ChildrenAccept(ResolvedASTVisitor* visitor) const {
  return absl::OkStatus();
}

void ResolvedNode::SetParseLocationRange(
    const ParseLocationRange& parse_location_range) {
  parse_location_range_ =
      std::make_unique<ParseLocationRange>(parse_location_range);
}

void ResolvedNode::ClearParseLocationRange() { parse_location_range_.reset(); }

void ResolvedNode::SetOperatorKeywordLocationRange(
    const ParseLocationRange& range) {
  operator_keyword_parse_location_range_ =
      std::make_unique<ParseLocationRange>(range);
}

void ResolvedNode::ClearOperatorKeywordLocationRange() {
  operator_keyword_parse_location_range_.reset();
}

std::string ResolvedNode::DebugString(const DebugStringConfig& config) const {
  std::string output;
  DebugStringImpl(this, config, /*prefix=1*/ "", /*prefix2=*/"", &output);
  return output;
}

namespace {
std::string HtmlEscape(absl::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}
}  // namespace

// True if `node` has a ResolvedScan anywhere in its subtree (e.g. an expression
// that contains a subquery).  Used by the HTML emitter to decide whether to
// descend into a non-scan child to surface its nested scans as their own boxes.
static bool SubtreeContainsScan(const ResolvedNode* node) {
  std::vector<const ResolvedNode*> children;
  node->GetChildNodes(&children);
  for (const ResolvedNode* child : children) {
    if (child == nullptr) continue;
    if (child->IsScan() || SubtreeContainsScan(child)) return true;
  }
  return false;
}

std::string ResolvedNode::DebugStringHtml(
    std::vector<const ResolvedScan*>* scan_order) const {
  DebugStringConfig config;
  config.linear_mode = true;
  config.omit_parse_location = true;
  int scan_counter = 0;
  std::string output;
  EmitNodeHtml(this, config, &scan_counter, scan_order, &output, /*depth=*/0);
  return output;
}

void ResolvedNode::EmitNodeHtml(const ResolvedNode* node,
                                const DebugStringConfig& config,
                                int* scan_counter,
                                std::vector<const ResolvedScan*>* scan_order,
                                std::string* output, int depth) {
  if (node == nullptr) {
    absl::StrAppend(output, "<div class=\"rscan-stmt\">&lt;nullptr&gt;</div>");
    return;
  }
  if (node->IsScan()) {
    EmitScanChainHtml(node->GetAs<ResolvedScan>(), config, scan_counter,
                      scan_order, output, depth);
    return;
  }
  // A non-scan node (typically the statement): render as a labeled block whose
  // scan fields become nested query boxes.
  absl::StrAppend(output, "<div class=\"rscan-stmt\"><div class=\"rscan-head\">",
                  HtmlEscape(node->node_kind_string()), "</div>");
  EmitScanFieldsHtml(node, config, /*elide=*/nullptr, scan_counter, scan_order,
                     output, depth, /*top_level=*/true);
  absl::StrAppend(output, "</div>");
}

void ResolvedNode::EmitScanChainHtml(
    const ResolvedScan* scan, const DebugStringConfig& config,
    int* scan_counter, std::vector<const ResolvedScan*>* scan_order,
    std::string* output, int depth) {
  // Collect the pipe spine: spine.back() is the source (leaf) scan, spine[0]
  // is the top of the chain.  A leaf scan with no pipe input is a one-element
  // spine.
  std::vector<const ResolvedScan*> spine;
  for (const ResolvedScan* s = scan; s != nullptr; s = s->GetPipeInputScan()) {
    spine.push_back(s);
  }
  // This whole chain is one query: colour it by family (blue/green alternating
  // by subquery `depth`), and within it the initial source then each pipe
  // operator alternate a darker ("-b", first) then lighter ("-a") shade.
  const char* family = (depth % 2 == 0) ? "blue" : "green";
  // Emit source first, then each operator stacked below it.
  int pos = 0;
  for (int i = static_cast<int>(spine.size()) - 1; i >= 0; --i, ++pos) {
    const ResolvedScan* s = spine[i];
    const bool is_operator = (i != static_cast<int>(spine.size()) - 1);
    const int id = (*scan_counter)++;
    if (scan_order != nullptr) scan_order->push_back(s);
    const char* shade = (pos % 2 == 0) ? "b" : "a";
    // An operator box is headed by "|> <Scan>"; tag it so its field tree indents
    // by the width of the "|> " prefix to line up under the scan name (matching
    // the textual linear DebugString).
    absl::StrAppend(output, "<div class=\"rscan seg-", family, "-", shade,
                    (is_operator ? " rscan-op" : ""), "\" data-node-id=\"r", id,
                    "\"><div class=\"rscan-head\">",
                    (is_operator ? "|&gt; " : ""),
                    HtmlEscape(s->node_kind_string()), "</div>");
    EmitScanFieldsHtml(s, config,
                       /*elide=*/is_operator ? s->GetPipeInputScan() : nullptr,
                       scan_counter, scan_order, output, depth);
    absl::StrAppend(output, "</div>");
  }
}

void ResolvedNode::EmitScanFieldsHtml(
    const ResolvedNode* scan, const DebugStringConfig& config,
    const ResolvedNode* elide, int* scan_counter,
    std::vector<const ResolvedScan*>* scan_order, std::string* output,
    int depth, bool top_level) {
  std::vector<DebugStringField> fields;
  scan->CollectDebugStringFields(&fields);

  // The statement's query is the depth-0 query (the statement adds no nesting
  // level); a scan's scan-child fields are genuine subqueries one level deeper.
  const int nested_depth = top_level ? depth : depth + 1;

  // Build the displayable field list (drop the elided pipe-input scan and the
  // parse_location noise) so connectors can be chosen from each field's true
  // position among its siblings.
  std::vector<const DebugStringField*> display;
  for (const DebugStringField& field : fields) {
    if (elide != nullptr && field.nodes.size() == 1 &&
        field.nodes[0] == elide) {
      continue;
    }
    if (config.omit_parse_location && field.name == "parse_location") {
      continue;
    }
    display.push_back(&field);
  }

  const BoxGlyphs& glyphs = kUnicodeBoxGlyphs;
  // Pending box-glyph tree text; flushed as one `<div class="rscan-tree">`
  // whenever a nested box must be emitted between two fields, so the connector
  // lines of the leaf fields and the field-name lines of the scan fields all
  // belong to one connected tree.
  std::string text;
  auto flush_text = [&]() {
    if (text.empty()) return;
    if (text.back() == '\n') text.pop_back();
    absl::StrAppend(output, "<div class=\"rscan-tree\">", HtmlEscape(text),
                    "</div>");
    text.clear();
  };

  for (size_t i = 0; i < display.size(); ++i) {
    const DebugStringField& field = *display[i];
    const bool is_last = (i + 1 == display.size());

    // Does this field introduce a nested scan (a subquery shown as its own box)?
    bool has_scan = false;
    for (const ResolvedNode* child : field.nodes) {
      if (child != nullptr && (child->IsScan() || SubtreeContainsScan(child))) {
        has_scan = true;
        break;
      }
    }
    if (!has_scan) {
      // A leaf field (scalar or non-scan subtree): part of the connected tree.
      AppendFieldTreeText(field, is_last, config, &text);
      continue;
    }

    // A scan-child field is a sibling of the leaf fields: render its name as a
    // tree node ("|-name=") and indent the nested scan/subquery box underneath.
    absl::StrAppend(&text, is_last ? glyphs.tree_last : glyphs.tree_branch);
    if (!field.name.empty()) absl::StrAppend(&text, field.name, "=");
    absl::StrAppend(&text, "\n");
    flush_text();

    std::vector<const ResolvedNode*> leaf_children;
    for (const ResolvedNode* child : field.nodes) {
      // A subpipeline (e.g. a `|> FORK` branch) is "like the final subquery in a
      // ResolvedWithScan": render its pipe chain as its own nested query box so
      // it reads -- and is selectable -- as a subquery, with the same depth-
      // based colours and stacked, selectable pipe operators.
      const ResolvedScan* subpipeline_scan = nullptr;
      if (child != nullptr && child->Is<ResolvedGeneralizedQuerySubpipeline>()) {
        const ResolvedSubpipeline* sp =
            child->GetAs<ResolvedGeneralizedQuerySubpipeline>()->subpipeline();
        if (sp != nullptr) subpipeline_scan = sp->scan();
      } else if (child != nullptr && child->Is<ResolvedSubpipeline>()) {
        subpipeline_scan = child->GetAs<ResolvedSubpipeline>()->scan();
      }
      if (subpipeline_scan != nullptr) {
        // The `data-layer` label (no visible heading) makes the box a selectable
        // hierarchy layer like the input pane's, so a pipe operator inside it
        // reads as "in Subpipeline".
        absl::StrAppend(output,
                        "<div class=\"rscan-children\">"
                        "<div class=\"rscan-query\" data-layer=\"Subpipeline\">");
        EmitScanChainHtml(subpipeline_scan, config, scan_counter, scan_order,
                          output, nested_depth);
        absl::StrAppend(output, "</div></div>");
      } else if (child != nullptr && child->IsScan()) {
        // The `data-layer` label (no visible heading) makes the box a selectable
        // hierarchy layer.  The statement's own query (top_level) is "Query with
        // pipe operators"; a scan-child of another scan is a "Subquery" (matching
        // the input pane's layers).
        absl::StrAppend(
            output, "<div class=\"rscan-children\"><div class=\"rscan-query\" data-layer=\"",
            top_level ? "Query with pipe operators" : "Subquery", "\">");
        // A nested scan child is a subquery: deeper nesting → next colour family.
        EmitScanChainHtml(child->GetAs<ResolvedScan>(), config, scan_counter,
                          scan_order, output, nested_depth);
        absl::StrAppend(output, "</div></div>");
      } else if (child != nullptr && SubtreeContainsScan(child)) {
        // A non-scan child that contains a subquery (e.g. a WHERE filter
        // expression with a scalar subquery).  Expand it structurally so the
        // subquery's scans become their own boxes -- clickable and correlated
        // across panes -- rather than being buried in flat DebugString text.
        absl::StrAppend(output, "<div class=\"rscan-children\"><div class=\"rscan-field\">",
                        HtmlEscape(child->node_kind_string()), "</div>");
        // Still expanding the same expression (the subquery scan inside it picks
        // up depth+1 when EmitScanChainHtml reaches it), so keep `depth`.
        EmitScanFieldsHtml(child, config, /*elide=*/nullptr, scan_counter,
                           scan_order, output, depth);
        absl::StrAppend(output, "</div>");
      } else {
        leaf_children.push_back(child);
      }
    }
    if (!leaf_children.empty()) {
      // Rare: a scan-child field that also has plain children. Render them as a
      // small indented tree under the (already-emitted) field name.
      DebugStringField leaf_field = field;
      leaf_field.name.clear();
      leaf_field.nodes = std::move(leaf_children);
      std::string sub;
      AppendFieldTreeText(leaf_field, /*is_last=*/true, config, &sub);
      if (!sub.empty() && sub.back() == '\n') sub.pop_back();
      absl::StrAppend(output,
                      "<div class=\"rscan-children\"><div class=\"rscan-tree\">",
                      HtmlEscape(sub), "</div></div>");
    }
  }
  flush_text();
}

void ResolvedNode::AppendFieldTreeText(const DebugStringField& field,
                                       bool is_last,
                                       const DebugStringConfig& config,
                                       std::string* text) {
  // Always render with Unicode box glyphs (the nicer connector lines).
  DebugStringConfig tree_config = config;
  tree_config.use_box_glyphs = true;
  const BoxGlyphs& glyphs = kUnicodeBoxGlyphs;

  const absl::string_view field_connector =
      is_last ? glyphs.tree_last : glyphs.tree_branch;
  const absl::string_view field_indent =
      is_last ? glyphs.tree_space : glyphs.tree_vertical;
  const bool print_field_name = !field.name.empty();
  const bool value_has_newlines = absl::StrContains(field.value, "\n");
  const bool print_one_line = field.nodes.empty() && !value_has_newlines;

  if (print_field_name) {
    absl::StrAppend(text, field_connector, field.name, "=");
    if (print_one_line) absl::StrAppend(text, field.value);
    absl::StrAppend(text, "\n");
  } else if (print_one_line) {
    absl::StrAppend(text, field_connector, field.value, "\n");
  }

  if (!print_one_line) {
    if (value_has_newlines) {
      absl::StrAppend(text, field_indent, "  \"\"\"\n");
      for (absl::string_view line : absl::StrSplit(field.value, '\n')) {
        std::string line_content = absl::StrCat(field_indent, "  ", line);
        absl::StrAppend(text, absl::StripTrailingAsciiWhitespace(line_content),
                        "\n");
      }
      absl::StrAppend(text, field_indent, "  \"\"\"\n");
    }
    for (const ResolvedNode* child : field.nodes) {
      const bool is_last_node = (child == field.nodes.back());
      const absl::string_view field_name_indent =
          print_field_name ? field_indent : absl::string_view{};
      const absl::string_view field_value_indent =
          (!is_last_node || (!print_field_name && !is_last))
              ? glyphs.tree_vertical
              : glyphs.tree_space;
      const absl::string_view node_connector =
          is_last_node ? glyphs.tree_last : glyphs.tree_branch;
      DebugStringImpl(child, tree_config,
                      absl::StrCat(field_name_indent, field_value_indent),
                      absl::StrCat(field_name_indent, node_connector), text,
                      /*pipe_input_to_elide=*/nullptr);
    }
  }
}

void ResolvedNode::AppendAnnotations(
    const ResolvedNode* node, absl::Span<const NodeAnnotation> annotations,
    std::string* output) {
  if (node == nullptr) {
    return;
  }
  for (const NodeAnnotation& annotation : annotations) {
    if (annotation.node == node) {
      absl::StrAppend(output, " ", annotation.annotation);
      return;
    }
  }
}

void ResolvedNode::DebugStringImpl(const ResolvedNode* node,
                                   const DebugStringConfig& config,
                                   absl::string_view prefix1,
                                   absl::string_view prefix2,
                                   std::string* output,
                                   const ResolvedNode* pipe_input_to_elide) {
  // In linear_mode, render a scan and its "pipe input" spine flattened like
  // pipe syntax. The source (deepest input) scan is printed first, then each
  // downstream scan is stacked below it as a "|> <Scan>" operator. A vertical
  // bar connects the operators down the left edge, aligned under the source
  // scan's connector, so the chain still reads as one tree branch. Within each
  // scan's own fields, the consumed pipe input is shown inline as "<pipe_input>"
  // (see `pipe_input_to_elide`) rather than recursed into.
  if (config.linear_mode && node != nullptr && node->IsScan() &&
      node->GetAs<ResolvedScan>()->GetPipeInputScan() != nullptr) {
    const BoxGlyphs& glyphs =
        config.use_box_glyphs ? kUnicodeBoxGlyphs : kAsciiBoxGlyphs;

    // Collect the pipe spine: spine[0] is this scan (the top of the chain) and
    // spine.back() is the source scan (whose pipe input is null).
    std::vector<const ResolvedScan*> spine;
    for (const ResolvedScan* scan = node->GetAs<ResolvedScan>();
         scan != nullptr; scan = scan->GetPipeInputScan()) {
      spine.push_back(scan);
    }

    // `stem` is `prefix2` minus its trailing tree connector ("+-"/"├─"/"└─").
    // The connector's corner sits at column width(stem); the vertical bar and
    // the "|" of each "|>" are drawn in that same column.
    absl::string_view connector =
        absl::EndsWith(prefix2, glyphs.tree_last)       ? glyphs.tree_last
        : absl::EndsWith(prefix2, glyphs.tree_branch)   ? glyphs.tree_branch
                                                        : absl::string_view{};
    absl::string_view stem = prefix2;
    stem.remove_suffix(connector.size());

    // The continuation bar that connects the operators is drawn with a faint
    // "." (rather than the "|" of "|>") so it reads as a light outline.
    constexpr absl::string_view kPipeBar = ".";

    // Source scan: extend its connector by one horizontal glyph ("+-" -> "+--")
    // so its name lines up with the operator names, and continue the bar below.
    DebugStringBody(spine.back(), config,
                    /*name_prefix=*/absl::StrCat(prefix2, glyphs.horizontal),
                    /*field_prefix=*/absl::StrCat(stem, kPipeBar, "  "),
                    output, /*pipe_input_to_elide=*/nullptr);

    // Operators, from just above the source up to the top. The top (spine[0])
    // is printed last and ends the bar (its fields get no trailing bar).
    for (int i = static_cast<int>(spine.size()) - 2; i >= 0; --i) {
      const bool is_last = (i == 0);
      DebugStringBody(
          spine[i], config,
          /*name_prefix=*/absl::StrCat(stem, glyphs.vertical, "> "),
          /*field_prefix=*/
          is_last ? absl::StrCat(stem, "   ")
                  : absl::StrCat(stem, kPipeBar, "  "),
          output, /*pipe_input_to_elide=*/spine[i]->GetPipeInputScan());
    }
    return;
  }

  DebugStringBody(node, config, /*name_prefix=*/prefix2,
                  /*field_prefix=*/prefix1, output, pipe_input_to_elide);
}

void ResolvedNode::DebugStringBody(const ResolvedNode* node,
                                   const DebugStringConfig& config,
                                   absl::string_view name_prefix,
                                   absl::string_view field_prefix,
                                   std::string* output,
                                   const ResolvedNode* pipe_input_to_elide) {
  const BoxGlyphs& glyphs =
      config.use_box_glyphs ? kUnicodeBoxGlyphs : kAsciiBoxGlyphs;

  const ResolvedNode* elide = pipe_input_to_elide;

  std::vector<DebugStringField> fields;

  // Trees containing nullptr AST nodes are not valid; however we still want
  // them to display a debug string indicating where the null node is in the
  // tree, as this makes debugging easier.
  if (node != nullptr) {
    node->CollectDebugStringFields(&fields);
  }

  // The visual renderer records parse locations for correspondence but does not
  // want them shown; drop them from every node so the displayed tree matches the
  // clean textual DebugString.
  if (config.omit_parse_location) {
    fields.erase(std::remove_if(fields.begin(), fields.end(),
                                [](const DebugStringField& field) {
                                  return field.name == "parse_location";
                                }),
                 fields.end());
  }

  // In linear_mode, the scan field consumed as the pipe input (which may be
  // nested, e.g. the `scan` of a ResolvedSetOperationItem) is either omitted
  // entirely (default) or shown inline as an "<pipe_input>" placeholder,
  // wherever it appears among the descendants.
  if (elide != nullptr) {
    auto is_pipe_input = [elide](const DebugStringField& field) {
      return field.nodes.size() == 1 && field.nodes[0] == elide;
    };
    if (config.omit_pipe_input_scan_field) {
      fields.erase(std::remove_if(fields.begin(), fields.end(), is_pipe_input),
                   fields.end());
    } else {
      for (DebugStringField& field : fields) {
        if (is_pipe_input(field)) {
          field.value = "<pipe_input>";
          field.nodes.clear();
        }
      }
    }
  }

  bool multiline = false;
  for (const DebugStringField& field : fields) {
    if (!field.nodes.empty()) {
      multiline = true;
      break;
    }
  }

  if (node != nullptr) {
    absl::StrAppend(output, name_prefix, node->GetNameForDebugString(config));
  } else {
    absl::StrAppend(output, name_prefix, "<nullptr AST node>");
  }

  if (fields.empty()) {
    AppendAnnotations(node, config.annotations, output);
    *output += "\n";
  } else if (multiline) {
    AppendAnnotations(node, config.annotations, output);
    *output += "\n";

    for (const DebugStringField& field : fields) {
      const bool is_last_field = (&field == &fields.back());
      absl::string_view field_connector =
          is_last_field ? glyphs.tree_last : glyphs.tree_branch;
      absl::string_view field_indent =
          is_last_field ? glyphs.tree_space : glyphs.tree_vertical;

      const bool print_field_name = !field.name.empty();
      const bool value_has_newlines = absl::StrContains(field.value, "\n");
      const bool print_one_line = field.nodes.empty() && !value_has_newlines;

      absl::string_view column_created_string =
          config.print_created_columns && field.column_created ? "{c}" : "";
      absl::string_view accessed_string =
          config.print_accessed ? field.accessed ? "{*}" : "{ }" : "";

      if (print_field_name) {
        absl::StrAppend(output, field_prefix, field_connector, field.name,
                        column_created_string, accessed_string, "=");
        if (print_one_line) {
          absl::StrAppend(output, field.value);
        }
        absl::StrAppend(output, "\n");
      } else if (print_one_line) {
        absl::StrAppend(output, field_prefix, field_connector, field.value,
                        column_created_string, accessed_string, "\n");
      }

      if (!print_one_line) {
        if (value_has_newlines) {
          absl::StrAppend(output, field_prefix, field_indent, "  \"\"\"\n");
          for (auto line : absl::StrSplit(field.value, '\n')) {
            std::string line_content = absl::StrCat(field_indent, "  ", line);
            absl::StrAppend(output, field_prefix,
                            absl::StripTrailingAsciiWhitespace(line_content),
                            "\n");
          }
          absl::StrAppend(output, field_prefix, field_indent, "  \"\"\"\n");
        }

        for (const ResolvedNode* child : field.nodes) {
          bool is_last_node = (child == field.nodes.back());
          const absl::string_view field_name_indent =
              print_field_name ? field_indent : "";
          const absl::string_view field_value_indent =
              (!is_last_node || (!print_field_name && !is_last_field)
                   ? glyphs.tree_vertical
                   : glyphs.tree_space);
          absl::string_view node_connector =
              is_last_node ? glyphs.tree_last : glyphs.tree_branch;

          DebugStringImpl(
              child, config,
              absl::StrCat(field_prefix, field_name_indent, field_value_indent),
              absl::StrCat(field_prefix, field_name_indent, node_connector),
              output, elide);
        }
      }
    }
  } else {
    *output += "(";
    for (const DebugStringField& field : fields) {
      absl::string_view column_created_string =
          config.print_created_columns && field.column_created ? "{c}" : "";
      absl::string_view accessed_string =
          config.print_accessed ? field.accessed ? "{*}" : "{ }" : "";

      if (&field != &fields[0]) *output += ", ";
      if (field.name.empty()) {
        absl::StrAppend(output, field.value, column_created_string,
                        accessed_string);
      } else {
        absl::StrAppend(output, field.name, column_created_string,
                        accessed_string, "=", field.value);
      }
    }
    *output += ")";
    AppendAnnotations(node, config.annotations, output);
    *output += "\n";
  }
}

void ResolvedNode::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  // Print parse_location if available.
  const auto location = GetParseLocationRangeOrNULL();
  if (location != nullptr) {
    fields->emplace_back("parse_location", location->GetString(),
                         /*accessed_in=*/false, /*column_created_in=*/false);
  }
  const ParseLocationRange* operator_keyword_location =
      GetOperatorKeywordLocationRangeOrNULL();
  if (operator_keyword_location != nullptr) {
    fields->emplace_back("operator_keyword_location",
                         operator_keyword_location->GetString(),
                         /*accessed_in=*/false, /*column_created_in=*/false);
  }
}

bool ResolvedNode::HasDebugStringFieldsWithNodes() const {
  std::vector<DebugStringField> fields;
  CollectDebugStringFields(&fields);
  for (const DebugStringField& field : fields) {
    if (!field.nodes.empty()) {
      return true;
    }
  }
  return false;
}

std::string ResolvedNode::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return node_kind_string();
}

absl::Status ResolvedNode::CheckNoFieldsAccessed() const {
  return absl::OkStatus();
}

void ResolvedNode::ClearFieldsAccessed() const {}

void ResolvedNode::MarkFieldsAccessed() const {}

// NOTE: An equivalent method on ASTNodes exists in ../parser/parse_tree.cc.
void ResolvedNode::GetDescendantsWithKinds(
    const std::set<ResolvedNodeKind>& node_kinds,
    std::vector<const ResolvedNode*>* found_nodes) const {
  found_nodes->clear();

  // Use non-recursive traversal to avoid stack issues.
  std::queue<const ResolvedNode*> node_queue;
  node_queue.push(this);

  std::vector<const ResolvedNode*> tmp_vector;

  while (!node_queue.empty()) {
    const ResolvedNode* node = node_queue.front();
    node_queue.pop();

    if (googlesql_base::ContainsKey(node_kinds, node->node_kind())) {
      // Emit this node and don't scan its children.
      found_nodes->push_back(node);
    } else {
      // Else queue its children for traversal.
      tmp_vector.clear();
      node->GetChildNodes(&tmp_vector);
      for (const ResolvedNode* tmp_node : tmp_vector) {
        node_queue.push(tmp_node);
      }
    }
  }
}

void ResolvedNode::GetDescendantsSatisfying(
    bool (ResolvedNode::*filter_method)() const,
    std::vector<const ResolvedNode*>* found_nodes) const {
  found_nodes->clear();

  // Use non-recursive traversal to avoid stack issues.
  std::queue<const ResolvedNode*> node_queue;
  node_queue.push(this);

  std::vector<const ResolvedNode*> tmp_vector;

  while (!node_queue.empty()) {
    const ResolvedNode* node = node_queue.front();
    node_queue.pop();

    if ((node->*filter_method)()) {
      found_nodes->push_back(node);
    }

    // Queue node's children for traversal.
    tmp_vector.clear();
    node->GetChildNodes(&tmp_vector);
    for (const ResolvedNode* tmp_node : tmp_vector) {
      node_queue.push(tmp_node);
    }
  }
}

std::vector<ResolvedColumn> ResolvedNode::GetColumnsCreated() const {
  return std::vector<ResolvedColumn>();
}

std::vector<ResolvedColumn> ResolvedNode::GetColumnsReferenced() const {
  return std::vector<ResolvedColumn>();
}

// NameFormat nodes format as
//   <name> := <node>
// if <node> fits on one line (because it has no child fields to print).
//
// Otherwise, they format as
//   <name> :=
//     <node>
//       <children of node>
//       ...
void ResolvedNode::CollectDebugStringFieldsWithNameFormat(
    const ResolvedNode* node, std::vector<DebugStringField>* fields) const {
  ABSL_DCHECK(fields->empty());
  if (node == nullptr) {
    return;
  }
  if (node->HasDebugStringFieldsWithNodes()) {
    fields->emplace_back("" /* name */, node, /*accessed_in=*/false,
                         /*column_created_in=*/false);
  } else {
    node->CollectDebugStringFields(fields);
  }
}

std::string ResolvedNode::GetNameForDebugStringWithNameFormat(
    absl::string_view name, const ResolvedNode* node) const {
  if (node == nullptr) {
    return absl::StrCat(name, " := <nullptr AST node>");
  } else if (node->HasDebugStringFieldsWithNodes()) {
    return absl::StrCat(name, " :=");
  } else {
    return absl::StrCat(name, " := ", node->GetNameForDebugString({}));
  }
}

int ResolvedNode::GetTreeDepth() const {
  int max_depth = 0;
  std::vector<const ResolvedNode*> children;
  GetChildNodes(&children);
  for (const ResolvedNode* child : children) {
    const int child_depth = child->GetTreeDepth();
    if (child_depth > max_depth) {
      max_depth = child_depth;
    }
  }
  return max_depth + 1;
}

absl::Status ResolvedNode::SaveTo(FileDescriptorSetMap* file_descriptor_set_map,
                                  ResolvedNodeProto* proto) const {
  const ParseLocationRange* parse_location_range =
      GetParseLocationRangeOrNULL();
  if (parse_location_range == nullptr) {
    return absl::OkStatus();
  }
  // Serialize parse location range.
  GOOGLESQL_ASSIGN_OR_RETURN(*proto->mutable_parse_location_range(),
                   parse_location_range->ToProto());
  return absl::OkStatus();
}

// Methods for classes in the generated code with customized DebugStrings.

// ResolvedComputedColumn gets formatted as
//   <name> := <expr>
// with <expr>'s children printed as its own children.
void ResolvedComputedColumn::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  CollectDebugStringFieldsWithNameFormat(expr_.get(), fields);
}

std::string ResolvedComputedColumn::GetNameForDebugString(
    const DebugStringConfig& config) const {
  std::string name = column_.ShortDebugString();
  if (config.print_created_columns) {
    absl::StrAppend(&name, "{c}");
  }
  return GetNameForDebugStringWithNameFormat(name, expr_.get());
}

void ResolvedDeferredComputedColumn::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  CollectDebugStringFieldsWithNameFormat(expr_.get(), fields);
}

std::string ResolvedDeferredComputedColumn::GetNameForDebugString(
    const DebugStringConfig& config) const {
  std::string name = column().ShortDebugString();
  if (config.print_created_columns) {
    absl::StrAppend(&name, "{c}");
  }
  absl::StrAppend(&name, " [side_effect_column=",
                  side_effect_column_.ShortDebugString(), "]");
  return GetNameForDebugStringWithNameFormat(name, expr());
}

// ResolvedOutputColumn gets formatted as
//   <column> AS <name> [<column->type>]
void ResolvedOutputColumn::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  ABSL_DCHECK(fields->empty());
}

std::string ResolvedOutputColumn::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat(column_.DebugString(), " AS ", ToIdentifierLiteral(name_),
                      " [", column_.type()->DebugString(), "]");
}

// ResolvedConstant gets formatted as
//   Constant(name(constant), type[, value]).
void ResolvedConstant::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  ABSL_DCHECK_LE(fields->size(), 2);  // type and parse location

  fields->emplace(fields->begin(), "", constant_->FullName(),
                  /*accessed_in=*/false, /*column_created_in=*/false);
  fields->emplace_back("value", constant_->ConstantValueDebugString(),
                       constant_accessed(), /*column_created_in=*/false);
}

std::string ResolvedConstant::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat("Constant");
}

void ResolvedSystemVariable::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  fields->emplace(fields->begin(), "",
                  absl::StrJoin(name_path_, ".",
                                [](std::string* out, absl::string_view in) {
                                  absl::StrAppend(out, ToIdentifierLiteral(in));
                                }),
                  name_path_accessed(), /*column_created_in=*/false);
}

std::string ResolvedSystemVariable::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat("SystemVariable");
}

// ResolvedFunctionCall gets formatted as
//   FunctionCall(name(arg_types) -> type)
// with only <arguments> printed as children.
void ResolvedFunctionCallBase::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);

  ABSL_DCHECK_LE(fields->size(), 3);  // type, parse_location and type_annotation_map

  // Clear the "type" field if present.
  fields->erase(std::remove_if(
                    fields->begin(), fields->end(),
                    [](const DebugStringField& x) { return x.name == "type"; }),
                fields->end());

  if (!argument_list_.empty()) {
    // Use empty name to avoid printing "arguments=" with extra indentation.
    fields->emplace_back("", argument_list_, argument_list_accessed(),
                         /*column_created_in=*/false);
  } else if (!generic_argument_list_.empty()) {
    fields->emplace_back("", generic_argument_list_,
                         generic_argument_list_accessed(),
                         /*column_created_in=*/false);
  }
  if (!hint_list_.empty()) {
    fields->emplace_back("hint_list", hint_list_, hint_list_accessed(),
                         /*column_created_in=*/false);
  }
  if (!collation_list_.empty()) {
    fields->emplace_back(
        "collation_list", ResolvedCollation::ToString(collation_list_),
        collation_list_accessed(), /*column_created_in=*/false);
  }
}

std::string ResolvedFunctionCallBase::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat(
      node_kind_string(), "(",
      error_mode_ == SAFE_ERROR_MODE ? "{SAFE_ERROR_MODE} " : "",
      function_ != nullptr ? function_->DebugString() : "<unknown>",
      signature_->DebugString(), ")");
}

// ResolvedCast gets formatted as
//   Cast(<from_type> -> <to_type>)
// with only <from_expr> and <return_null_on_error> (if set to true) printed
// as children.
void ResolvedCast::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  ABSL_DCHECK_LE(fields->size(), 3);  // type, type_annotation_map and parse location

  // Clear the "type" field if present.
  fields->erase(std::remove_if(
                    fields->begin(), fields->end(),
                    [](const DebugStringField& x) { return x.name == "type"; }),
                fields->end());

  if (expr_ != nullptr) {
    // Use empty name to avoid printing "arguments=" with extra indentation.
    fields->emplace_back("", expr_.get(), expr_accessed(),
                         /*column_created_in=*/false);
  }
  if (return_null_on_error_) {
    fields->emplace_back("return_null_on_error", "TRUE",
                         return_null_on_error_accessed(),
                         /*column_created_in=*/false);
  }
  if (extended_cast_ != nullptr) {
    fields->emplace_back("extended_cast", extended_cast_.get(),
                         extended_cast_accessed(), /*column_created_in=*/false);
  }
  if (format_ != nullptr) {
    fields->emplace_back("format", format_.get(), format_accessed(),
                         /*column_created_in=*/false);
  }
  if (time_zone_ != nullptr) {
    fields->emplace_back("time_zone", time_zone_.get(), time_zone_accessed(),
                         /*column_created_in=*/false);
  }
  if (!type_modifiers_.IsEmpty()) {
    fields->emplace_back("type_modifiers", type_modifiers_.DebugString(),
                         type_modifiers_accessed(),
                         /*column_created_in=*/false);
  }
}

std::string ResolvedCast::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat("Cast(", expr_->type()->DebugString(), " -> ",
                      type()->DebugString(), ")");
}

// ResolvedExtendedCastElement gets formatted as
//   ResolvedExtendedCastElement(function=name).
void ResolvedExtendedCastElement::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  ABSL_DCHECK_LE(fields->size(), 1);  // function
}

void ResolvedCast::set_type_parameters(const TypeParameters& v) {
  type_modifiers_ =
      TypeModifiers::MakeTypeModifiers(v, type_modifiers_.release_collation());
}

std::string ResolvedExtendedCastElement::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat(
      "ResolvedExtendedCastElement(", from_type_->DebugString(), " -> ",
      to_type_->DebugString(), ", function",
      function_ != nullptr ? function_->DebugString() : "<unknown>", ")");
}

// ResolvedMakeProtoField gets formatted as
//   <field>[(format=TIMESTAMP_MILLIS)] := <expr>
// with <expr>'s children printed as its own children.  The required proto
// format is shown in parentheses when present.
// <expr> is normally just a ResolvedColumnRef, but could be a cast expr.
void ResolvedMakeProtoField::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  CollectDebugStringFieldsWithNameFormat(expr_.get(), fields);
}

std::string ResolvedMakeProtoField::GetNameForDebugString(
    const DebugStringConfig& config) const {
  // If the MakeProtoFieldNode has any modifiers present, add them
  // in parentheses on the field name.
  std::string name;
  if (field_descriptor_->is_extension()) {
    absl::StrAppend(&name, "[", field_descriptor_->full_name(), "]");
  } else {
    absl::StrAppend(&name, field_descriptor_->name());
  }

  std::vector<std::string> modifiers;
  if (format() != FieldFormat::DEFAULT_FORMAT) {
    modifiers.push_back(
        absl::StrCat("format=", FieldFormat_Format_Name(format_)));
  }

  if (!modifiers.empty()) {
    absl::StrAppend(&name, "(", absl::StrJoin(modifiers, ","), ")");
  }
  return GetNameForDebugStringWithNameFormat(name, expr_.get());
}

// ResolvedOption gets formatted as
//   [<qualifier>.]<name> := <value>
// if no parse location is available and the assignment operator is "=".
// Otherwise, it is formatted as
//   [<qualifier>.]<name><assignment_operator>
//   [+-parse_location=<location>]
//   +-<value>
void ResolvedOption::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  if (fields->empty() && assignment_op_ == DEFAULT_ASSIGN) {
    CollectDebugStringFieldsWithNameFormat(value_.get(), fields);
  } else {
    fields->emplace_back("", value_.get(), value_accessed(),
                         /*column_created_in=*/false);
  }
}

std::string ResolvedOption::GetNameForDebugString(
    const DebugStringConfig& config) const {
  const std::string prefix = absl::StrCat(
      qualifier_.empty() ? ""
                          : absl::StrCat(ToIdentifierLiteral(qualifier_), "."),
      ToIdentifierLiteral(name_));
  if (GetParseLocationRangeOrNULL() == nullptr &&
      assignment_op_ == ResolvedOption::DEFAULT_ASSIGN) {
    return GetNameForDebugStringWithNameFormat(prefix, value_.get());
  }
  absl::string_view assignment_op_string;
  switch (assignment_op_) {
    case ResolvedOption::ADD_ASSIGN:
      assignment_op_string = "+=";
      break;
    case ResolvedOption::SUB_ASSIGN:
      assignment_op_string = "-=";
      break;
    default:
      assignment_op_string = "=";
  }
  return absl::StrCat(prefix, assignment_op_string);
}

std::string ResolvedWindowFrame::FrameUnitToString(FrameUnit frame_unit) {
  switch (frame_unit) {
    case ResolvedWindowFrame::ROWS:
      return "ROWS";
    case ResolvedWindowFrame::RANGE:
      return "RANGE";
    default:
      ABSL_LOG(ERROR) << "Invalid frame unit: " << frame_unit;
      return absl::StrCat("INVALID_FRAME_UNIT(", frame_unit, ")");
  }
}

std::string ResolvedWindowFrameExpr::BoundaryTypeToString(
    BoundaryType boundary_type) {
  switch (boundary_type) {
    case ResolvedWindowFrameExpr::UNBOUNDED_PRECEDING:
      return "UNBOUNDED PRECEDING";
    case ResolvedWindowFrameExpr::OFFSET_PRECEDING:
      return "OFFSET PRECEDING";
    case ResolvedWindowFrameExpr::CURRENT_ROW:
      return "CURRENT ROW";
    case ResolvedWindowFrameExpr::OFFSET_FOLLOWING:
      return "OFFSET FOLLOWING";
    case ResolvedWindowFrameExpr::UNBOUNDED_FOLLOWING:
      return "UNBOUNDED FOLLOWING";
    default:
      ABSL_LOG(ERROR) << "Invalid boundary Type: " << boundary_type;
      return absl::StrCat("INVALID_BOUNDARY_TYPE(", boundary_type, ")");
  }
}

void ResolvedWindowFrame::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  fields->emplace_back("start_expr", start_expr_.get(), start_expr_accessed(),
                       /*column_created_in=*/false);
  fields->emplace_back("end_expr", end_expr_.get(), end_expr_accessed(),
                       /*column_created_in=*/false);
}

std::string ResolvedWindowFrame::GetFrameUnitString() const {
  return FrameUnitToString(frame_unit_);
}

std::string ResolvedWindowFrame::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat(node_kind_string(), "(frame_unit=", GetFrameUnitString(),
                      ")");
}

void ResolvedWindowFrameExpr::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);
  if (expression_ != nullptr) {
    // Use empty name to avoid printing "expression=" with extra indentation.
    fields->emplace_back("", expression_.get(), expression_accessed(),
                         /*column_created_in=*/false);
  }
}

std::string ResolvedWindowFrameExpr::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return absl::StrCat(node_kind_string(),
                      "(boundary_type=", GetBoundaryTypeString(), ")");
}

std::string ResolvedWindowFrameExpr::GetBoundaryTypeString() const {
  return BoundaryTypeToString(boundary_type_);
}

std::string ResolvedInsertStmt::InsertModeToString(InsertMode insert_mode) {
  switch (insert_mode) {
    case ResolvedInsertStmt::OR_ERROR:
      return "OR ERROR";
    case ResolvedInsertStmt::OR_IGNORE:
      return "OR IGNORE";
    case ResolvedInsertStmt::OR_REPLACE:
      return "OR REPLACE";
    case ResolvedInsertStmt::OR_UPDATE:
      return "OR UPDATE";
    default:
      ABSL_LOG(ERROR) << "Invalid insert mode: " << insert_mode;
      return absl::StrCat("INVALID_INSERT_MODE(", insert_mode, ")");
  }
}

std::string ResolvedOnConflictClause::GetConflictActionString() const {
  return ConflictActionToString(conflict_action_);
}

std::string ResolvedOnConflictClause::ConflictActionToString(
    ConflictAction action) {
  switch (action) {
    case ResolvedOnConflictClause::NOTHING:
      return "NOTHING";
    case ResolvedOnConflictClause::UPDATE:
      return "UPDATE";
    default:
      ABSL_LOG(ERROR) << "Invalid on conflict action mode: " << action;
      return absl::StrCat("INVALID_ON_CONFLICT_ACTION(", action, ")");
  }
}

std::string ResolvedInsertStmt::GetInsertModeString() const {
  return InsertModeToString(insert_mode_);
}

std::string ResolvedAggregateHavingModifier::HavingModifierKindToString(
    HavingModifierKind kind) {
  switch (kind) {
    case ResolvedAggregateHavingModifier::MAX:
      return "MAX";
    case ResolvedAggregateHavingModifier::MIN:
      return "MIN";
    default:
      ABSL_LOG(ERROR) << "Invalid having modifier kind: " << kind;
      return absl::StrCat("INVALID_HAVING_MODIFIER_KIND(", kind, ")");
  }
}

std::string ResolvedAggregateHavingModifier::GetHavingModifierKindString()
    const {
  return HavingModifierKindToString(kind_);
}

std::string ResolvedImportStmt::ImportKindToString(ImportKind kind) {
  switch (kind) {
    case MODULE:
      return "MODULE";
    case PROTO:
      return "PROTO";
    default:
      ABSL_LOG(ERROR) << "Invalid import kind: " << kind;
      return absl::StrCat("INVALID_IMPORT_KIND(", kind, ")");
  }
}

std::string ResolvedImportStmt::GetImportKindString() const {
  return ImportKindToString(import_kind_);
}

std::string ResolvedDropIndexStmt::IndexTypeToString(IndexType index_type) {
  switch (index_type) {
    case INDEX_SEARCH:
      return "INDEX_SEARCH";
    case INDEX_VECTOR:
      return "INDEX_VECTOR";
    case INDEX_DEFAULT:
      return "INDEX";
  }
}

std::string ResolvedDropIndexStmt::GetIndexTypeString() const {
  return IndexTypeToString(index_type_);
}

std::string ResolvedAlterIndexStmt::AlterIndexTypeToString(
    ResolvedAlterIndexStmt::AlterIndexType index_type) {
  switch (index_type) {
    case INDEX_SEARCH:
      return "INDEX_SEARCH";
    case INDEX_VECTOR:
      return "INDEX_VECTOR";
    case INDEX_DEFAULT:
      return "INDEX_DEFAULT";
  }
}

std::string ResolvedAlterIndexStmt::GetAlterIndexTypeString() const {
  return AlterIndexTypeToString(index_type_);
}

absl::StatusOr<TypeParameters> ResolvedColumnAnnotations::GetFullTypeParameters(
    const Type* type) const {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  // We need Type* to figure out the size of TypeParameters.child_list since
  // TypeParameters.child_list.size() is not equals to
  // ResolvedColumnAnnotations.child_list.size().
  // ResolvedColumnAnnotations.child_list will shrink the list size while
  // TypeParameters.child_list won't, see their contracts for details.

  // Annotations with child_list describes complex type parameters, (e.g.
  // STRUCT<STRING(10)>). We reconstruct full type parameters recursively.
  if (child_list_size() > 0) {
    std::vector<TypeParameters> child_parameters;
    if (type->IsArray()) {
      GOOGLESQL_RET_CHECK_EQ(child_list_size(), 1);
      GOOGLESQL_ASSIGN_OR_RETURN(TypeParameters child_parameter,
                       child_list(0)->GetFullTypeParameters(
                           type->AsArray()->element_type()));
      child_parameters.push_back(child_parameter);
    } else if (type->IsStruct()) {
      const StructType* struct_type = type->AsStruct();
      GOOGLESQL_RET_CHECK_LE(child_list_size(), struct_type->num_fields());
      // TypeParameters.child_list.size() is the same as number of subfields
      // in the STRUCT, which may be longer than child_list.size().
      child_parameters.resize(struct_type->num_fields());
      for (int field_index = 0; field_index < child_list_size();
           ++field_index) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            child_parameters[field_index],
            child_list(field_index)
                ->GetFullTypeParameters(struct_type->field(field_index).type));
      }
    } else if (type->IsRange()) {
      GOOGLESQL_RET_CHECK_EQ(child_list_size(), 1);
      GOOGLESQL_ASSIGN_OR_RETURN(TypeParameters child_parameter,
                       child_list(0)->GetFullTypeParameters(
                           type->AsRange()->element_type()));
      child_parameters.push_back(child_parameter);
    } else if (type->IsMap()) {
      GOOGLESQL_RET_CHECK_EQ(child_list_size(), 2);
      GOOGLESQL_ASSIGN_OR_RETURN(
          TypeParameters key_parameter,
          child_list(0)->GetFullTypeParameters(type->AsMap()->key_type()));
      child_parameters.push_back(key_parameter);
      GOOGLESQL_ASSIGN_OR_RETURN(
          TypeParameters value_parameter,
          child_list(1)->GetFullTypeParameters(type->AsMap()->value_type()));
      child_parameters.push_back(value_parameter);
    } else {
      GOOGLESQL_RET_CHECK_FAIL() << "ResolvedColumnAnnotations has children, which is "
                          "unexpected for type: "
                       << type->DebugString();
    }

    // If no children has type parameters, return empty type parameters.
    bool has_no_children =
        std::all_of(child_parameters.begin(), child_parameters.end(),
                    std::mem_fn(&TypeParameters::IsEmpty));
    if (has_no_children) {
      return TypeParameters();
    }

    // If children has type parameters, make sure type parameters in root
    // annotation is empty.
    GOOGLESQL_RET_CHECK(type_parameters().IsEmpty())
        << "ResolvedColumnAnnotations can't have both child type parameters "
           "and root type parameters";
    return TypeParameters::MakeTypeParametersWithChildList(child_parameters);
  }

  // Non-empty type_parameters means annotations with simple type
  // parameters. We directly return type_parameters here.
  if (!type_parameters().IsEmpty()) {
    return type_parameters();
  }

  return TypeParameters();
}

// Gets the type parameters for a complex type (STRUCT or ARRAY etc..) as one
// object, by storing type parameters for subfields in
// TypeParameters.child_list instead of ResolvedColumnAnnotations.child_list.
absl::StatusOr<TypeParameters> ResolvedColumnDefinition::GetFullTypeParameters()
    const {
  if (annotations() == nullptr) {
    return TypeParameters();
  }
  return annotations()->GetFullTypeParameters(type());
}

FunctionEnums::Volatility ResolvedCreateFunctionStmt::volatility() const {
  switch (determinism_level()) {
    case ResolvedCreateStatementEnums::DETERMINISM_VOLATILE:
    case ResolvedCreateStatementEnums::DETERMINISM_NOT_DETERMINISTIC:
    case ResolvedCreateStatementEnums::DETERMINISM_UNSPECIFIED:
      return FunctionEnums::VOLATILE;
    case ResolvedCreateStatementEnums::DETERMINISM_DETERMINISTIC:
    case ResolvedCreateStatementEnums::DETERMINISM_IMMUTABLE:
      return FunctionEnums::IMMUTABLE;
    case ResolvedCreateStatementEnums::DETERMINISM_STABLE:
      return FunctionEnums::STABLE;
  }
}

std::string ResolvedGraphLabelNaryExpr::GraphLogicalOpTypeToString(
    GraphLogicalOpType logical_op_type) {
  switch (logical_op_type) {
    case ResolvedGraphLabelNaryExpr::NOT:
      return "NOT";
    case ResolvedGraphLabelNaryExpr::OR:
      return "OR";
    case ResolvedGraphLabelNaryExpr::AND:
      return "AND";
    default:
      ABSL_LOG(ERROR) << "Invalid Logical Type: " << logical_op_type;
      return absl::StrCat("INVALID_LOGICAL_TYPE(", logical_op_type, ")");
  }
}

std::string ResolvedGraphLabelNaryExpr::GetGraphLogicalOpTypeString() const {
  return GraphLogicalOpTypeToString(op_);
}

void ResolvedStaticDescribeScan::CollectDebugStringFields(
    std::vector<DebugStringField>* fields) const {
  SUPER::CollectDebugStringFields(fields);

  if (!describe_text_.empty()) {
    // Show describe_text contents directly, rather than the normal way
    // strings get shown as quoted values on a single line, with
    // escaped newlines.
    fields->emplace_back(/*name=*/"describe_text", describe_text_,
                         describe_text_accessed(), /*column_created_in=*/false);
  }
  if (input_scan_ != nullptr) {
    fields->emplace_back(/*name=*/"input_scan", input_scan_.get(),
                         input_scan_accessed(), /*column_created_in=*/false);
  }
}

std::string ResolvedStaticDescribeScan::GetNameForDebugString(
    const DebugStringConfig& config) const {
  return SUPER::GetNameForDebugString(config);
}

const ResolvedScan* ResolvedPipeIfScan::GetSelectedCaseScan() const {
  if (selected_case() == -1) {
    return input_scan();
  } else {
    return if_case_list()[selected_case()]->subpipeline()->scan();
  }
}

std::vector<ResolvedColumn> ResolvedGraphPathScan::GetColumnsCreated() const {
  std::vector<ResolvedColumn> columns = SUPER::GetColumnsCreated();
  if (path_ != nullptr) {
    columns.push_back(path_->column());
  }
  if (quantifier_ != nullptr) {
    columns.push_back(head_);
    columns.push_back(tail_);
  }
  return columns;
}

std::vector<ResolvedColumn> ResolvedGraphPathScan::GetColumnsReferenced()
    const {
  std::vector<ResolvedColumn> columns = SUPER::GetColumnsCreated();
  if (quantifier_ == nullptr) {
    columns.push_back(head_);
    columns.push_back(tail_);
  }
  return columns;
}

std::vector<ResolvedColumn> ResolvedAuxLoadDataStmt::GetColumnsCreated() const {
  std::vector<ResolvedColumn> columns = SUPER::GetColumnsCreated();
  columns.insert(columns.end(), pseudo_column_list_.begin(),
                 pseudo_column_list_.end());
  // The columns appear to be created in multiple overlapping places.
  // If column_definition_list is present, it gives all created columns.
  // Otherwise, with_partition_columns gives some created columns, and
  // the output_column_list can include those, and also create more.
  if (column_definition_list_.empty()) {
    absl::flat_hash_set<ResolvedColumn> already_created;
    if (with_partition_columns_ != nullptr) {
      for (const auto& column_definition :
           with_partition_columns_->column_definition_list()) {
        already_created.insert(column_definition->column());
      }
    }
    for (const auto& output_column : output_column_list_) {
      if (!already_created.contains(output_column->column())) {
        columns.push_back(output_column->column());
      }
    }
  }
  return columns;
}

std::vector<ResolvedColumn> ResolvedAuxLoadDataStmt::GetColumnsReferenced()
    const {
  std::vector<ResolvedColumn> columns = SUPER::GetColumnsCreated();
  return columns;
}

}  // namespace googlesql
