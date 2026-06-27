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

#include "googlesql/tools/execute_query/execute_query_web_handler.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/base/path.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/resolved_ast/sql_builder.h"
#include "googlesql/tools/execute_query/execute_query_tool.h"
#include "googlesql/tools/execute_query/web/embedded_resources.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/random/random.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "file_based_test_driver/file_based_test_driver.h"
#include "file_based_test_driver/run_test_case_result.h"
#include "file_based_test_driver/test_case_options.h"

using testing::Eq;
using testing::HasSubstr;
using testing::Not;
using testing::StartsWith;

namespace googlesql {

namespace {

class FakeQueryWebTemplates : public QueryWebTemplates {
 public:
  FakeQueryWebTemplates(std::string contents, std::string css, std::string body)
      : contents_(std::move(contents)),
        css_(std::move(css)),
        body_(std::move(body)) {}
  ~FakeQueryWebTemplates() override = default;

  const std::string& GetWebPageContents() const override { return contents_; }
  const std::string& GetWebPageCSS() const override { return css_; }
  const std::string& GetWebPageBody() const override { return body_; }
  const std::string& GetVisualizeContent() const override {
    return viz_content_.empty() ? QueryWebTemplates::GetVisualizeContent()
                                : viz_content_;
  }

  // Overrides the /visualize content fragment template (defaults to the real
  // embedded one).
  void SetVisualizeContent(std::string content) {
    viz_content_ = std::move(content);
  }

