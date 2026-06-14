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

#ifndef GOOGLESQL_PARSER_REWRITABLE_STRING_H_
#define GOOGLESQL_PARSER_REWRITABLE_STRING_H_

#include <cstdint>
#include <functional>
#include <string>

#include "absl/container/btree_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {

// A string that supports inserting wrappers (prefix/suffix text) around ranges
// that are addressed using offsets into the *original* string, even as the
// string is mutated by earlier wraps.
//
// This is the building block for rewriting a SQL string into annotated output
// (e.g. wrapping every AST node's source range in HTML tags). Because every
// operation addresses text by its original byte offset, callers never have to
// track how earlier insertions shifted positions.
//
// The content is represented as an ordered map of blocks keyed by their start
// offset in the original string. Each block is either:
//   * an "original" block, whose value is a slice of the original string (and
//     can therefore be sliced further at any offset), or
//   * a "rewritten" block, whose value has already been transformed/wrapped and
//     is therefore opaque -- it can only be addressed at its endpoints.
//
// The initial state is a single original block covering the whole input.
//
// Optional transform: original-text segments are passed through `transform`
// (if provided) at the moment they are materialized -- either when emitted by
// GetFullString()/GetSubstring() or when folded into a rewritten block by
// WrapSubstring(). This is how HTML-escaping is applied to original text while
// leaving inserted tags untouched. The transform must be pure (it may be
// applied in wrap order rather than text order).
class RewritableString {
 public:
  using TransformFn = std::function<std::string(absl::string_view)>;

  // `original` must outlive this object. `transform` may be null.
  explicit RewritableString(absl::string_view original,
                            TransformFn transform = nullptr);

  // Returns the full current contents, with original text transformed.
  std::string GetFullString() const;

  // Returns the current contents of the original-coordinate range
  // [start, end). It is an error if `start` or `end` falls strictly inside a
  // rewritten block (such a slice has no meaningful value).
  absl::StatusOr<std::string> GetSubstring(int64_t start, int64_t end) const;

  // Wraps the original-coordinate range [start, end) with `prefix` and
  // `suffix`, replacing the covered blocks with a single rewritten block whose
  // value is prefix + <current contents of the range> + suffix. Original blocks
  // at the boundaries are split as needed. It is an error if `start` or `end`
  // falls strictly inside an existing rewritten block, or if the range is empty
  // or out of bounds. `prefix` and `suffix` are inserted verbatim (not
  // transformed).
  absl::Status WrapSubstring(int64_t start, int64_t end,
                             absl::string_view prefix, absl::string_view suffix);

 private:
  struct Block {
    int64_t length;     // Length of this block in original coordinates.
    bool is_original;   // If true, value is original_.substr(<key>, length).
    std::string value;  // Only populated when !is_original (stored post
                        // transform).
  };

  // Ensures a block boundary exists exactly at `offset`, splitting an original
  // block if necessary. No-op if `offset` is already at a boundary or at the
  // very end. Returns an error if `offset` falls strictly inside a rewritten
  // block.
  absl::Status SplitAt(int64_t offset);

  // Returns the materialized (transformed for original text) value of `block`,
  // whose key in the map is `start`.
  std::string Materialize(int64_t start, const Block& block) const;

  absl::string_view original_;
  TransformFn transform_;
  absl::btree_map<int64_t, Block> blocks_;
};

}  // namespace googlesql

#endif  // GOOGLESQL_PARSER_REWRITABLE_STRING_H_
