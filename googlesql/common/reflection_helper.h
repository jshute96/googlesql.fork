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

#ifndef GOOGLESQL_COMMON_REFLECTION_HELPER_H_
#define GOOGLESQL_COMMON_REFLECTION_HELPER_H_

#include <string>

#include "googlesql/common/reflection.pb.h"

namespace googlesql {
namespace reflection {

// Format `result_table` into a string.
// This produces the formatted output for pipe DESCRIBE.
// If `include_table_schema` is false, skip the columns and table aliases.
std::string FormatResultTable(const reflection::ResultTable& result_table,
                              bool include_table_schema = true);

// Formats a diff between the `input` and `output` ResultTables as HTML, for
// query visualizer hover boxes.  It walks the two tables in parallel (much like
// `FormatResultTable`), matching columns by (table alias, name) and table
// aliases/CTEs by name.  Added items are wrapped in <span class="nl-add">,
// removed items in <span class="nl-del">.  A column whose type changed is shown
// once with an inline diff on the type.  Within a matched table alias, the
// individual added/removed column names are marked.  Reordering is not detected.
//
// The output is monospace-preformatted: cells are space-padded for alignment
// (intended to render under `white-space: pre`) and line breaks are emitted as
// <br> so the result embeds as a single token.  Cell text is HTML-escaped; the
// span markup is not.  When the two tables are identical the output has no diff
// spans (all text renders normally).
std::string FormatResultTableDiffHtml(const reflection::ResultTable& input,
                                      const reflection::ResultTable& output);

}  // namespace reflection
}  // namespace googlesql

#endif  // GOOGLESQL_COMMON_REFLECTION_HELPER_H_
