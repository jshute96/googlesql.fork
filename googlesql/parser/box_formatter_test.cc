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

#include <memory>
#include <string>

#include "googlesql/parser/parser.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace {

using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Not;

// Strips HTML tags and unescapes entities, leaving the computed text layout
// (the divs are pure decoration over this text).
std::string PlainText(absl::string_view html) {
  std::string out;
  bool in_tag = false;
  for (char c : html) {
    if (c == '<') {
      in_tag = true;
    } else if (c == '>') {
      in_tag = false;
    } else if (!in_tag) {
      out += c;
    }
  }
  return absl::StrReplaceAll(out, {{"&amp;", "&"},
                                   {"&lt;", "<"},
                                   {"&gt;", ">"},
                                   {"&quot;", "\""}});
}

LanguageOptions Pipes() {
  LanguageOptions options;
  options.EnableLanguageFeature(FEATURE_PIPES);
  return options;
}

// Returns the raw HTML rendering.
std::string BoxHtml(absl::string_view sql, int width = 60,
                    const LanguageOptions& options = Pipes()) {
  ParserOptions parser_options(options);
  std::unique_ptr<ParserOutput> parser_output;
  GOOGLESQL_QCHECK_OK(ParseStatement(sql, parser_options, &parser_output));
  absl::StatusOr<std::string> html =
      SqlToBoxHtml(sql, parser_output->statement(), options, width);
  GOOGLESQL_QCHECK_OK(html.status());
  return *html;
}

// Returns the computed text layout (tags stripped).
std::string Box(absl::string_view sql, int width = 60,
                const LanguageOptions& options = Pipes()) {
  return PlainText(BoxHtml(sql, width, options));
}

TEST(BoxFormatterTest, WrappedInBoxedContainer) {
  EXPECT_THAT(BoxHtml("SELECT 1"),
              HasSubstr("<div class=\"formatted-sql boxed\">"));
  EXPECT_THAT(BoxHtml("SELECT 1"), HasSubstr("ast-IntLiteral"));
}

TEST(BoxFormatterTest, ShortQueryStaysInline) {
  EXPECT_THAT(Box("SELECT 1"), Eq("SELECT 1"));
}

TEST(BoxFormatterTest, StandardClausesEachOnOwnLine) {
  EXPECT_THAT(Box("SELECT a, b FROM t WHERE x > 1"),
              Eq("SELECT a, b\nFROM t\nWHERE x > 1"));
}

TEST(BoxFormatterTest, PipeOperatorsStack) {
  EXPECT_THAT(Box("FROM t |> WHERE x > 1 |> SELECT y"),
              Eq("FROM t\n|> WHERE x > 1\n|> SELECT y"));
}

TEST(BoxFormatterTest, OriginalWhitespaceIsDropped) {
  // Messy input whitespace is replaced by the computed layout.
  EXPECT_THAT(Box("SELECT    a  ,   b\n\n  FROM     t"),
              Eq("SELECT a, b\nFROM t"));
}

TEST(BoxFormatterTest, ListBreaksOnePerLineWithCommasAttached) {
  // Narrow width forces the select list to break; each item is on its own line
  // and the comma is attached to the end of the item.
  EXPECT_THAT(Box("SELECT alpha, beta, gamma FROM t", 16),
              Eq("SELECT\n    alpha,\n    beta,\n    gamma\nFROM t"));
}

TEST(BoxFormatterTest, CommaIsInsideTheItemBox) {
  // The comma must live inside the preceding item's div (it "violates" the box
  // model so list items read naturally).
  // The comma closes inside an item box (",</div>"), rather than appearing as
  // its own sibling between item boxes (", <div").
  std::string html = BoxHtml("SELECT alpha, beta FROM t", 16);
  EXPECT_THAT(html, HasSubstr(",</div>"));
  EXPECT_THAT(html, Not(HasSubstr(", <div")));
}

TEST(BoxFormatterTest, PipeContentIndentsUnderOperator) {
  // Continuation of a pipe operator indents four columns (one inside the
  // operator name, which starts at column three after "|> ").
  EXPECT_THAT(Box("FROM t |> WHERE aaaa > 1 AND bbbb < 2", 20),
              Eq("FROM t\n|> WHERE\n    aaaa > 1 AND bbbb < 2"));
}

TEST(BoxFormatterTest, AggregateGroupByAlignsWithAggregate) {
  // GROUP BY always drops to its own line, indented three columns to align with
  // AGGREGATE (which starts after "|> ").
  std::string out = Box(
      "FROM t |> AGGREGATE COUNT(*) AS c GROUP BY x, y", 30);
  EXPECT_THAT(out, HasSubstr("|> AGGREGATE COUNT(*) AS c\n   GROUP BY x, y"));
}

TEST(BoxFormatterTest, SubqueryInlineWhenItFits) {
  EXPECT_THAT(Box("SELECT * FROM (SELECT 1) AS s"),
              Eq("SELECT *\nFROM (SELECT 1) AS s"));
}

TEST(BoxFormatterTest, SubqueryContentsIndentWhenBroken) {
  std::string out = Box("SELECT x FROM t WHERE a IN (SELECT y FROM uuuuuu)", 24);
  EXPECT_THAT(out, HasSubstr("IN (\n        SELECT y\n        FROM uuuuuu\n    )"));
}

TEST(BoxFormatterTest, LineCommentPreserved) {
  std::string out = Box("SELECT 1 -- hello\n|> WHERE x");
  EXPECT_THAT(out, HasSubstr("-- hello"));
}

TEST(BoxFormatterTest, BlockCommentPreserved) {
  std::string out = Box("SELECT /* note */ 1");
  EXPECT_THAT(out, HasSubstr("/* note */"));
}

TEST(BoxFormatterTest, HtmlIsEscaped) {
  EXPECT_THAT(BoxHtml("SELECT '<a>' AS x"), HasSubstr("&lt;a&gt;"));
}

}  // namespace
}  // namespace googlesql
