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

#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace googlesql {

RewritableString::RewritableString(absl::string_view original,
                                   TransformFn transform)
    : original_(original), transform_(std::move(transform)) {
  if (!original_.empty()) {
    blocks_.emplace(0, Block{.length = static_cast<int64_t>(original_.size()),
                             .is_original = true,
                             .value = ""});
  }
}

std::string RewritableString::Materialize(int64_t start,
                                          const Block& block) const {
  if (!block.is_original) {
    return block.value;
  }
  absl::string_view text = original_.substr(start, block.length);
  return transform_ ? transform_(text) : std::string(text);
}

std::string RewritableString::GetFullString() const {
  std::string result;
  for (const auto& [start, block] : blocks_) {
    absl::StrAppend(&result, Materialize(start, block));
  }
  return result;
}

absl::Status RewritableString::SplitAt(int64_t offset) {
  if (blocks_.empty()) {
    return absl::OkStatus();
  }
  // Find the block that contains `offset`: the last block whose key <= offset.
  auto it = blocks_.upper_bound(offset);
  if (it == blocks_.begin()) {
    // `offset` is before the first block; nothing to split.
    return absl::OkStatus();
  }
  --it;
  const int64_t block_start = it->first;
  const int64_t block_end = block_start + it->second.length;
  if (offset == block_start || offset >= block_end) {
    // Already at a boundary (or past the end of this block).
    return absl::OkStatus();
  }
  // `offset` falls strictly inside this block.
  GOOGLESQL_RET_CHECK(it->second.is_original)
      << "Cannot split rewritten block [" << block_start << ", " << block_end
      << ") at offset " << offset;
  const int64_t left_len = offset - block_start;
  const int64_t right_len = block_end - offset;
  it->second.length = left_len;
  blocks_.emplace(offset, Block{.length = right_len,
                                .is_original = true,
                                .value = ""});
  return absl::OkStatus();
}

absl::Status RewritableString::WrapSubstring(int64_t start, int64_t end,
                                             absl::string_view prefix,
                                             absl::string_view suffix) {
  GOOGLESQL_RET_CHECK_GE(start, 0);
  GOOGLESQL_RET_CHECK_LT(start, end);
  GOOGLESQL_RET_CHECK_LE(end, static_cast<int64_t>(original_.size()));

  GOOGLESQL_RETURN_IF_ERROR(SplitAt(start));
  GOOGLESQL_RETURN_IF_ERROR(SplitAt(end));

  // Collect and concatenate the blocks covering [start, end).
  std::string body;
  auto it = blocks_.lower_bound(start);
  GOOGLESQL_RET_CHECK(it != blocks_.end() && it->first == start)
      << "No block boundary at " << start;
  int64_t pos = start;
  auto first = it;
  while (it != blocks_.end() && it->first < end) {
    GOOGLESQL_RET_CHECK_EQ(it->first, pos) << "Gap in blocks before " << pos;
    absl::StrAppend(&body, Materialize(it->first, it->second));
    pos += it->second.length;
    ++it;
  }
  GOOGLESQL_RET_CHECK_EQ(pos, end)
      << "Blocks do not tile [" << start << ", " << end << ")";

  blocks_.erase(first, it);
  blocks_.emplace(start,
                  Block{.length = end - start,
                        .is_original = false,
                        .value = absl::StrCat(prefix, body, suffix)});
  return absl::OkStatus();
}

absl::StatusOr<std::string> RewritableString::GetSubstring(int64_t start,
                                                           int64_t end) const {
  GOOGLESQL_RET_CHECK_GE(start, 0);
  GOOGLESQL_RET_CHECK_LE(start, end);
  GOOGLESQL_RET_CHECK_LE(end, static_cast<int64_t>(original_.size()));
  if (start == end) {
    return std::string();
  }

  std::string result;
  // Find the block containing `start`.
  auto it = blocks_.upper_bound(start);
  GOOGLESQL_RET_CHECK(it != blocks_.begin());
  --it;
  int64_t pos = start;
  while (pos < end) {
    GOOGLESQL_RET_CHECK(it != blocks_.end());
    const int64_t block_start = it->first;
    const int64_t block_end = block_start + it->second.length;
    if (it->second.is_original) {
      // Original blocks can be sliced at any offset.
      const int64_t slice_end = block_end < end ? block_end : end;
      absl::string_view text =
          original_.substr(pos, slice_end - pos);
      absl::StrAppend(&result, transform_ ? transform_(text)
                                          : std::string(text));
      pos = slice_end;
    } else {
      // Rewritten blocks are opaque: the requested range must include the
      // whole block (both endpoints must align with block boundaries).
      if (pos != block_start) {
        return absl::FailedPreconditionError(absl::StrCat(
            "Substring start ", start, " falls inside rewritten block [",
            block_start, ", ", block_end, ")"));
      }
      if (block_end > end) {
        return absl::FailedPreconditionError(absl::StrCat(
            "Substring end ", end, " falls inside rewritten block [",
            block_start, ", ", block_end, ")"));
      }
      absl::StrAppend(&result, it->second.value);
      pos = block_end;
    }
    ++it;
  }
  return result;
}

}  // namespace googlesql
