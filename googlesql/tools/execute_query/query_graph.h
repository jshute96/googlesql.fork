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

#ifndef GOOGLESQL_TOOLS_EXECUTE_QUERY_QUERY_GRAPH_H_
#define GOOGLESQL_TOOLS_EXECUTE_QUERY_QUERY_GRAPH_H_

#include <string>
#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/tools/execute_query/viz_node_mapping.h"
#include "absl/container/flat_hash_map.h"

namespace googlesql {

// A structured, engine-neutral description of a query as a node-link graph,
// used by the execute_query visualizer's graph view.  It is the shared
// substrate the client-side renderer consumes (elkjs geometry + our own HTML
// nodes + an SVG edge overlay): one node per "operator" (a `ResolvedScan`),
// dataflow edges between them, and a containment hierarchy of query "boxes".
//
// This is the *operator-mode* model.  Query mode is a client-side collapse of
// the operators that share a container, so it needs no separate emission.
//
// Node ids match the `data-node-id` ("r<n>") tags the textual/HTML panes emit
// for the same scans, so a selection in any pane maps 1:1 onto a graph node
// (cross-view correspondence).  Container ids use a distinct "b<n>" space.
struct QueryGraph {
  // One operator node, drawn as a box.  `id` is "r<n>" (matches the other
  // panes); `kind` is the `ResolvedScan` node-kind string (e.g. "TableScan").
  struct Node {
    std::string id;
    std::string kind;
    std::string container;  // id of the enclosing container box.
  };

  // A directed dataflow edge.  Data flows from `from` (a producer/input) to
  // `to` (the consuming operator); the renderer draws it pointing downward.
  //   kind="pipe":  the linear pipe-input spine (one operator feeding the next).
  //   kind="input": a secondary input (join rhs, set-op input, subquery feeding
  //                 a scan); `label` carries the consuming field's name.
  struct Edge {
    std::string from;
    std::string to;
    std::string kind;
    std::string label;
  };

  // A containment box: a query / subquery boundary that groups the operators
  // drawn inside it.  `parent` is the enclosing container id ("" for the root).
  struct Container {
    std::string id;
    std::string kind;
    std::string parent;
  };

  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<Container> containers;

  // Serializes the graph to a compact JSON object {nodes, edges, containers}
  // for embedding in the page and consumption by the client renderer.
  std::string ToJson() const;
};

// Builds the operator-mode graph for the Resolved AST rooted at `root`.
// `node_ids` is the shared visualizer identity space (the same one the other
// panes use), so emitted node ids ("r<n>") line up across panes.  Scans absent
// from `node_ids` (e.g. ones nested inside expression subqueries, which the
// linear panes also don't surface yet) are skipped.
QueryGraph BuildResolvedAstQueryGraph(const ResolvedNode* root,
                                      const ResolvedNodeIds& node_ids);

}  // namespace googlesql

#endif  // GOOGLESQL_TOOLS_EXECUTE_QUERY_QUERY_GRAPH_H_
