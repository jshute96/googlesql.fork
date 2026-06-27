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
  rids_[node].insert(rid);
}

void NodeRefMarkers::Inherit(const ASTNode* node, const ASTNode* from) {
  if (node == nullptr || from == nullptr) return;
  auto it = rids_.find(from);
  if (it == rids_.end()) return;
  // Copy `from`'s set before touching rids_[node]: inserting a new entry can
  // rehash the map and invalidate `it` (which points into the same map).
  const absl::flat_hash_set<int> from_ids = it->second;
  rids_[node].insert(from_ids.begin(), from_ids.end());
}

std::string NodeRefMarkers::Emit(const ASTNode* node) {
  auto it = rids_.find(node);
  if (it == rids_.end() || it->second.empty()) return "";
  return absl::StrCat("<span class=\"ni-ref\" data-node-id=\"", prefix_,
                      counter_++, "\" data-corresp=\"", RidTokens(it->second),
                      "\"></span>");
}

}  // namespace googlesql
