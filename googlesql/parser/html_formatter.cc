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

#include "googlesql/parser/html_formatter.h"

#include <cstdint>
#include <string>
#include <vector>

#include "googlesql/parser/ast_node.h"
#include "googlesql/parser/parse_tree_visitor.h"
#include "googlesql/parser/rewritable_string.h"
#include "googlesql/parser/visit_result.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/parse_tokens.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace googlesql {

namespace {

// Escapes the characters that are special in HTML text content / attributes.
std::string EscapeHtml(absl::string_view text) {
  return absl::StrReplaceAll(text, {{"&", "&amp;"},
                                    {"<", "&lt;"},
                                    {">", "&gt;"},
                                    {"\"", "&quot;"}});
}

// Post-order traversal visitor that wraps each AST node's source range in a
// <div>. Wrapping in post-order (children before parents) guarantees inner
// blocks already exist before an outer node wraps them.
class HtmlWrapVisitor : public NonRecursiveParseTreeVisitor {
 public:
  HtmlWrapVisitor(RewritableString* rewritable, absl::string_view filename,
                  int64_t root_start, int64_t root_end, int64_t input_size)
      : rewritable_(rewritable),
        filename_(filename),
        root_start_(root_start),
        root_end_(root_end),
        input_size_(input_size) {}

  absl::StatusOr<VisitResult> defaultVisit(const ASTNode* node) override {
    return VisitResult::VisitChildren(node,
                                      [this, node]() { return WrapNode(node); });
  }

  int skipped_nodes() const { return skipped_nodes_; }

 private:
  absl::Status WrapNode(const ASTNode* node) {
    const ParseLocationRange& range = node->location();
    const int64_t start = range.start().GetByteOffset();
    const int64_t end = range.end().GetByteOffset();

    // Skip nodes with invalid/empty ranges, ranges outside the rendered root,
    // or nodes that came from a different source (e.g. macro expansion).
    if (start < 0 || end <= start || end > input_size_ || start < root_start_ ||
        end > root_end_ || range.start().filename() != filename_) {
      ++skipped_nodes_;
      return absl::OkStatus();
    }

    const std::string prefix =
        absl::StrCat("<div class=\"ast ast-", node->GetNodeKindString(), "\">");
    absl::Status status =
        rewritable_->WrapSubstring(start, end, prefix, "</div>");
    if (!status.ok()) {
      // Defensive: a parser-location quirk (overlapping/misaligned ranges) can
      // make a wrap impossible. Skip this node rather than failing the render.
      ++skipped_nodes_;
    }
    return absl::OkStatus();
  }

  RewritableString* rewritable_;
  absl::string_view filename_;
  int64_t root_start_;
  int64_t root_end_;
  int64_t input_size_;
  int skipped_nodes_ = 0;
};

}  // namespace

absl::StatusOr<std::string> SqlToHtml(absl::string_view sql,
                                      const ASTNode* root,
                                      const LanguageOptions& language_options) {
  GOOGLESQL_RET_CHECK_NE(root, nullptr);
  const int64_t input_size = static_cast<int64_t>(sql.size());
  const int64_t root_start = root->start_location().GetByteOffset();
  const int64_t root_end = root->end_location().GetByteOffset();
  GOOGLESQL_RET_CHECK_GE(root_start, 0);
  GOOGLESQL_RET_CHECK_LE(root_start, root_end);
  GOOGLESQL_RET_CHECK_LE(root_end, input_size);

  RewritableString rewritable(sql, &EscapeHtml);

  // First pass: wrap comment tokens, which do not appear in the AST. Comments
  // are wrapped before AST nodes so that they become ordinary (opaque) blocks
  // nested inside any node wraps that follow. A tokenization failure here is
  // non-fatal -- fall back to AST-only wrapping.
  ParseResumeLocation resume =
      ParseResumeLocation::FromStringView(root->start_location().filename(), sql);
  resume.set_byte_position(static_cast<int>(root_start));
  ParseTokenOptions options;
  options.include_comments = true;
  options.language_options = language_options;
  std::vector<ParseToken> tokens;
  if (GetParseTokens(options, &resume, &tokens).ok()) {
    for (const ParseToken& token : tokens) {
      if (token.IsEndOfInput()) {
        break;
      }
      const ParseLocationRange token_range = token.GetLocationRange();
      const int64_t token_start = token_range.start().GetByteOffset();
      const int64_t token_end = token_range.end().GetByteOffset();
      if (token_start >= root_end) {
        break;
      }
      if (!token.IsComment() || token_start < root_start ||
          token_end > root_end || token_end <= token_start) {
        continue;
      }
      // Best-effort: ignore a comment that cannot be wrapped.
      rewritable
          .WrapSubstring(token_start, token_end, "<div class=\"sql-comment\">",
                         "</div>")
          .IgnoreError();
    }
  }

  // Second pass: wrap each AST node's source range, bottom-up.
  HtmlWrapVisitor visitor(&rewritable, root->start_location().filename(),
                          root_start, root_end, input_size);
  GOOGLESQL_RETURN_IF_ERROR(root->TraverseNonRecursive(&visitor));

  GOOGLESQL_ASSIGN_OR_RETURN(std::string body,
                       rewritable.GetSubstring(root_start, root_end));
  return absl::StrCat("<div class=\"formatted-sql\">", body, "</div>");
}

}  // namespace googlesql
