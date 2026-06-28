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

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace googlesql {
namespace {

// The mapping types compare/store the node pointers only (never dereference
// them), so synthetic addresses are sufficient to exercise the logic.
template <typename T>
const T* FakePtr(uintptr_t v) {
  return reinterpret_cast<const T*>(v);
}

TEST(RidTokensTest, SortsAscendingAndDropsNegative) {
  EXPECT_EQ(RidTokens({}), "");
  EXPECT_EQ(RidTokens({3}), "r3");
  EXPECT_EQ(RidTokens({7, 3, 5}), "r3 r5 r7");
  EXPECT_EQ(RidTokens({-1, 2}), "r2");
}

TEST(ResolvedNodeIdsTest, LookupAndAssign) {
  ResolvedNodeIds ids;
  const ResolvedNode* a = FakePtr<ResolvedNode>(0x10);
  const ResolvedNode* b = FakePtr<ResolvedNode>(0x20);

  EXPECT_EQ(ids.Lookup(a), -1);
  EXPECT_FALSE(ids.Contains(a));

  ids.Assign(a, 5);
  EXPECT_EQ(ids.Lookup(a), 5);
  EXPECT_TRUE(ids.Contains(a));
  EXPECT_EQ(ids.size(), 1);

  // Negative ids and nullptr are ignored.
  ids.Assign(b, -1);
  ids.Assign(nullptr, 3);
  EXPECT_EQ(ids.Lookup(b), -1);
  EXPECT_EQ(ids.size(), 1);

  // Later assignment wins.
  ids.Assign(a, 9);
  EXPECT_EQ(ids.Lookup(a), 9);
}

TEST(ResolvedNodeIdsTest, AssignInOrderUsesIndex) {
  std::vector<const ResolvedNode*> order = {
      FakePtr<ResolvedNode>(0x10), FakePtr<ResolvedNode>(0x20),
      FakePtr<ResolvedNode>(0x30)};
  ResolvedNodeIds ids;
  ids.AssignInOrder<const ResolvedNode>(order);
  EXPECT_EQ(ids.Lookup(order[0]), 0);
  EXPECT_EQ(ids.Lookup(order[1]), 1);
  EXPECT_EQ(ids.Lookup(order[2]), 2);
}

TEST(NodeRefMarkersTest, EmitsOwnIdAndCorrespSet) {
  NodeRefMarkers refs("s");
  const ASTNode* op = FakePtr<ASTNode>(0x10);

  // No correspondence -> empty span, no own-id consumed.
  EXPECT_FALSE(refs.Has(op));
  EXPECT_EQ(refs.Emit(op), "");

  // Several markers union into one set (not last-writer-wins).
  refs.Add(op, 3);
  refs.Add(op, 7);
  refs.Add(op, -1);  // ignored
  EXPECT_TRUE(refs.Has(op));
  EXPECT_EQ(refs.Emit(op),
            "<span class=\"ni-ref\" data-node-id=\"s0\" "
            "data-corresp=\"r3 r7\"></span>");
}

TEST(NodeRefMarkersTest, InheritCopiesResultOperatorEdges) {
  NodeRefMarkers refs("s");
  const ASTNode* op = FakePtr<ASTNode>(0x10);
  const ASTNode* query = FakePtr<ASTNode>(0x20);

  refs.Add(op, 4);
  // The container layer borrows its result operator's correspondences.
  refs.Inherit(query, op);
  EXPECT_EQ(refs.Emit(query),
            "<span class=\"ni-ref\" data-node-id=\"s0\" "
            "data-corresp=\"r4\"></span>");
  // Inheriting from a node with no edges is a no-op.
  const ASTNode* empty = FakePtr<ASTNode>(0x30);
  const ASTNode* other = FakePtr<ASTNode>(0x40);
  refs.Inherit(other, empty);
  EXPECT_FALSE(refs.Has(other));
}

TEST(NodeRefMarkersTest, OwnIdsAdvanceOnlyOnNonEmptyEmit) {
  NodeRefMarkers refs("a");
  const ASTNode* x = FakePtr<ASTNode>(0x10);
  const ASTNode* y = FakePtr<ASTNode>(0x20);
  const ASTNode* z = FakePtr<ASTNode>(0x30);
  refs.Add(x, 1);
  refs.Add(z, 2);
  EXPECT_NE(refs.Emit(x).find("data-node-id=\"a0\""), std::string::npos);
  EXPECT_EQ(refs.Emit(y), "");  // no edges -> no own-id consumed
  // z still gets a1 (y did not consume an own-id).
  EXPECT_NE(refs.Emit(z).find("data-node-id=\"a1\""), std::string::npos);
}

TEST(NodeRefMarkersTest, ContainerIdsUseQNamespace) {
  NodeRefMarkers refs("s");
  const ASTNode* c = FakePtr<ASTNode>(0x10);
  refs.AddContainer(c, 5);
  refs.AddContainer(c, 2);
  EXPECT_EQ(refs.Emit(c),
            "<span class=\"ni-ref\" data-node-id=\"s0\" "
            "data-corresp=\"q2 q5\"></span>");
}

TEST(NodeRefMarkersTest, InheritAsContainerCopiesOperatorIdsAsQ) {
  NodeRefMarkers refs("s");
  const ASTNode* op = FakePtr<ASTNode>(0x10);
  const ASTNode* query = FakePtr<ASTNode>(0x20);
  refs.Add(op, 4);  // operator -> r4
  refs.InheritAsContainer(query, op);
  EXPECT_EQ(refs.Emit(query),
            "<span class=\"ni-ref\" data-node-id=\"s0\" "
            "data-corresp=\"q4\"></span>");
}

TEST(NodeRefMarkersTest, MixedNamespacesEmitRThenQ) {
  NodeRefMarkers refs("a");
  const ASTNode* n = FakePtr<ASTNode>(0x10);
  refs.Add(n, 7);
  refs.AddContainer(n, 3);
  EXPECT_EQ(refs.Emit(n),
            "<span class=\"ni-ref\" data-node-id=\"a0\" "
            "data-corresp=\"r7 q3\"></span>");
}

}  // namespace
}  // namespace googlesql
