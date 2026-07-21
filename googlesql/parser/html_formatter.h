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

#ifndef GOOGLESQL_PARSER_HTML_FORMATTER_H_
#define GOOGLESQL_PARSER_HTML_FORMATTER_H_

#include "googlesql/parser/ast_node.h"
#include "googlesql/public/language_options.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {

// Experimental: renders `sql` as HTML that preserves the original text exactly
// (newlines, indentation, etc.) but wraps each AST node's source range in a
// nested <div class="ast ast-<NodeKind>"> element, and each comment token (which
// does not appear in the AST) in a <div class="sql-comment"> element. The whole
// thing is wrapped in <div class="formatted-sql">.
//
// The idea is to defer all visual formatting to CSS: because the divs form a
// clean hierarchy matching the parse tree, CSS can draw boxes around subqueries,
// pipe operators, etc. Original text is HTML-escaped; inserted tags are not.
//
// Only the byte range covered by `root` is rendered. `root` must be a node from
// a parse of `sql` (its locations are byte offsets into `sql`).
// `language_options` is used when re-tokenizing to find comments.
//
// This is best-effort: nodes whose source ranges are invalid, empty, outside
// `root`, or that come from macro expansion are skipped rather than failing the
// whole render.
absl::StatusOr<std::string> SqlToHtml(absl::string_view sql,
                                      const ASTNode* root,
                                      const LanguageOptions& language_options);

}  // namespace googlesql

#endif  // GOOGLESQL_PARSER_HTML_FORMATTER_H_