 private:
  std::string contents_;
  std::string css_;
  std::string body_;
  std::string viz_content_;
};

bool HandleRequest(const ExecuteQueryWebRequest& request,
                   const QueryWebTemplates& templates, std::string& result) {
  ExecuteQueryWebHandler handler(templates);
  return handler.HandleRequest(request, [&result](absl::string_view s) {
    result = s;
    return true;
  });
}

bool HandleVisualizeContent(const ExecuteQueryWebRequest& request,
                            const QueryWebTemplates& templates,
                            std::string& result) {
  ExecuteQueryWebHandler handler(templates);
  return handler.HandleVisualizeContent(request,
                                        [&result](absl::string_view s) {
                                          result = s;
                                          return true;
                                        });
}

}  // namespace

TEST(ExecuteQueryWebHandlerTest, TestCSS) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard, "",
                             "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("CSS: {{css}}", "some_css", ""), result));
  EXPECT_THAT(result, Eq("CSS: some_css"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryPreserved) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard, "foo bar",
                             "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "", "Query: {{query}}"), result));
  EXPECT_THAT(result, Eq("Query: foo bar"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryPreservedForScriptMode) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kScript,
                             SQLBuilder::TargetSyntaxMode::kStandard, "foo bar",
                             "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "", "Query: {{query}}"), result));
  EXPECT_THAT(result, Eq("Query: foo bar"));
}

TEST(ExecuteQueryWebHandlerTest, TestModesPreserved) {
  std::vector<std::string> modes = {"analyze", "explain", "parse"};
  std::string mode_template =
      "{{mode_analyze}}-{{mode_explain}}-{{mode_parse}}";
  std::string result;
  absl::BitGen bitgen;
  for (int i = 0; i < 20; i++) {
    for (int subset_size = 0; subset_size <= modes.size(); subset_size++) {
      std::vector<std::string> subset = modes;
      std::shuffle(subset.begin(), subset.end(), bitgen);
      subset.resize(subset_size);

      std::vector<std::string> expected_v;
      for (const std::string& s : modes) {
        expected_v.push_back(absl::c_linear_search(subset, s) ? "true" : "");
      }
      std::string expected = absl::StrJoin(expected_v, "-");

      EXPECT_TRUE(HandleRequest(
          ExecuteQueryWebRequest(subset, ExecuteQueryConfig::SqlMode::kQuery,
                                 SQLBuilder::TargetSyntaxMode::kStandard,
                                 "foo bar", "none", "MAXIMUM", "ALL_MINUS_DEV"),
          FakeQueryWebTemplates("{{> body}}", "", mode_template), result));
      EXPECT_THAT(result, Eq(expected))
          << "Failed for subset [" << absl::StrJoin(subset, ",") << "]";
    }
  }
}

TEST(ExecuteQueryWebHandlerTest, TestSqlModesPreserved) {
  std::vector<ExecuteQueryConfig::SqlMode> sql_modes = {
      ExecuteQueryConfig::SqlMode::kQuery,
      ExecuteQueryConfig::SqlMode::kExpression,
      ExecuteQueryConfig::SqlMode::kScript};
  std::string sql_mode_template =
      "{{sql_mode_query}}-{{sql_mode_expression}}-{{sql_mode_script}}";
  std::vector<std::string> expected_results = {"true--", "-true-", "--true"};
  std::string result;
  for (int index = 0; index < sql_modes.size(); ++index) {
    EXPECT_TRUE(HandleRequest(
        ExecuteQueryWebRequest(/*ToolMode is tested separately*/ {"execute"},
                               sql_modes[index],
                               SQLBuilder::TargetSyntaxMode::kStandard,
                               "foo bar", "none", "MAXIMUM", "ALL_MINUS_DEV"),
        FakeQueryWebTemplates("{{> body}}", "", sql_mode_template), result));
    EXPECT_THAT(result, Eq(expected_results[index]))
        << "Failed for subset [" << expected_results[index] << "]";
  }
}

TEST(ExecuteQueryWebHandlerTest, TestTargetSyntaxModesPreserved) {
  std::vector<SQLBuilder::TargetSyntaxMode> target_syntax_modes = {
      SQLBuilder::TargetSyntaxMode::kStandard,
      SQLBuilder::TargetSyntaxMode::kPipe};
  std::string target_syntax_mode_template =
      "{{target_syntax_mode_standard}}-{{target_syntax_mode_pipe}}";
  std::vector<std::string> expected_results = {"true-", "-true"};
  std::string result;
  for (int index = 0; index < target_syntax_modes.size(); ++index) {
    EXPECT_TRUE(HandleRequest(
        ExecuteQueryWebRequest(/*ToolMode is tested separately*/ {"execute"},
                               ExecuteQueryConfig::SqlMode::kQuery,
                               target_syntax_modes[index], "foo bar", "none",
                               "MAXIMUM", "ALL_MINUS_DEV"),
        FakeQueryWebTemplates("{{> body}}", "", target_syntax_mode_template),
        result));
    EXPECT_THAT(result, Eq(expected_results[index]))
        << "Failed for subset [" << expected_results[index] << "]";
  }
}

TEST(ExecuteQueryWebHandlerTest, TestQueryEscaped) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({""}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "</select> Exploit!", "none", "MAXIMUM",
                             "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "", "Query: {{query}}"), result));
  EXPECT_THAT(result, Eq("Query: &lt;&#x2F;select&gt; Exploit!"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryResultPresent) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1", "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{result}}-{{error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result, Eq("true-"));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeCrossRefs) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          {"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kStandard,
          "FROM (SELECT 1 AS x) |> WHERE x > 0 |> SELECT x", "none", "MAXIMUM",
          "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}{{#result_visualized}}{{#viz_blocks}}"
                            "[AST]{{{viz_resolved_ast_html}}}"
                            "[IN]{{{viz_input_sql_html}}}"
                            "[SB]{{{viz_sqlbuilder_sql_html}}}"
                            "{{/viz_blocks}}{{/result_visualized}}{{/statements}}"),
      result));
  // The Resolved AST pane emits one box per scan, each with a stable id.
  EXPECT_THAT(result, HasSubstr("class=\"rscan "));
  EXPECT_THAT(result, HasSubstr("data-node-id=\"r0\""));
  // The input pane carries hidden `.ni-ref` markers cross-referencing its boxes
  // to the scans they produced (input SQL <-> Resolved AST correspondence).
  EXPECT_THAT(result, HasSubstr("ni-ref"));
  EXPECT_THAT(result, HasSubstr("data-corresp=\"r"));
  // The SQLBuilder pane is laid out by the same box formatter as the input pane
  // (so both panes format identically), carrying hidden `.ni-ref` markers that
  // cross-reference each regenerated operator to the scan that produced it.
  EXPECT_THAT(result, HasSubstr("[SB]"));
  std::string sb = result.substr(result.find("[SB]"));
  EXPECT_THAT(sb, HasSubstr("class=\"ast ast-"));
  EXPECT_THAT(sb, HasSubstr("data-node-id=\"s"));
  EXPECT_THAT(sb, HasSubstr("data-corresp=\"r"));
  EXPECT_THAT(sb, HasSubstr("|&gt; WHERE"));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeJoinPipeCorrespondence) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          {"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kPipe,
          "FROM (SELECT 1 AS a), (SELECT 2 AS b), (SELECT 3 AS c) "
          "|> WHERE a > 0 "
          "|> AGGREGATE sum(b) AS s GROUP BY c "
          "|> ORDER BY s "
          "|> SELECT c, s",
          "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}{{#result_visualized}}{{#viz_blocks}}"
                            "[SB]{{{viz_sqlbuilder_sql_html}}}"
                            "{{/viz_blocks}}{{/result_visualized}}{{/statements}}"),
      result));
  std::string sb = result.substr(result.find("[SB]"));
  // The SQLBuilder pane is laid out by the box formatter (like the input pane);
  // each regenerated pipe operator carries a `.ni-ref` cross-referencing the
  // single scan it came from.  Correspondence is per operator: the user's own
  // pipe operators map 1:1 to FilterScan r8, AggregateScan r9, OrderByScan r10
  // and the final ProjectScan r11 -- NOT to a random nested join scan (the bug
  // this guards against).  The `.ni-title` is the operator's "|> KEYWORD" head.
  EXPECT_THAT(sb, HasSubstr("class=\"ast ast-"));
  EXPECT_THAT(
      sb,
      HasSubstr("data-corresp=\"r8\"></span><div class=\"ni-title\">|&gt; WHERE"));
  EXPECT_THAT(sb, HasSubstr("data-corresp=\"r9\"></span><div "
                           "class=\"ni-title\">|&gt; AGGREGATE"));
  EXPECT_THAT(sb, HasSubstr("data-corresp=\"r10\"></span><div "
                           "class=\"ni-title\">|&gt; ORDER BY"));
  EXPECT_THAT(sb, HasSubstr("data-corresp=\"r11\"></span><div "
                           "class=\"ni-title\">|&gt; SELECT"));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeNonQueryStatement) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          {"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kStandard,
          "CREATE TABLE t AS SELECT 1 AS x", "none", "MAXIMUM",
          "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}{{#result_visualized}}{{#viz_blocks}}"
                            "[V]{{{viz_resolved_ast_html}}}"
                            "{{/viz_blocks}}{{/result_visualized}}{{/statements}}"),
      result));
  // A non-query (DDL) statement is visualized just like a query: the resolved
  // CREATE-statement block contains the query's scan boxes.
  EXPECT_THAT(result, HasSubstr("[V]"));
  EXPECT_THAT(result, HasSubstr("class=\"rscan "));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeScript) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          {"visualize"}, ExecuteQueryConfig::SqlMode::kScript,
          SQLBuilder::TargetSyntaxMode::kStandard,
          "SELECT 111 AS aaa; SELECT 222 AS bbb;", "none", "MAXIMUM",
          "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}{{#result_visualized}}{{#viz_blocks}}"
                            "[V]{{{viz_input_sql_html}}}"
                            "{{/viz_blocks}}{{/result_visualized}}{{/statements}}"),
      result));
  // Each top-level statement of the script is visualized as its own block,
  // scoped to its OWN text: the first block's Input SQL pane shows aaa/111 and
  // not the second statement's bbb/222, and vice versa (not the whole script).
  std::vector<std::string> blocks = absl::StrSplit(result, "[V]");
  ASSERT_EQ(blocks.size(), 3u);  // leading "" + one block per statement
  EXPECT_THAT(blocks[1], HasSubstr("aaa"));
  EXPECT_EQ(blocks[1].find("bbb"), std::string::npos);
  EXPECT_THAT(blocks[2], HasSubstr("bbb"));
  EXPECT_EQ(blocks[2].find("aaa"), std::string::npos);
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeShell) {
  std::string result;
  ExecuteQueryWebHandler handler(QueryWebTemplates::Default());
  EXPECT_TRUE(handler.HandleVisualizeShell([&result](absl::string_view s) {
    result = s;
    return true;
  }));
  // The shell is a full-window page carrying the visualize chrome and the
  // client scripts that fetch and inject the content.
  EXPECT_THAT(result, HasSubstr("data-visualize-page"));
  EXPECT_THAT(result, HasSubstr("GoogleSQL Execute Query - Visualize"));
  EXPECT_THAT(result, HasSubstr("id=\"visualize-root\""));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeContentMultiStatement) {
  std::string result;
  // Uses the real content template + viz_block partial.
  EXPECT_TRUE(HandleVisualizeContent(
      ExecuteQueryWebRequest({"visualize"},
                             ExecuteQueryConfig::SqlMode::kScript,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 111 AS aaa; SELECT 222 AS bbb;", "none",
                             "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("", "", ""), result));
  // Two statements -> a statement dropdown and two statement blocks numbered by
  // their position in the script.
  EXPECT_THAT(result, HasSubstr("id=\"visualize-stmt-select\""));
  EXPECT_THAT(result, HasSubstr("Statement 1"));
  EXPECT_THAT(result, HasSubstr("Statement 2"));
  EXPECT_THAT(result, HasSubstr("data-stmt-index=\"1\""));
  EXPECT_THAT(result, HasSubstr("data-stmt-index=\"2\""));
  // Each renders the three-pane viz block.
  EXPECT_THAT(result, HasSubstr("class=\"viz\" data-viz"));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeContentRewriteToggle) {
  std::string result;
  // TYPEOF is expanded by a rewriter, so the post-rewrite tree differs and a
  // second (post) viz block is produced.
  EXPECT_TRUE(HandleVisualizeContent(
      ExecuteQueryWebRequest({"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT TYPEOF(1) AS t", "none", "MAXIMUM",
                             "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("", "", ""), result));
  EXPECT_THAT(result, HasSubstr("data-has-post=\"1\""));
  EXPECT_THAT(result, HasSubstr("class=\"visualize-block\" data-rewrite=\"pre\""));
  EXPECT_THAT(result,
              HasSubstr("class=\"visualize-block\" data-rewrite=\"post\""));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeContentMarkers) {
  std::string result;
  // A custom content template exercises the data exposed to it: the statement
  // index, the per-block rewrite key, and the multiple/has_viz flags.
  FakeQueryWebTemplates templates("", "", "");
  templates.SetVisualizeContent(
      "has_viz={{#has_viz}}1{{/has_viz}} multiple={{#multiple}}1{{/multiple}} "
      "{{#viz_statements}}[stmt {{index}} post={{#has_post_rewrite}}1{{/"
      "has_post_rewrite}}{{^has_post_rewrite}}0{{/has_post_rewrite}}"
      "{{#viz_blocks}}<{{rewrite_key}}>{{/viz_blocks}}]{{/viz_statements}}");
  EXPECT_TRUE(HandleVisualizeContent(
      ExecuteQueryWebRequest({"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1 AS x", "none", "MAXIMUM", "ALL_MINUS_DEV"),
      templates, result));
  EXPECT_THAT(result, HasSubstr("has_viz=1"));
  EXPECT_THAT(result, HasSubstr("multiple="));  // single statement -> no "1"
  EXPECT_THAT(result, Not(HasSubstr("multiple=1")));
  EXPECT_THAT(result, HasSubstr("[stmt 1 post=0<pre>]"));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeContentError) {
  std::string result;
  EXPECT_TRUE(HandleVisualizeContent(
      ExecuteQueryWebRequest({"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT * FROM no_such_table_xyz", "none",
                             "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("", "", ""), result));
  // A failing analysis surfaces the error in place of viz blocks.
  EXPECT_THAT(result, HasSubstr("class=\"error\""));
  EXPECT_THAT(result, HasSubstr("no_such_table_xyz"));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeNestedPipeSegmentation) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          {"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kStandard,
          "SELECT (SELECT 1) AS a, x FROM (SELECT 1 AS x) UNION ALL SELECT 2, 3",
          "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}{{#result_visualized}}{{#viz_blocks}}"
                            "[SB]{{{viz_sqlbuilder_sql_html}}}"
                            "{{/viz_blocks}}{{/result_visualized}}{{/statements}}"),
      result));
  // The regenerated SQL is a UNION of two parenthesized inputs whose pipe
  // operators are all nested.  Laid out by the box formatter, the nested
  // operators are present and carry correspondence ids.
  std::string sb = result.substr(result.find("[SB]"));
  EXPECT_THAT(sb, HasSubstr("|&gt;"));  // nested pipe operators are present.
  EXPECT_THAT(sb, HasSubstr("class=\"ast ast-"));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizeGraphJson) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          {"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kStandard,
          "FROM (SELECT 1 AS x) |> WHERE x > 0 |> SELECT x", "none", "MAXIMUM",
          "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}{{#result_visualized}}{{#viz_blocks}}"
                            "[G]{{{viz_resolved_graph_json}}}"
                            "{{/viz_blocks}}{{/result_visualized}}{{/statements}}"),
      result));
  // The structured QueryGraph model is embedded as JSON, with the same scan
  // ids (r0..) the Resolved AST pane uses and a pipe-edge spine.
  std::string g = result.substr(result.find("[G]"));
  EXPECT_THAT(g, HasSubstr("\"nodes\":["));
  EXPECT_THAT(g, HasSubstr("\"edges\":["));
  EXPECT_THAT(g, HasSubstr("\"containers\":["));
  EXPECT_THAT(g, HasSubstr("\"id\":\"r0\""));
  EXPECT_THAT(g, HasSubstr("\"kind\":\"pipe\""));
}

TEST(ExecuteQueryWebHandlerTest, TestVisualizePostRewriteSection) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          {"visualize"}, ExecuteQueryConfig::SqlMode::kQuery,
          SQLBuilder::TargetSyntaxMode::kStandard, "SELECT TYPEOF(1) AS t",
          "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}{{#result_visualized}}{{#viz_blocks}}"
                            "{{^viz_title}}[AST]{{/viz_title}}"
                            "{{#viz_title}}[POST]{{/viz_title}}"
                            "{{{viz_resolved_ast_html}}}"
                            "{{/viz_blocks}}{{/result_visualized}}{{/statements}}"),
      result));
  // The rewriter expands typeof() into an if(...$is_null...) form, so the
  // pre-rewrite pane keeps typeof() and a separate post-rewrite section appears
  // with the rewritten form.
  EXPECT_THAT(result, HasSubstr("[POST]"));
  const size_t post_pos = result.find("[POST]");
  EXPECT_THAT(result.substr(0, post_pos), HasSubstr("typeof"));
  const std::string post = result.substr(post_pos);
  EXPECT_THAT(post, HasSubstr("is_null"));
  EXPECT_EQ(post.find("typeof"), std::string::npos);
}

TEST(ExecuteQueryWebHandlerTest, TestQueryExecutedSimpleResult) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "DESCRIBE RAND", "none", "MAXIMUM",
                             "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{#result_executed_tables}}\n"
                            "{{#table}}{{> table}}{{/table}}\n"
                            "{{#error}}error:{{{error}}}{{/error}}\n"
                            "{{/result_executed_tables}}\n"
                            "{{/statements}}"),
      result));
  // We have one test showing the HTML for a table that comes out.
  // If this is too volatile, it can be replaced with a placeholder that
  // just checks some table is emitted.
  EXPECT_THAT(
      result,
      Eq("<table>\n  <thead>\n    <tr>\n        <th>#&zwnj;</th>\n        "
         "<th>Describe&zwnj;</th>\n    </tr>\n  </thead>\n  <tbody>\n      "
         "<tr>\n          <td>1</td>\n          <td>Function RAND\nSignature: "
         "RAND() -&gt; DOUBLE</td>\n      </tr>\n  </tbody>\n</table>\n\n"));
}

