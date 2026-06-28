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

#include "googlesql/tools/execute_query/viz_node_mapping.h"

#include <algorithm>
#include <string>
#include <vector>

#include "googlesql/parser/parse_tree.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"

namespace googlesql {

void ResolvedNodeIds::Assign(const ResolvedNode* node, int id) {
  if (node == nullptr || id < 0) return;
  by_node_[node] = id;
}

int ResolvedNodeIds::Lookup(const ResolvedNode* node) const {
  auto it = by_node_.find(node);
  return it == by_node_.end() ? -1 : it->second;
}

std::string RidTokens(const absl::flat_hash_set<int>& ids) {
  std::vector<int> sorted;
  sorted.reserve(ids.size());
  for (int id : ids) {
    if (id >= 0) sorted.push_back(id);
  }
  std::sort(sorted.begin(), sorted.end());
  std::string out;
  for (int id : sorted) {
    absl::StrAppend(&out, out.empty() ? "" : " ", "r", id);
  }
  return out;
}

void NodeRefMarkers::Add(const ASTNode* node, int rid) {
  if (node == nullptr || rid < 0) return;
  targets_[node].r.insert(rid);
}

void NodeRefMarkers::AddContainer(const ASTNode* node, int rid) {
  if (node == nullptr || rid < 0) return;
  targets_[node].q.insert(rid);
}

void NodeRefMarkers::AddExpr(const ASTNode* node, int rid) {
  if (node == nullptr || rid < 0) return;
  targets_[node].e.insert(rid);
}

void NodeRefMarkers::Inherit(const ASTNode* node, const ASTNode* from) {
  if (node == nullptr || from == nullptr) return;
  auto it = targets_.find(from);
  if (it == targets_.end()) return;
  // Copy `from`'s sets before touching targets_[node]: inserting a new entry can
  // rehash the map and invalidate `it` (which points into the same map).
  const Targets from_t = it->second;
  Targets& to = targets_[node];
  to.r.insert(from_t.r.begin(), from_t.r.end());
  to.q.insert(from_t.q.begin(), from_t.q.end());
  to.e.insert(from_t.e.begin(), from_t.e.end());
}

void NodeRefMarkers::InheritAsContainer(const ASTNode* node,
                                        const ASTNode* from) {
  if (node == nullptr || from == nullptr) return;
  auto it = targets_.find(from);
  if (it == targets_.end()) return;
  const absl::flat_hash_set<int> from_r = it->second.r;  // copy before rehash
  targets_[node].q.insert(from_r.begin(), from_r.end());
}

bool NodeRefMarkers::Has(const ASTNode* node) const {
  auto it = targets_.find(node);
  return it != targets_.end() && !it->second.empty();
}

// Appends sorted "<prefix><id>" tokens for `ids` to `out` (space-separated).
static void AppendPrefixedTokens(const absl::flat_hash_set<int>& ids,
                                 absl::string_view prefix, std::string* out) {
  std::vector<int> sorted(ids.begin(), ids.end());
  std::sort(sorted.begin(), sorted.end());
  for (int id : sorted) {
    absl::StrAppend(out, out->empty() ? "" : " ", prefix, id);
  }
}

std::string NodeRefMarkers::Emit(const ASTNode* node) {
  auto it = targets_.find(node);
  if (it == targets_.end() || it->second.empty()) return "";
  std::string corresp;
  AppendPrefixedTokens(it->second.r, "r", &corresp);
  AppendPrefixedTokens(it->second.q, "q", &corresp);
  AppendPrefixedTokens(it->second.e, "e", &corresp);
  return absl::StrCat("<span class=\"ni-ref\" data-node-id=\"", prefix_,
                      counter_++, "\" data-corresp=\"", corresp, "\"></span>");
}

}  // namespace googlesql
