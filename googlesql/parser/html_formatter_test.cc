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

#include <cstddef>
#include <memory>
#include <string>

#include "googlesql/parser/parser.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

// Counts non-overlapping occurrences of `needle` in `haystack`.
int CountSubstr(absl::string_view haystack, absl::string_view needle) {
  int count = 0;
  for (size_t pos = haystack.find(needle); pos != absl::string_view::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

// Parses `sql` and renders it to HTML, asserting success.
std::string Render(absl::string_view sql,
                   const LanguageOptions& language_options = {}) {
  ParserOptions parser_options(language_options);
  std::unique_ptr<ParserOutput> parser_output;
  GOOGLESQL_QCHECK_OK(ParseStatement(sql, parser_options, &parser_output));
  absl::StatusOr<std::string> html =
      SqlToHtml(sql, parser_output->statement(), language_options);
  GOOGLESQL_QCHECK_OK(html.status());
  return *html;
}

TEST(HtmlFormatterTest, SimpleSelect) {
  std::string html = Render("SELECT 1");
  EXPECT_THAT(html, HasSubstr("<div class=\"formatted-sql\">"));
  EXPECT_THAT(html, HasSubstr("class=\"ast ast-QueryStatement\""));
  EXPECT_THAT(html, HasSubstr("class=\"ast ast-Query\""));
  EXPECT_THAT(html, HasSubstr("class=\"ast ast-IntLiteral\""));
  // Every opening div has a matching closing div.
  EXPECT_EQ(CountSubstr(html, "<div"), CountSubstr(html, "</div>"));
}

TEST(HtmlFormatterTest, PreservesOriginalText) {
  // Original whitespace/newlines must be preserved verbatim.
  std::string sql = "SELECT\n  a,\n  b\nFROM t";
  std::string html = Render(sql);
  EXPECT_THAT(html, HasSubstr("SELECT\n"));
  EXPECT_THAT(html, HasSubstr("FROM "));
}

TEST(HtmlFormatterTest, Subquery) {
  std::string html = Render("SELECT * FROM (SELECT 1 AS x) AS t");
  EXPECT_THAT(html, HasSubstr("ast-TableSubquery"));
}

TEST(HtmlFormatterTest, ExpressionSubquery) {
  std::string html = Render("SELECT (SELECT 1) AS x");
  EXPECT_THAT(html, HasSubstr("ast-ExpressionSubquery"));
}

TEST(HtmlFormatterTest, LineComment) {
  // The comment is between tokens so it falls within the statement's range.
  std::string html = Render("SELECT -- a comment\n  1");
  EXPECT_THAT(html, HasSubstr("<div class=\"sql-comment\">"));
  EXPECT_THAT(html, HasSubstr("a comment"));
}

TEST(HtmlFormatterTest, BlockComment) {
  std::string html = Render("SELECT /* inline */ 1");
  EXPECT_THAT(html, HasSubstr("<div class=\"sql-comment\">"));
  EXPECT_THAT(html, HasSubstr("inline"));
}

TEST(HtmlFormatterTest, PipeQuery) {
  LanguageOptions options;
  options.EnableLanguageFeature(FEATURE_PIPES);
  std::string html = Render("FROM t |> WHERE x > 1", options);
  EXPECT_THAT(html, HasSubstr("ast-PipeWhere"));
}

TEST(HtmlFormatterTest, HtmlEscaping) {
  // The '<', '>', '&', '"' inside a string literal must be escaped in output,
  // and must not be mistaken for tag syntax.
  std::string html = Render("SELECT '<a> & \"b\"' AS x");
  EXPECT_THAT(html, HasSubstr("&lt;a&gt; &amp; &quot;b&quot;"));
  // The literal text must not appear unescaped.
  EXPECT_THAT(html, Not(HasSubstr("<a>")));
}

TEST(HtmlFormatterTest, StatementNotStartingAtOffsetZero) {
  // Leading whitespace means the statement's first AST node is not at offset 0.
  std::string html = Render("   SELECT 1");
  EXPECT_THAT(html, HasSubstr("ast-QueryStatement"));
  // Leading whitespace is outside root and not part of the rendered substring.
  EXPECT_THAT(html, Not(HasSubstr("   <div")));
}

}  // namespace
}  // namespace googlesql
