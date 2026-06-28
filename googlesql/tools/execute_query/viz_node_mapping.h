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

#ifndef GOOGLESQL_TOOLS_EXECUTE_QUERY_VIZ_NODE_MAPPING_H_
#define GOOGLESQL_TOOLS_EXECUTE_QUERY_VIZ_NODE_MAPPING_H_

#include <algorithm>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace googlesql {

class ASTNode;
class ResolvedNode;

// === Visualizer node-mapping framework ===
//
// The execute_query visualizer shows one statement across up to four panes (the
// input SQL, the Resolved AST as text, the Resolved AST as a graph, and the
// regenerated SQLBuilder SQL) and lets a click on a node in one pane highlight
// the corresponding node(s) in the others.  All of that correspondence is
// expressed over a single shared identity space: a dense integer id per
// ResolvedNode, rendered in the HTML as "r<id>".  Each pane projects onto it:
//
//   * The Resolved AST text pane assigns the ids (in emission order) and tags
//     each box with `data-node-id="r<id>"` -- it is the id authority.
//   * Every other pane tags its elements with
//     `data-corresp="r<id> r<id> ..."`: a *set* of ids.  The JS treats
//     correspondence as an undirected one-hop walk over these edges and splits
//     `data-corresp` on whitespace, so a single element may correspond to many
//     resolved nodes and a single resolved node to many elements (many:many).
//
// This header holds the small, pane-independent pieces of that model so the
// rule "every node has an id; each pane records edges to ids" is stated once
// and can be extended -- to more node kinds, and (in a later phase) to ids that
// survive Resolved AST rewrites -- without each pane re-deriving it.  See
// newdocs/execute-query-visualizer.md ("Node-mapping framework") for the full
// design and the planned next tiers (token-range provenance to retire the
// inline SQLBuilder markers; intrinsic, rewrite-stable node ids; the rewriter
// debugger).

// A dense, stable id for a ResolvedNode within one Resolved AST -- the shared
// join key across the panes.  This generalizes the visualizer's former
// scan-only index to *any* ResolvedNode, so non-scan nodes can be made
// correspondable as the framework grows.  Ids are still assigned in Resolved
// AST emission order (so the rendered "r<n>" labels stay stable and
// reading-order-meaningful).
class ResolvedNodeIds {
 public:
  ResolvedNodeIds() = default;

  // Registers `node` with id `id` (ignored when id < 0; later wins).
  void Assign(const ResolvedNode* node, int id);

  // Registers each node with its index as its id (emission order).  Accepts any
  // ResolvedNode subclass pointer span (e.g. `ResolvedScan* const`) -- the
  // upcast to `const ResolvedNode*` happens at the (fully-typed) call site.
  template <typename NodeT>
  void AssignInOrder(absl::Span<NodeT* const> nodes) {
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
      Assign(nodes[i], i);
    }
  }

  // The id for `node`, or -1 if it has none.
  int Lookup(const ResolvedNode* node) const;
  bool Contains(const ResolvedNode* node) const { return Lookup(node) >= 0; }
  int size() const { return static_cast<int>(by_node_.size()); }

 private:
  absl::flat_hash_map<const ResolvedNode*, int> by_node_;
};

// Formats a set of resolved-node ids as a `data-corresp` token string
// ("r3 r7 ...", ascending; "" when empty).  Negative ids are dropped.
std::string RidTokens(const absl::flat_hash_set<int>& ids);

// Accumulates, per parse ASTNode, the resolved-node ids that node corresponds
// to, and emits the hidden ".ni-ref" marker the box formatter can't place on the
// `.rect` directly.  One instance per pane render; `own_id_prefix` is the pane's
// own-id namespace ("a" for the input pane, "s" for the SQLBuilder pane) used
// only so the JS can address each element.
//
// Correspondences live in two id namespaces, both keyed on the same dense
// ResolvedNode id `n`:
//   * "r<n>" -- the *operator/scan* view of a node (its `.rscan` box).
//   * "q<n>" -- the *container* view of a subquery/subpipeline (the
//     `.rscan-query` wrapper that holds it in the Resolved AST pane).
// A subquery's result scan plays both roles (it is the last operator AND the
// whole subquery), so separating the namespaces lets selecting the Subquery
// highlight the *container* field across panes, distinct from selecting its last
// operator.  See newdocs/execute-query-visualizer.md.
//
// Both usage shapes are supported: record-all-then-emit (the SQLBuilder pane
// precomputes every marker's id before laying out boxes) and record-then-emit
// per node (the input pane discovers a node's scan and emits in one pass).
class NodeRefMarkers {
 public:
  explicit NodeRefMarkers(absl::string_view own_id_prefix)
      : prefix_(own_id_prefix) {}

  // Records that `node` corresponds to operator/scan id `rid` ("r<rid>";
  // no-op if rid<0).
  void Add(const ASTNode* node, int rid);

  // Records that `node` corresponds to the *container* view of `rid` ("q<rid>";
  // no-op if rid<0) -- used for subquery/subpipeline layer boxes.
  void AddContainer(const ASTNode* node, int rid);

  // Copies every correspondence recorded for `from` onto `node` (both
  // namespaces; no-op if `from` has none).  Call after `from`'s edges exist.
  void Inherit(const ASTNode* node, const ASTNode* from);

  // Copies `from`'s operator ids ("r<id>") onto `node` as *container* ids
  // ("q<id>").  Used when a query/subpipeline layer borrows its result
  // operator's id but as the container correspondence.
  void InheritAsContainer(const ASTNode* node, const ASTNode* from);

  // Whether `node` has any recorded correspondence.
  bool Has(const ASTNode* node) const;

  // The ".ni-ref" span for `node` -- its own id plus the space-separated
  // cross-references ("r<id>"/"q<id>") -- or "" if `node` has none.  Allocates a
  // fresh own-id each time it emits a non-empty span.
  std::string Emit(const ASTNode* node);

 private:
  // Operator ("r") and container ("q") correspondence ids for one node.
  struct Targets {
    absl::flat_hash_set<int> r;
    absl::flat_hash_set<int> q;
    bool empty() const { return r.empty() && q.empty(); }
  };

  std::string prefix_;
  int counter_ = 0;
  absl::flat_hash_map<const ASTNode*, Targets> targets_;
};

}  // namespace googlesql

#endif  // GOOGLESQL_TOOLS_EXECUTE_QUERY_VIZ_NODE_MAPPING_H_
