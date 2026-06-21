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

#ifndef GOOGLESQL_PUBLIC_AST_NODE_RESOLVED_INFO_H_
#define GOOGLESQL_PUBLIC_AST_NODE_RESOLVED_INFO_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"

namespace googlesql {

class ASTNode;
class NameList;
class ResolvedScan;
class Table;

// Extra information the resolver learned about an AST node that resolved to a
// ResolvedTableScan (e.g. a table name in a FROM clause or a JOIN).
struct TableScanInfo {
  // The catalog table that the scan reads from.  Never null.
  const Table* table = nullptr;

  // The NameList describing the names (columns and range variables) produced
  // by the table scan.  May be null if it wasn't captured.
  std::shared_ptr<const NameList> output_name_list;

  // The ResolvedScan produced for this AST node (a ResolvedTableScan), or null
  // if it wasn't captured.  Owned by the resolver/AnalyzerOutput; valid for the
  // lifetime of the AnalyzerOutput.  Used by the visualizer to correlate an
  // input-SQL node with its box in the Resolved AST pane.
  const ResolvedScan* scan = nullptr;
};

// Extra information the resolver learned about an AST node that directly
// corresponds to some ResolvedScan.
struct ResolvedScanInfo {
  // True if this AST node is a pipe operator (the `|> operator ...` produced
  // this scan).
  bool is_pipe_operator = false;

  // The NameList describing the names that are in scope after (i.e. produced
  // by) this scan.  May be null, e.g. for terminal pipe operators that do not
  // produce an output table.
  std::shared_ptr<const NameList> output_name_list;

  // The NameList describing the names that were in scope before (i.e. the input
  // to) this scan, where applicable.  May be null when there is no meaningful
  // input.
  std::shared_ptr<const NameList> input_name_list;

  // The ResolvedScan this AST node produced, or null if it wasn't captured.
  // Owned by the resolver/AnalyzerOutput; valid for the lifetime of the
  // AnalyzerOutput.  Used by the visualizer to correlate an input-SQL node with
  // its box in the Resolved AST pane.
  const ResolvedScan* scan = nullptr;
};

// Extra information the resolver learned about an AST node that resolved to a
// ResolvedFunctionCall.
struct FunctionCallInfo {
  // The chosen concrete function signature, including the return type,
  // formatted as a string.  E.g. "SUM(INT64) -> INT64".
  std::string signature;
};

// Extra information the resolver learned about an AST node that is a statement.
struct StatementInfo {
  // For a query statement, the visible output columns as (name, type) pairs.
  // Empty for non-query statements and for value-table output.
  std::vector<std::pair<std::string, std::string>> output_columns;

  // True if the query statement's output is a value table.
  bool is_value_table = false;

  // For value-table output, the value type formatted as a string.
  std::string value_table_type;
};

// Extra information the resolver learned about a particular AST node, beyond
// what is captured in the resolved AST.  This is intended for query visualizer
// and similar tooling.  Which fields are populated depends on the kind of node
// and what the resolver was able to learn about it.
struct ASTNodeResolvedInfo {
  // A short human-readable title for this node, for use as a heading in
  // visualizer tooling. Examples: "Table my.dataset.T", "|> WHERE",
  // "FROM query", "Table subquery", "CTE subquery cte_name". May be empty.
  std::string node_title;

  // Set when this AST node resolved to a ResolvedTableScan.
  std::optional<TableScanInfo> table_scan_info;

  // Set when this AST node directly corresponds to some ResolvedScan.
  std::optional<ResolvedScanInfo> resolved_scan_info;

  // Set when this AST node resolved to a ResolvedFunctionCall.
  std::optional<FunctionCallInfo> function_call_info;

  // Set when this AST node is a statement.
  std::optional<StatementInfo> statement_info;
};

// Maps an AST node to the extra resolution information the resolver learned
// about it.
//
// The ASTNode keys are owned by the parser output; they remain valid only while
// the corresponding ParserOutput (and the AnalyzerOutput that may own it) is
// alive.  The NameLists referenced by the values are kept alive by the shared
// pointers stored here.
using ASTNodeResolvedInfoMap =
    absl::flat_hash_map<const ASTNode*, ASTNodeResolvedInfo>;

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_AST_NODE_RESOLVED_INFO_H_