TEST(ExecuteQueryWebHandlerTest, TestQueryErrorPresent) {
  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "bad request", "none", "MAXIMUM", "ALL_MINUS_DEV"),
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{result}}-{{error}}"
                            "{{/statements}}"),
      result));
  EXPECT_THAT(result, StartsWith("-"));
  EXPECT_THAT(result, HasSubstr("Syntax error"));
}

TEST(ExecuteQueryWebHandlerTest, TestCatalogUsed) {
  auto web_template =
      FakeQueryWebTemplates("{{> body}}", "",
                            "{{#statements}}"
                            "{{{error}}}"
                            "{{#result_executed_tables}}"
                            "{{#table}}(table){{/table}}"
                            "{{#error}}error:{{{error}}}{{/error}}"
                            "{{/result_executed_tables}}"
                            "{{/statements}}");

  std::string result;
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "DESCRIBE Value", "none", "MAXIMUM",
                             "ALL_MINUS_DEV"),
      web_template, result));
  EXPECT_THAT(result, Eq("INVALID_ARGUMENT: Object not found"));

  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "DESCRIBE Value", "sample", "MAXIMUM",
                             "ALL_MINUS_DEV"),
      web_template, result));
  EXPECT_THAT(result, Eq("(table)"));
}

TEST(ExecuteQueryWebHandlerTest, TestEchoStatement) {
  std::string result;
  const auto web_template = FakeQueryWebTemplates("{{> body}}", "",
                                                  "{{#statements}}"
                                                  "{{#show_statement_text}}"
                                                  "{{statement_text}}"
                                                  "{{/show_statement_text}}"
                                                  "\n---\n"
                                                  "{{/statements}}");

  // With one input statement, show_statement_text isn't set.
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "SELECT 1;", "none", "MAXIMUM", "ALL_MINUS_DEV"),
      web_template, result));
  EXPECT_THAT(result, Eq("\n---\n"));

  // With more than one input statement, show_statement_text is set.
  // Newlines are stripped off the beginning and end of statement_text.
  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest({"execute"}, ExecuteQueryConfig::SqlMode::kQuery,
                             SQLBuilder::TargetSyntaxMode::kStandard,
                             "\n\n\r\nSELECT 1;\n\r\nSELECT\n  2;\n\r\n",
                             "none", "MAXIMUM", "ALL_MINUS_DEV"),
      web_template, result));
  EXPECT_THAT(result, Eq("SELECT 1;\n---\nSELECT\n  2;\n---\n"));
}

