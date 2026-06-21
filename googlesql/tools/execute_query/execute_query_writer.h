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

#ifndef GOOGLESQL_TOOLS_EXECUTE_QUERY_EXECUTE_QUERY_WRITER_H_
#define GOOGLESQL_TOOLS_EXECUTE_QUERY_EXECUTE_QUERY_WRITER_H_

#include <iosfwd>
#include <memory>
#include <ostream>
#include <string>

#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace googlesql {

// Bundle of the three representations rendered by the "visualize" tool mode:
// the input SQL, the Resolved AST (in linear pipe form), and the
// SQLBuilder-regenerated SQL.  Plain-text fields are used by text output; the
// `_html` fields are used by web output and may be empty if rendering failed.
struct VisualizationData {
  std::string input_sql;
  std::string resolved_ast_text;
  std::string sqlbuilder_sql;

  std::string input_sql_html;
  std::string resolved_ast_html;
  std::string sqlbuilder_sql_html;
};

class ExecuteQueryWriter {
 public:
  virtual ~ExecuteQueryWriter() = default;

  // Write the text of the statement, if desired by the subclass.
  // By default, this is a no-op.
  virtual absl::Status statement_text(absl::string_view statement) {
    return absl::OkStatus();
  }

  // Write textual logging messages. A newline will be added on the end.
  // This can be called multiple times for the same statement.
  virtual absl::Status log(absl::string_view message) {
    return WriteOperationString("log", message);
  }

  virtual absl::Status parsed(absl::string_view parse_debug_string) {
    return WriteOperationString("parsed", parse_debug_string);
  }

  // Experimental HTML renderings of the SQL with the parse tree expressed as
  // nested <div> elements: `original_html` preserves the original text
  // (parser/html_formatter.h); `boxed_html` uses a computed pretty-printed
  // layout (parser/box_formatter.h). This is deliberately not routed through
  // WriteOperationString: it is only meaningful in web mode, and the default is
  // a no-op so other writers ignore it.
  virtual absl::Status formatted_sql_html(absl::string_view original_html,
                                          absl::string_view boxed_html) {
    return absl::OkStatus();
  }

  // Experimental HTML "query viewer" for analyze mode: the box-formatted query
  // (parser/box_formatter.h) with per-AST-node resolver info (input/output
  // NameLists) attached as hover boxes. Web-only; default no-op.
  virtual absl::Status formatted_analyzed_html(absl::string_view html) {
    return absl::OkStatus();
  }
  // Note: This is being abused in some cases to send text directly as output.
  // This doesn't work as expected in web mode.  At most one of those outputs
  // shows up and it goes in the "Unparsed" section.
  virtual absl::Status unparsed(absl::string_view unparse_string) {
    return WriteOperationString("unparsed", unparse_string);
  }

  // This can be called twice per statement.
  // The first time always has `post_rewrite` false.
  // Optionally, if we have post-rewrite output that's different, this
  // can be called again with `post_rewrite` true.
  virtual absl::Status resolved(const ResolvedNode& ast, bool post_rewrite) {
    return absl::UnimplementedError(
        "ExecuteQueryWriter::resolved is not implemented");
  }
  virtual absl::Status unanalyze(absl::string_view unanalyze_string) {
    return WriteOperationString("unanalyze", unanalyze_string);
  }

  // Output for the "visualize" tool mode.  The default (text) rendering prints
  // the three representations as labeled sections; web mode overrides this to
  // populate the side-by-side visualizer panes.
  virtual absl::Status visualized(const VisualizationData& data) {
    return WriteOperationString(
        "visualized",
        absl::StrCat("==== Input SQL ====\n", data.input_sql, "\n\n",
                     "==== Resolved AST (linear) ====\n",
                     data.resolved_ast_text, "\n\n",
                     "==== SQLBuilder SQL ====\n", data.sqlbuilder_sql, "\n"));
  }

  virtual absl::Status explained(const ResolvedNode& ast,
                                 absl::string_view explain) {
    return absl::UnimplementedError(
        "ExecuteQueryWriter::explained is not implemented");
  }

  // This is used for commands that directly produce output as a string.
  virtual absl::Status executed(absl::string_view output) {
    return WriteOperationString("executed", output);
  }
  // This is used for commands that produce a table as output.
  virtual absl::Status executed(const ResolvedNode& ast,
                                std::unique_ptr<EvaluatorTableIterator> iter) {
    return absl::UnimplementedError(
        "ExecuteQueryWriter::executed is not implemented");
  }

  // This is used for commands that produce multiple tables as output.
  virtual absl::Status executed_multi(
      const ResolvedNode& ast,
      std::vector<absl::StatusOr<std::unique_ptr<EvaluatorTableIterator>>>
          results) {
    return absl::UnimplementedError(
        "ExecuteQueryWriter::executed_multi is not implemented");
  }

  virtual absl::Status ExecutedExpression(const ResolvedNode& ast,
                                          const Value& value) {
    return absl::UnimplementedError(
        "ExecuteQueryWriter::executed is not implemented");
  }

  // Called at the start of a statement.  `is_first` is true for the first one.
  virtual absl::Status StartStatement(bool is_first) {
    return absl::OkStatus();
  }

  virtual void FlushStatement(bool at_end, std::string error_msg) = 0;

 protected:
  virtual absl::Status WriteOperationString(absl::string_view operation_name,
                                            absl::string_view str) {
    return absl::UnimplementedError(
        absl::StrCat("ExecuteQueryWriter does not implement ", operation_name));
  }
};

absl::Status PrintResults(std::unique_ptr<EvaluatorTableIterator> iter,
                          std::ostream& out, bool use_box_glyphs = false);

// Writes a human-readable representation of the query result to an output
// stream.
class ExecuteQueryStreamWriter : public ExecuteQueryWriter {
 public:
  explicit ExecuteQueryStreamWriter(std::ostream&, bool use_box_glyphs = false,
                                    bool linear_resolved_ast = false,
                                    bool linear_and_tree_resolved_ast = false);
  ExecuteQueryStreamWriter(const ExecuteQueryStreamWriter&) = delete;
  ExecuteQueryStreamWriter& operator=(const ExecuteQueryStreamWriter&) = delete;

  absl::Status resolved(const ResolvedNode& ast, bool post_rewrite) override;
  absl::Status explained(const ResolvedNode& ast,
                         absl::string_view explain) override;
  absl::Status executed(const ResolvedNode& ast,
                        std::unique_ptr<EvaluatorTableIterator> iter) override;

  absl::Status executed_multi(
      const ResolvedNode& ast,
      std::vector<absl::StatusOr<std::unique_ptr<EvaluatorTableIterator>>>
          results) override;

  absl::Status ExecutedExpression(const ResolvedNode& ast,
                                  const Value& value) override;

  void FlushStatement(bool at_end, std::string error_msg) override;

 protected:
  absl::Status WriteOperationString(absl::string_view operation_name,
                                    absl::string_view str) override {
    stream_ << str << '\n';
    return absl::OkStatus();
  }

 private:
  std::ostream& stream_;
  bool use_box_glyphs_;
  bool linear_resolved_ast_;
  bool linear_and_tree_resolved_ast_;
};

}  // namespace googlesql

#endif  // GOOGLESQL_TOOLS_EXECUTE_QUERY_EXECUTE_QUERY_WRITER_H_
