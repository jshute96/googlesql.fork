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

#ifndef GOOGLESQL_PARSER_BOX_FORMATTER_H_
#define GOOGLESQL_PARSER_BOX_FORMATTER_H_

#include <functional>
#include <string>

#include "googlesql/parser/ast_node.h"
#include "googlesql/public/language_options.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {

// Optional callback that returns extra HTML to attach to an AST node's region
// (shown as a hover box). Returns "" for nodes with nothing to attach. This
// keeps the parser layer free of analyzer types: the caller (which has the
// resolver info) supplies the rendered HTML. Only nodes that already form a
// region (subqueries, pipe operators, table names) can carry an annotation.
using BoxAnnotator = std::function<std::string(const ASTNode*)>;

// Experimental: renders `sql` as HTML using a computed "box" layout instead of
// the original whitespace. The layout is produced by a width-aware
// pretty-printer (Wadler/Oppen style): each construct is laid out flat on one
// line if it fits within `width` columns, otherwise it is broken across lines
// with indentation. Because the layout is computed as real text (newlines +
// spaces), the result reads correctly top-to-bottom / left-to-right, and the
// AST <div class="ast ast-<NodeKind>"> wrappers become inline decoration over
// that text (render with white-space: pre).
//
// Layout rules implemented:
//   * Pipe operators stack vertically; only "|>" sits at the left margin, the
//     operator name follows on the same line, continuation indents under it.
//   * |> AGGREGATE keeps its list to the right; GROUP BY always drops to its
//     own line aligned with AGGREGATE.
//   * Standard query clauses (SELECT/FROM/WHERE/...) each start a new line.
//   * Comma-separated lists stay inline if they fit, else one item per line,
//     with the comma attached to (inside) the preceding item.
//   * A subquery's contents indent on their own lines when they don't fit
//     inline within parentheses.
//
// `root` must come from a parse of `sql`. `language_options` is used when
// re-tokenizing to find comments. Best-effort: unrecognized nodes fall back to
// an inline rendering of their original text.
//
// When `break_pipe_operators` is true, every pipe operator "|>" starts on its
// own line (the multi-line pipe form) even when the (sub)query would otherwise
// fit on one line -- including pipe operators inside a parenthesized subquery.
// When false (default), a short pipe (sub)query may be laid out inline.
absl::StatusOr<std::string> SqlToBoxHtml(absl::string_view sql,
                                         const ASTNode* root,
                                         const LanguageOptions& language_options,
                                         int width = 80,
                                         BoxAnnotator annotate = nullptr,
                                         bool break_pipe_operators = false);

}  // namespace googlesql

#endif  // GOOGLESQL_PARSER_BOX_FORMATTER_H_