static void RunFileBasedTest(
    absl::string_view test_case_input,
    file_based_test_driver::RunTestCaseResult* test_result) {
  file_based_test_driver::TestCaseOptions test_case_options;
  test_case_options.RegisterString("mode", "execute");
  test_case_options.RegisterString("sql_mode", "query");
  test_case_options.RegisterString("target_syntax_mode", "standard");
  test_case_options.RegisterString("catalog", "sample");
  test_case_options.RegisterString("enabled_language_features", "MAXIMUM");
  test_case_options.RegisterString("enabled_ast_rewrites", "ALL_MINUS_DEV");
  std::string input_sql = std::string(test_case_input);
  GOOGLESQL_ASSERT_OK(test_case_options.ParseTestCaseOptions(&input_sql));
  ExecuteQueryConfig config;
  std::string result;
  std::vector<std::string> modes =
      absl::StrSplit(test_case_options.GetString("mode"), ' ');
  FakeQueryWebTemplates web_template(
      "{{> body}}", "",
      "query:{{query}}\n"
      "{{#mode_execute}}mode_execute\n{{/mode_execute}}"
      "{{#mode_analyze}}mode_analyze\n{{/mode_analyze}}"
      "{{#mode_parse}}mode_parse\n{{/mode_parse}}"
      "{{#mode_explain}}mode_explain\n{{/mode_explain}}"
      "{{#mode_unanalyze}}mode_unanalyze\n{{/mode_unanalyze}}"
      "{{#mode_unparse}}mode_unparse\n{{/mode_unparse}}"
      "{{#sql_mode_query}}sql_mode_query\n{{/sql_mode_query}}"
      "{{#sql_mode_expression}}sql_mode_expression\n{{/sql_mode_expression}}"
      "{{#sql_mode_script}}sql_mode_script\n{{/sql_mode_script}}"
      "{{#target_syntax_mode_standard}}target_syntax_mode_standard\n"
      "{{/target_syntax_mode_standard}}"
      "{{#target_syntax_mode_pipe}}target_syntax_mode_pipe\n"
      "{{/target_syntax_mode_pipe}}"
      "{{#statements}}\n"
      "{{#show_statement_text}}\n"
      "{{#statement_text}}\n"
      "statement_text:{{statement_text}}\n"
      "{{/statement_text}}\n"
      "{{/show_statement_text}}\n"
      "{{#error}}\n"
      "error:{{{error}}}\n"
      "{{/error}}\n"
      "{{#result}}\n"
      "{{#result_executed}}\n"
      "{{#result_executed_tables}}\n"
      "{{#table}}\n"
      "table:{{> table}}\n"
      "{{/table}}\n"
      "{{#error}}\n"
      "error:{{{error}}}\n"
      "{{/error}}\n"
      "{{/result_executed_tables}}\n"
      "{{#result_executed_text}}\n"
      "result_executed_text:{{result_executed_text}}\n"
      "{{/result_executed_text}}\n"
      "{{/result_executed}}\n"
      "{{#result_analyzed}}\n"
      "result_analyzed:{{{result_analyzed}}}\n"
      "{{/result_analyzed}}\n"
      "{{#result_formatted_analyzed}}\n"
      "result_formatted_analyzed:{{{result_formatted_analyzed}}}\n"
      "{{/result_formatted_analyzed}}\n"
      "{{#result_parsed}}\n"
      "result_parsed:{{result_parsed}}\n"
      "{{/result_parsed}}\n"
      "{{#result_formatted_sql}}\n"
      "result_formatted_sql:{{{result_formatted_sql}}}\n"
      "result_formatted_sql_boxed:{{{result_formatted_sql_boxed}}}\n"
      "{{/result_formatted_sql}}\n"
      "{{#result_explained }}\n"
      "result_explained:{{result_explained}}\n"
      "{{/result_explained}}\n"
      "{{#result_unanalyzed}}\n"
      "result_unanalyzed:{{result_unanalyzed}}\n"
      "{{/result_unanalyzed}}\n"
      "{{#result_unparsed}}\n"
      "result_unparsed:{{result_unparsed}}\n"
      "{{/result_unparsed}}\n"
      "{{#result_log}}\n"
      "result_log:{{result_log}}\n"
      "{{/result_log}}\n"
      "{{/result}}\n"
      "{{/statements}}");

  EXPECT_TRUE(HandleRequest(
      ExecuteQueryWebRequest(
          modes,
          ExecuteQueryConfig::parse_sql_mode(
              test_case_options.GetString("sql_mode")),
          ExecuteQueryConfig::parse_target_syntax_mode(
              test_case_options.GetString("target_syntax_mode")),
          input_sql, test_case_options.GetString("catalog"),
          test_case_options.GetString("enabled_language_features"),
          test_case_options.GetString("enabled_ast_rewrites")),
      web_template, result));

  test_result->AddTestOutput(result);
}

TEST(ExecuteQueryWebHandlerTest, FileBasedTest) {
  const std::string pattern =
      googlesql_base::JoinPath(::testing::SrcDir(),
                     "_main/googlesql/tools/execute_query/"
                     "testdata/execute_query_web_handler.test");

  EXPECT_TRUE(file_based_test_driver::RunTestCasesFromFiles(pattern,
                                                            &RunFileBasedTest));
}

}  // namespace googlesql
