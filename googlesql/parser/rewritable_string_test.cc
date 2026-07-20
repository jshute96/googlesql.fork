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

#include "googlesql/parser/rewritable_string.h"

#include <string>

#include "googlesql/base/testing/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace {

using ::googlesql_base::testing::IsOkAndHolds;
using ::googlesql_base::testing::StatusIs;
using ::testing::Eq;
using ::testing::HasSubstr;

TEST(RewritableStringTest, EmptyInput) {
  RewritableString rs("");
  EXPECT_THAT(rs.GetFullString(), Eq(""));
}

TEST(RewritableStringTest, IdentityFullString) {
  RewritableString rs("SELECT 1");
  EXPECT_THAT(rs.GetFullString(), Eq("SELECT 1"));
}

TEST(RewritableStringTest, SingleWrapMidString) {
  RewritableString rs("SELECT abc FROM t");
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(7, 10, "[", "]"));
  EXPECT_THAT(rs.GetFullString(), Eq("SELECT [abc] FROM t"));
}

TEST(RewritableStringTest, WrapWholeString) {
  RewritableString rs("abc");
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(0, 3, "<", ">"));
  EXPECT_THAT(rs.GetFullString(), Eq("<abc>"));
}

TEST(RewritableStringTest, NestedWrapsInnerThenOuter) {
  RewritableString rs("SELECT abc FROM t");
  // Inner first (children before parents).
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(7, 10, "(", ")"));
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(0, 17, "<", ">"));
  EXPECT_THAT(rs.GetFullString(), Eq("<SELECT (abc) FROM t>"));
}

TEST(RewritableStringTest, IdenticalRangeDoubleWrap) {
  // Mirrors QueryStatement/Query having the same range: inner wrap then outer
  // wrap over the same [start, end).
  RewritableString rs("abc");
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(0, 3, "i(", ")"));
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(0, 3, "o(", ")"));
  EXPECT_THAT(rs.GetFullString(), Eq("o(i(abc))"));
}

TEST(RewritableStringTest, AdjacentSiblingWraps) {
  RewritableString rs("ab cd");
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(0, 2, "[", "]"));
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(3, 5, "{", "}"));
  EXPECT_THAT(rs.GetFullString(), Eq("[ab] {cd}"));
}

TEST(RewritableStringTest, WrapSplittingFirstAndLastBlocks) {
  RewritableString rs("0123456789");
  // First wrap creates a rewritten block in the middle.
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(4, 6, "(", ")"));
  // Second wrap's boundaries fall inside the original blocks on either side,
  // so they must be split (but never inside the rewritten block).
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(2, 8, "[", "]"));
  EXPECT_THAT(rs.GetFullString(), Eq("01[23(45)67]89"));
}

TEST(RewritableStringTest, GetSubstringOriginalOnly) {
  RewritableString rs("0123456789");
  EXPECT_THAT(rs.GetSubstring(2, 5), IsOkAndHolds("234"));
}

TEST(RewritableStringTest, GetSubstringSpanningRewrittenBlockAtEndpoints) {
  RewritableString rs("0123456789");
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(4, 6, "(", ")"));
  // Endpoints align with the rewritten block boundaries (and original text on
  // the ends), so this is fine.
  EXPECT_THAT(rs.GetSubstring(2, 8), IsOkAndHolds("23(45)67"));
}

TEST(RewritableStringTest, GetSubstringEndpointInsideRewrittenBlockFails) {
  RewritableString rs("0123456789");
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(4, 6, "(", ")"));
  // Endpoint 5 falls strictly inside the rewritten block [4, 6).
  EXPECT_THAT(rs.GetSubstring(2, 5),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("rewritten block")));
  EXPECT_THAT(rs.GetSubstring(5, 9),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("rewritten block")));
}

TEST(RewritableStringTest, WrapBoundaryInsideRewrittenBlockFails) {
  RewritableString rs("0123456789");
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(4, 6, "(", ")"));
  // Boundary 5 falls strictly inside the rewritten block: cannot split it.
  EXPECT_THAT(rs.WrapSubstring(5, 8, "[", "]"),
              StatusIs(absl::StatusCode::kInternal));
}

TEST(RewritableStringTest, WrapInvalidRangeFails) {
  RewritableString rs("abc");
  EXPECT_THAT(rs.WrapSubstring(2, 2, "[", "]"),
              StatusIs(absl::StatusCode::kInternal));  // empty
  EXPECT_THAT(rs.WrapSubstring(2, 1, "[", "]"),
              StatusIs(absl::StatusCode::kInternal));  // inverted
  EXPECT_THAT(rs.WrapSubstring(0, 4, "[", "]"),
              StatusIs(absl::StatusCode::kInternal));  // out of bounds
}

TEST(RewritableStringTest, TransformAppliedToOriginalTextOnly) {
  auto upper = [](absl::string_view s) {
    return absl::AsciiStrToUpper(s);
  };
  RewritableString rs("abc def", upper);
  // Prefix/suffix are not transformed; original text is.
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(0, 3, "<x>", "</x>"));
  EXPECT_THAT(rs.GetFullString(), Eq("<x>ABC</x> DEF"));
}

TEST(RewritableStringTest, TransformAppliedExactlyOnceUnderNesting) {
  // The transform should run on each original char exactly once, even when an
  // outer wrap folds an already-rewritten inner block.
  auto escaper = [](absl::string_view s) {
    std::string out;
    for (char c : s) {
      if (c == '&') {
        out += "&amp;";
      } else {
        out += c;
      }
    }
    return out;
  };
  // Indices: a=0, &=1, b=2, &=3, c=4.
  RewritableString rs("a&b&c", escaper);
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(2, 3, "(", ")"));  // wraps 'b'
  GOOGLESQL_ASSERT_OK(rs.WrapSubstring(0, 5, "[", "]"));
  EXPECT_THAT(rs.GetFullString(), Eq("[a&amp;(b)&amp;c]"));
}

TEST(RewritableStringTest, GetSubstringAppliesTransform) {
  auto upper = [](absl::string_view s) {
    return absl::AsciiStrToUpper(s);
  };
  RewritableString rs("abcdef", upper);
  EXPECT_THAT(rs.GetSubstring(1, 4), IsOkAndHolds("BCD"));
}

}  // namespace
}  // namespace googlesql
