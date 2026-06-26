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

#ifndef GOOGLESQL_TOOLS_EXECUTE_QUERY_EXECUTE_QUERY_WEB_WRITER_H_
#define GOOGLESQL_TOOLS_EXECUTE_QUERY_EXECUTE_QUERY_WEB_WRITER_H_

#include <memory>
#include <string>
#include <vector>

#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/tools/execute_query/execute_query_writer.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "mstch/mstch.hpp"

namespace googlesql {

// Writer for an ExecuteQuery call that populates template params used for the
// HTML output.
class ExecuteQueryWebWriter : public ExecuteQueryWriter {
 public:
  explicit ExecuteQueryWebWriter(mstch::map &template_params)
      : template_params_(template_params) {
    // The `statements` param is an array of maps, containing the `error`
    // and `result_*` keys for each statmement.
    template_params_["statements"] = mstch::array();
  }

  ExecuteQueryWebWriter(const ExecuteQueryWebWriter &) = delete;
  ExecuteQueryWebWriter &operator=(const ExecuteQueryWebWriter &) = delete;

  absl::Status statement_text(absl::string_view statement) override {
    current_statement_params_["statement_text"] = std::string(statement);
    return absl::OkStatus();
  }

  absl::Status log(absl::string_view message) override {
    absl::StrAppend(&log_messages_, message, "\n");
    current_statement_params_["result_log"] = std::string(log_messages_);
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status parsed(absl::string_view parse_debug_string) override;

  absl::Status formatted_sql_html(absl::string_view original_html,
                                  absl::string_view boxed_html) override {
    current_statement_params_["result_formatted_sql"] =
        std::string(original_html);
    current_statement_params_["result_formatted_sql_boxed"] =
        std::string(boxed_html);
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status formatted_analyzed_html(absl::string_view html) override {
    current_statement_params_["result_formatted_analyzed"] = std::string(html);
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status unparsed(absl::string_view unparse_string) override {
    current_statement_params_["result_unparsed"] = std::string(unparse_string);
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status resolved(const ResolvedNode& ast, bool post_rewrite) override;

  absl::Status unanalyze(absl::string_view unanalyze_string) override {
    current_statement_params_["result_unanalyzed"] =
        std::string(unanalyze_string);
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status visualized(const VisualizationData& data) override {
    current_statement_params_["result_visualized"] = true;
    // One or two full visualizer UIs ("viz blocks"): the pre-rewrite state, plus
    // the post-rewrite state when the rewriters changed the tree.
    auto make_block = [](absl::string_view title, bool no_input_links,
                         const std::string& input_html,
                         const std::string& ast_html,
                         const std::string& sqlbuilder_html,
                         const std::string& graph_json) {
      mstch::map block;
      if (!title.empty()) block["viz_title"] = std::string(title);
      if (no_input_links) block["viz_no_input_links"] = true;
      block["viz_input_sql_html"] = input_html;
      block["viz_resolved_ast_html"] = ast_html;
      block["viz_sqlbuilder_sql_html"] = sqlbuilder_html;
      if (!graph_json.empty()) block["viz_resolved_graph_json"] = graph_json;
      return block;
    };
    mstch::array viz_blocks;
    viz_blocks.push_back(make_block(/*title=*/"", /*no_input_links=*/false,
                                    data.input_sql_html, data.resolved_ast_html,
                                    data.sqlbuilder_sql_html,
                                    data.resolved_graph_json));
    if (!data.post_rewrite_ast_html.empty()) {
      viz_blocks.push_back(make_block(
          "After rewrites", /*no_input_links=*/true,
          data.post_rewrite_input_sql_html, data.post_rewrite_ast_html,
          data.post_rewrite_sqlbuilder_sql_html,
          data.post_rewrite_resolved_graph_json));
    }
    current_statement_params_["viz_blocks"] = viz_blocks;
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status explained(const ResolvedNode &ast,
                         absl::string_view explain) override {
    current_statement_params_["result_explained"] = std::string(explain);
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status executed(absl::string_view output) override {
    current_statement_params_["result_executed"] = true;
    current_statement_params_["result_executed_text"] = std::string(output);
    got_results_ = true;
    return absl::OkStatus();
  }

  absl::Status executed(const ResolvedNode &ast,
                        std::unique_ptr<EvaluatorTableIterator> iter) override;

  absl::Status executed_multi(
      const ResolvedNode& ast,
      std::vector<absl::StatusOr<std::unique_ptr<EvaluatorTableIterator>>>
          results) override;

  absl::Status ExecutedExpression(const ResolvedNode &ast,
                                  const Value &value) override;

  absl::Status StartStatement(bool is_first) override {
    if (!is_first) {
      FlushStatement(/*at_end=*/false);
    }
    return absl::OkStatus();
  }

  void FlushStatement(bool at_end, std::string error_msg = "") override;

  bool GotResults() const { return got_results_; }

 private:

  mstch::map &template_params_;
  mstch::map current_statement_params_;
  mstch::array statement_params_array_;
  std::string log_messages_;
  bool got_results_{false};
};

}  // namespace googlesql

#endif  // GOOGLESQL_TOOLS_EXECUTE_QUERY_EXECUTE_QUERY_WEB_WRITER_H_
