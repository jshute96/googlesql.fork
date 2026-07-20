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

#include <string>
#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace {

// Appends `s` to `out` as a JSON string literal (quotes + minimal escaping).
void AppendJsonString(absl::string_view s, std::string* out) {
  out->push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':
        absl::StrAppend(out, "\\\"");
        break;
      case '\\':
        absl::StrAppend(out, "\\\\");
        break;
      case '\n':
        absl::StrAppend(out, "\\n");
        break;
      case '\r':
        absl::StrAppend(out, "\\r");
        break;
      case '\t':
        absl::StrAppend(out, "\\t");
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          absl::StrAppend(out, absl::StrFormat("\\u%04x", c));
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

// Walks the Resolved AST in the same flattened order the linear panes use,
// assigning each scan a stable "r<n>" node id (looked up from `scan_ids`) and
// recording dataflow edges and containment.  `next_container` hands out fresh
// "b<n>" ids for nested query boxes.
class QueryGraphBuilder {
 public:
  explicit QueryGraphBuilder(
      const absl::flat_hash_map<const ResolvedScan*, int>& scan_ids)
      : scan_ids_(scan_ids) {}

  QueryGraph Build(const ResolvedNode* root) {
    const std::string root_container =
        AddContainer(root->node_kind_string(), /*parent=*/"");
    if (root->IsScan()) {
      AddChain(root->GetAs<ResolvedScan>(), root_container);
    } else {
      // A statement (or other non-scan root): each direct child scan starts a
      // chain inside the root container, mirroring the linear emitter.
      std::vector<const ResolvedNode*> children;
      root->GetChildNodes(&children);
      for (const ResolvedNode* child : children) {
        if (child != nullptr && child->IsScan()) {
          AddChain(child->GetAs<ResolvedScan>(), root_container);
        }
      }
    }
    return std::move(graph_);
  }

 private:
  std::string AddContainer(absl::string_view kind, absl::string_view parent) {
    std::string id = absl::StrCat("b", next_container_++);
    graph_.containers.push_back(
        {.id = id, .kind = std::string(kind), .parent = std::string(parent)});
    return id;
  }

  // Returns the "r<n>" id for `scan`, or "" if it is not in `scan_ids_` (e.g.
  // nested inside an expression subquery, which the linear panes also skip).
  std::string NodeId(const ResolvedScan* scan) const {
    auto it = scan_ids_.find(scan);
    return it == scan_ids_.end() ? "" : absl::StrCat("r", it->second);
  }

  // Processes the pipe-input chain whose output (top) scan is `top`, placing
  // its operators inside container `container`.  Returns the node id of `top`
  // (the chain's output), which an upstream consumer connects to.
  std::string AddChain(const ResolvedScan* top, const std::string& container) {
    // Collect the spine: spine[0] is `top`, spine.back() is the source scan.
    std::vector<const ResolvedScan*> spine;
    for (const ResolvedScan* s = top; s != nullptr;
         s = s->GetPipeInputScan()) {
      spine.push_back(s);
    }

    // Emit source-first (bottom of the dataflow) up to `top`, chaining pipe
    // edges and attaching each scan's secondary inputs.
    std::string prev_id;
    for (int i = static_cast<int>(spine.size()) - 1; i >= 0; --i) {
      const ResolvedScan* s = spine[i];
      const std::string id = NodeId(s);
      if (id.empty()) continue;
      graph_.nodes.push_back(
          {.id = id, .kind = s->node_kind_string(), .container = container});
      if (!prev_id.empty()) {
        graph_.edges.push_back(
            {.from = prev_id, .to = id, .kind = "pipe", .label = ""});
      }
      AddSecondaryInputs(s, id, container);
      prev_id = id;
    }
    return NodeId(top);
  }

  // For scan `s` (node id `id`, in container `container`): each direct child
  // scan other than its pipe input is a secondary input (join rhs, set-op
  // input, subquery).  Each starts its own nested container and feeds `s`.
  void AddSecondaryInputs(const ResolvedScan* s, const std::string& id,
                          const std::string& container) {
    const ResolvedScan* pipe_input = s->GetPipeInputScan();
    std::vector<const ResolvedNode*> children;
    s->GetChildNodes(&children);
    for (const ResolvedNode* child : children) {
      if (child == nullptr || !child->IsScan() || child == pipe_input) {
        continue;
      }
      const ResolvedScan* child_scan = child->GetAs<ResolvedScan>();
      // Label the nested query box by its output (top) scan's kind, so the
      // collapsed query-graph node reads meaningfully (e.g. "JoinScan").
      const std::string child_container =
          AddContainer(child_scan->node_kind_string(), container);
      const std::string child_out = AddChain(child_scan, child_container);
      if (!child_out.empty()) {
        graph_.edges.push_back(
            {.from = child_out, .to = id, .kind = "input", .label = ""});
      }
    }
  }

  const absl::flat_hash_map<const ResolvedScan*, int>& scan_ids_;
  QueryGraph graph_;
  int next_container_ = 0;
};

}  // namespace

std::string QueryGraph::ToJson() const {
  std::string out = "{\"nodes\":[";
  for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
    if (i != 0) out.push_back(',');
    absl::StrAppend(&out, "{\"id\":");
    AppendJsonString(nodes[i].id, &out);
    absl::StrAppend(&out, ",\"kind\":");
    AppendJsonString(nodes[i].kind, &out);
    absl::StrAppend(&out, ",\"container\":");
    AppendJsonString(nodes[i].container, &out);
    out.push_back('}');
  }
  absl::StrAppend(&out, "],\"edges\":[");
  for (int i = 0; i < static_cast<int>(edges.size()); ++i) {
    if (i != 0) out.push_back(',');
    absl::StrAppend(&out, "{\"from\":");
    AppendJsonString(edges[i].from, &out);
    absl::StrAppend(&out, ",\"to\":");
    AppendJsonString(edges[i].to, &out);
    absl::StrAppend(&out, ",\"kind\":");
    AppendJsonString(edges[i].kind, &out);
    absl::StrAppend(&out, ",\"label\":");
    AppendJsonString(edges[i].label, &out);
    out.push_back('}');
  }
  absl::StrAppend(&out, "],\"containers\":[");
  for (int i = 0; i < static_cast<int>(containers.size()); ++i) {
    if (i != 0) out.push_back(',');
    absl::StrAppend(&out, "{\"id\":");
    AppendJsonString(containers[i].id, &out);
    absl::StrAppend(&out, ",\"kind\":");
    AppendJsonString(containers[i].kind, &out);
    absl::StrAppend(&out, ",\"parent\":");
    AppendJsonString(containers[i].parent, &out);
    out.push_back('}');
  }
  absl::StrAppend(&out, "]}");
  return out;
}

QueryGraph BuildResolvedAstQueryGraph(
    const ResolvedNode* root,
    const absl::flat_hash_map<const ResolvedScan*, int>& scan_ids) {
  return QueryGraphBuilder(scan_ids).Build(root);
}

}  // namespace googlesql
