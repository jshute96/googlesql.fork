//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "googlesql/tools/execute_query/query_graph.h"

#include <memory>
#include <string>
#include <vector>

#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/builtin_function_options.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/status.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"

namespace googlesql {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

class QueryGraphTest : public ::testing::Test {
 protected:
  QueryGraphTest() : catalog_("test_catalog", nullptr) {}

  void SetUp() override {
    catalog_.AddBuiltinFunctions(
        BuiltinFunctionOptions::AllReleasedFunctions());
    catalog_.AddOwnedTable(new SimpleTable(
        "t1", {{"a", type_factory_.get_int64()},
               {"b", type_factory_.get_string()}}));
    catalog_.AddOwnedTable(
        new SimpleTable("t2", {{"a", type_factory_.get_int64()}}));
  }

  // Analyzes `sql`, builds the scan-id map exactly as the visualizer panes do
  // (from the linear HTML emitter's scan order), and returns the graph.
  QueryGraph BuildGraph(absl::string_view sql) {
    GOOGLESQL_CHECK_OK(AnalyzeStatement(sql, options_, &catalog_, &type_factory_,
                                    &output_));
    const ResolvedNode* root = output_->resolved_statement();
    std::vector<const ResolvedScan*> scan_order;
    root->DebugStringHtml(&scan_order);
    scan_ids_.clear();
    for (int i = 0; i < static_cast<int>(scan_order.size()); ++i) {
      scan_ids_[scan_order[i]] = i;
    }
    scan_count_ = static_cast<int>(scan_order.size());
    return BuildResolvedAstQueryGraph(root, scan_ids_);
  }

  AnalyzerOptions options_;
  SimpleCatalog catalog_;
  TypeFactory type_factory_;
  std::unique_ptr<const AnalyzerOutput> output_;
  absl::flat_hash_map<const ResolvedScan*, int> scan_ids_;
  int scan_count_ = 0;
};

// Every reachable scan becomes exactly one node whose id matches the "r<n>"
// id the other panes emit, and the containment/edge endpoints all resolve.
TEST_F(QueryGraphTest, StructuralInvariants) {
  QueryGraph g = BuildGraph("SELECT a, b FROM t1 WHERE a > 0");

  // One node per scan, ids exactly {r0 .. r(N-1)}.
  EXPECT_EQ(g.nodes.size(), scan_count_);
  absl::flat_hash_set<std::string> node_ids;
  for (const QueryGraph::Node& n : g.nodes) {
    EXPECT_TRUE(node_ids.insert(n.id).second) << "duplicate id " << n.id;
  }
  for (int i = 0; i < scan_count_; ++i) {
    EXPECT_THAT(node_ids, Contains(absl::StrCat("r", i)));
  }

  // Containers: exactly one root (empty parent), and every node/container
  // points at a real container.
  absl::flat_hash_set<std::string> container_ids;
  int roots = 0;
  for (const QueryGraph::Container& c : g.containers) {
    container_ids.insert(c.id);
    if (c.parent.empty()) ++roots;
  }
  EXPECT_EQ(roots, 1);
  for (const QueryGraph::Node& n : g.nodes) {
    EXPECT_THAT(container_ids, Contains(n.container));
  }
  for (const QueryGraph::Container& c : g.containers) {
    if (!c.parent.empty()) EXPECT_THAT(container_ids, Contains(c.parent));
  }

  // Edge endpoints all resolve to nodes.
  for (const QueryGraph::Edge& e : g.edges) {
    EXPECT_THAT(node_ids, Contains(e.from));
    EXPECT_THAT(node_ids, Contains(e.to));
  }
}

// The linear emitter numbers scans source-first, so r0 is the leaf source
// scan: nothing feeds into it (no incoming edge), and the chain above it is
// connected by pipe edges.
TEST_F(QueryGraphTest, PipeChainFlowsFromSource) {
  QueryGraph g = BuildGraph("SELECT a FROM t1 WHERE a > 0");
  ASSERT_GT(g.nodes.size(), 1u);

  bool has_pipe = false;
  for (const QueryGraph::Edge& e : g.edges) {
    if (e.kind == "pipe") has_pipe = true;
    EXPECT_NE(e.to, "r0") << "the leaf source scan should have no input";
  }
  EXPECT_TRUE(has_pipe);
}

// A join contributes a secondary ("input") edge in addition to the pipe spine,
// and the right input lives in its own nested container.
TEST_F(QueryGraphTest, JoinHasSecondaryInputEdge) {
  QueryGraph g = BuildGraph("SELECT t1.a FROM t1 JOIN t2 USING (a)");

  bool has_pipe = false;
  bool has_input = false;
  for (const QueryGraph::Edge& e : g.edges) {
    if (e.kind == "pipe") has_pipe = true;
    if (e.kind == "input") has_input = true;
  }
  EXPECT_TRUE(has_pipe);
  EXPECT_TRUE(has_input);

  // More than just the root container (the join's secondary input nests one).
  EXPECT_GT(g.containers.size(), 1u);
}

TEST_F(QueryGraphTest, ToJsonShapeAndEscaping) {
  QueryGraph g;
  g.nodes.push_back({.id = "r0", .kind = "TableScan", .container = "b0"});
  g.edges.push_back(
      {.from = "r1", .to = "r0", .kind = "pipe", .label = "a\"b\\c"});
  g.containers.push_back({.id = "b0", .kind = "stmt", .parent = ""});

  std::string json = g.ToJson();
  EXPECT_THAT(json, HasSubstr("\"nodes\":["));
  EXPECT_THAT(json, HasSubstr("\"edges\":["));
  EXPECT_THAT(json, HasSubstr("\"containers\":["));
  EXPECT_THAT(json, HasSubstr("\"id\":\"r0\""));
  // Special characters in a string value are escaped.
  EXPECT_THAT(json, HasSubstr("\"label\":\"a\\\"b\\\\c\""));
}

}  // namespace
}  // namespace googlesql
