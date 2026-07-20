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

#include "googlesql/common/reflection_helper.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "googlesql/common/reflection.pb.h"
#include "googlesql/public/strings.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "google/protobuf/repeated_ptr_field.h"

namespace googlesql {
namespace reflection {

// Try to limit column lists to this width, but don't add newlines
// inside column names.
static const size_t kMaxColumnListWidth = 80;

// Quote names in the formatted output.
// Empty string is used for missing names and shouldn't be quoted as ``.
// Some special marker names with "<...>" also shouldn't get quoted.
static std::string QuoteName(absl::string_view name) {
  if (name.empty() || name == "<unnamed>" || name == "<value>")
    return std::string(name);
  return ToIdentifierLiteral(name);
}

// Quote names with `QuoteName` and return them as a comma-separated list.
// Add newlines to try to keep width under kMaxColumnListWidth.
static std::string QuoteNameList(
    const google::protobuf::RepeatedPtrField<std::string>& names) {
  std::string output;
  std::string next_line;
  for (int i = 0; i < names.size(); ++i) {
    std::string quoted_name = QuoteName(names[i]);
    if (i != names.size() - 1) {
      absl::StrAppend(&quoted_name, ", ");
    }
    if (!next_line.empty() &&
        next_line.size() + quoted_name.size() >= kMaxColumnListWidth) {
      next_line.pop_back();  // Remove the trailing space.
      absl::StrAppend(&output, next_line, "\n");
      next_line.clear();
    }
    absl::StrAppend(&next_line, quoted_name);
  }
  if (!next_line.empty()) {
    absl::StrAppend(&output, next_line);
  }
  return output;
}

// The absl helper for trimming only works on string_view.
static void StripTrailingSpaces(std::string* str) {
  while (!str->empty() && str->back() == ' ') {
    str->pop_back();
  }
}

// Format the header line above a table, including the newline.
static std::string FormatHeader(absl::string_view header) {
  return absl::StrCat("**", header, "**:\n");
}

// Helper function to format a vector of string into a table, with a header.
// The size of `header` dictates the number of columns to print.
// Missing or extra column values in `rows[k]` are ignored.
// When cell values include newlines, rows are wrapped onto multiple lines.
static std::string FormatTableHeaderAndRows(
    absl::Span<const std::string> header,
    absl::Span<const std::vector<std::string>> rows) {
  std::string output;

  // Wrap rows in multiple lines if cells have newlines.
  // Each row from `rows` becomes one or more in `rows_wrapped`.
  std::vector<std::vector<std::string>> rows_wrapped;
  for (const auto& row : rows) {
    // Split each cell in the row by newline.
    std::vector<std::vector<std::string>> cells_and_lines;
    size_t max_lines = 0;
    for (int i = 0; i < std::min(header.size(), row.size()); ++i) {
      cells_and_lines.push_back(absl::StrSplit(row[i], '\n'));
      max_lines = std::max(max_lines, cells_and_lines.back().size());
    }

    if (max_lines == 1) {
      rows_wrapped.push_back(row);
    } else {
      // Build wrapped rows, padding so we add `max_lines` rows for each cell.
      for (int line_num = 0; line_num < max_lines; ++line_num) {
        std::vector<std::string> partial_row;
        for (const auto& cell_lines : cells_and_lines) {
          if (line_num < cell_lines.size()) {
            partial_row.push_back(cell_lines[line_num]);
          } else {
            partial_row.push_back("");
          }
        }
        rows_wrapped.push_back(partial_row);
      }
    }
  }

  // Calculate column widths based on wrapped rows.
  std::vector<size_t> wrapped_widths(header.size());
  for (int i = 0; i < header.size(); ++i) {
    wrapped_widths[i] = header[i].size();
  }
  for (const auto& row : rows_wrapped) {
    for (int i = 0; i < std::min(header.size(), row.size()); ++i) {
      wrapped_widths[i] = std::max(wrapped_widths[i], row[i].size());
    }
  }

  // Format headers.
  for (int i = 0; i < header.size(); ++i) {
    absl::StrAppend(&output,
                    absl::StrFormat("%-*s  ", wrapped_widths[i], header[i]));
  }
  StripTrailingSpaces(&output);
  absl::StrAppend(&output, "\n");

  // Format separator line.
  for (int i = 0; i < wrapped_widths.size(); ++i) {
    absl::StrAppend(&output,
                    absl::StrFormat("%-*s  ", wrapped_widths[i],
                                    std::string(wrapped_widths[i], '-')));
  }
  StripTrailingSpaces(&output);
  absl::StrAppend(&output, "\n");

  // Print the wrapped rows.
  for (const auto& wrapped_row : rows_wrapped) {
    for (int i = 0; i < std::min(header.size(), wrapped_row.size()); ++i) {
      absl::StrAppend(&output, absl::StrFormat("%-*s  ", wrapped_widths[i],
                                               wrapped_row[i]));
    }
    StripTrailingSpaces(&output);
    absl::StrAppend(&output, "\n");
  }

  return output;
}

std::string FormatResultTable(const reflection::ResultTable& result_table,
                              bool include_table_schema) {
  std::string output;

  // Make the tables for Columns and Pseudo-columns.
  // Only include Table Alias column if any exist.
  bool has_table_aliases = !result_table.table_alias().empty();
  for (const reflection::Column& column : result_table.column()) {
    if (!column.table_alias().empty()) {
      has_table_aliases = true;
      break;
    }
  }

  std::vector<std::string> columns_header = {"Column Name", "Type"};
  if (has_table_aliases) {
    columns_header.insert(columns_header.begin(), "Table Alias");
  }

  auto MakeColumnsRow = [has_table_aliases](const reflection::Column& column) {
    std::vector<std::string> row;
    if (has_table_aliases) {
      row.push_back(QuoteName(column.table_alias()));
    }
    row.push_back(column.column_name().empty()
                      ? "<unnamed>"
                      : QuoteName(column.column_name()));
    row.emplace_back(column.type());
    return row;
  };

  std::vector<std::vector<std::string>> columns_rows;
  for (const reflection::Column& column : result_table.column()) {
    columns_rows.push_back(MakeColumnsRow(column));
  }

  std::vector<std::vector<std::string>> pseudo_columns_rows;
  for (const reflection::Column& pseudo_column : result_table.pseudo_column()) {
    pseudo_columns_rows.push_back(MakeColumnsRow(pseudo_column));
  }

  auto MakeTableAliasRows = [](const auto& table_alias_list,
                               std::vector<std::string>* header) {
    bool has_pseudo_columns = false;
    std::vector<std::vector<std::string>> table_aliases_rows;
    for (const TableAlias& table_alias : table_alias_list) {
      std::vector<std::string> quoted_column_names;
      for (const std::string& column_name : table_alias.column_name()) {
        quoted_column_names.push_back(QuoteName(column_name));
      }
      std::vector<std::string> quoted_pseudo_column_names;
      for (const std::string& column_name : table_alias.pseudo_column_name()) {
        quoted_column_names.push_back(QuoteName(column_name));
        has_pseudo_columns = true;
      }

      table_aliases_rows.push_back(
          {QuoteName(table_alias.name()),
           QuoteNameList(table_alias.column_name()),
           QuoteNameList(table_alias.pseudo_column_name())});
    }
    // Don't print the "Pseudo-columns" column if there aren't any.
    if (!has_pseudo_columns) {
      header->pop_back();
    }
    return table_aliases_rows;
  };

  // Make the table for Table Aliases.
  std::vector<std::string> table_aliases_header = {"Table Alias", "Columns",
                                                   "Pseudo-columns"};
  std::vector<std::vector<std::string>> table_aliases_rows =
      MakeTableAliasRows(result_table.table_alias(), &table_aliases_header);

  if (include_table_schema) {
    if (!result_table.column().empty()) {
      absl::StrAppend(&output, FormatHeader("Columns"),
                      FormatTableHeaderAndRows(columns_header, columns_rows));
    }

    if (!result_table.pseudo_column().empty()) {
      absl::StrAppend(
          &output, "\n", FormatHeader("Pseudo-columns"),
          FormatTableHeaderAndRows(columns_header, pseudo_columns_rows));
    }

    if (has_table_aliases && !result_table.table_alias().empty()) {
      absl::StrAppend(
          &output, "\n", FormatHeader("Table Aliases"),
          FormatTableHeaderAndRows(table_aliases_header, table_aliases_rows));
    }
  }

  if (!result_table.common_table_expression().empty()) {
    // Make the table for Table Aliases.
    std::vector<std::string> header = {"Name", "Columns", "Pseudo-columns"};
    std::vector<std::vector<std::string>> rows =
        MakeTableAliasRows(result_table.common_table_expression(), &header);

    absl::StrAppend(&output, "\n", FormatHeader("Common table expressions"),
                    FormatTableHeaderAndRows(header, rows));
  }

  // Add other description lines below the tables.
  if (result_table.is_value_table() || result_table.is_ordered()) {
    absl::StrAppend(&output, "\n");
    if (include_table_schema && result_table.is_value_table()) {
      absl::StrAppend(&output, "Result is a value table.\n");
    }
    if (result_table.is_ordered()) {
      absl::StrAppend(&output, "Result is ordered.\n");
    }
  }

  return output;
}

namespace {

// HTML-escape `s` for embedding as text content.
std::string EscapeHtml(absl::string_view s) {
  return absl::StrReplaceAll(
      s, {{"&", "&amp;"}, {"<", "&lt;"}, {">", "&gt;"}, {"\"", "&quot;"}});
}

// How a piece of text differs between the input and output NameLists.
enum class Mark { kSame, kAdd, kDel };

// A run of text with a single diff status.
struct Seg {
  std::string text;  // raw (un-escaped) display text; its length is the width
  Mark mark = Mark::kSame;
};
// A table cell is a sequence of segments (so a single cell can show, e.g., an
// old type struck through followed by the new type added).
using Cell = std::vector<Seg>;

// A table row.  If `row_mark` is kAdd/kDel the whole row is highlighted and the
// individual segments are left unmarked; otherwise per-segment marks apply.
struct Row {
  Mark row_mark = Mark::kSame;
  std::vector<Cell> cells;
};

int PlainWidth(const Cell& cell) {
  int width = 0;
  for (const Seg& seg : cell) width += static_cast<int>(seg.text.size());
  return width;
}

std::string WrapMark(Mark mark, absl::string_view html) {
  switch (mark) {
    case Mark::kAdd:
      return absl::StrCat("<span class=\"nl-add\">", html, "</span>");
    case Mark::kDel:
      return absl::StrCat("<span class=\"nl-del\">", html, "</span>");
    case Mark::kSame:
      return std::string(html);
  }
  return std::string(html);
}

std::string RenderCell(const Cell& cell) {
  std::string out;
  for (const Seg& seg : cell) {
    absl::StrAppend(&out, WrapMark(seg.mark, EscapeHtml(seg.text)));
  }
  return out;
}

// A pairing of an input index and an output index.  A -1 means the item is
// absent on that side (i.e. it was deleted or added).
struct Pairing {
  int in_index;
  int out_index;
};

// Classic LCS-based diff over two key sequences.  Returns aligned pairings in
// merged order: matched keys carry both indices, deletions carry only the
// input index, insertions only the output index.
std::vector<Pairing> LcsDiff(absl::Span<const std::string> a,
                             absl::Span<const std::string> b) {
  const int n = static_cast<int>(a.size());
  const int m = static_cast<int>(b.size());
  std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
  for (int i = n - 1; i >= 0; --i) {
    for (int j = m - 1; j >= 0; --j) {
      dp[i][j] = (a[i] == b[j]) ? dp[i + 1][j + 1] + 1
                                : std::max(dp[i + 1][j], dp[i][j + 1]);
    }
  }
  std::vector<Pairing> out;
  int i = 0, j = 0;
  while (i < n && j < m) {
    if (a[i] == b[j]) {
      out.push_back({i, j});
      ++i;
      ++j;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      out.push_back({i, -1});
      ++i;
    } else {
      out.push_back({-1, j});
      ++j;
    }
  }
  while (i < n) out.push_back({i++, -1});
  while (j < m) out.push_back({-1, j++});
  return out;
}

std::string ColumnNameDisplay(const reflection::Column& column) {
  return column.column_name().empty() ? "<unnamed>"
                                      : QuoteName(column.column_name());
}

// Renders one diff line: each cell padded to its column width, with a
// two-space gap, trailing spaces stripped.  A row-level add/delete wraps the
// whole line so the highlight covers it edge to edge.
std::string RenderRow(const Row& row, absl::Span<const int> widths) {
  std::string line;
  for (size_t i = 0; i < widths.size(); ++i) {
    const Cell empty_cell;
    const Cell& cell = i < row.cells.size() ? row.cells[i] : empty_cell;
    const bool plain_row = row.row_mark == Mark::kSame;
    // For a whole-row mark the segments are unmarked text; render them plain so
    // we don't nest spans inside the row wrapper.
    if (plain_row) {
      absl::StrAppend(&line, RenderCell(cell));
    } else {
      std::string text;
      for (const Seg& seg : cell) absl::StrAppend(&text, seg.text);
      absl::StrAppend(&line, EscapeHtml(text));
    }
    int pad = static_cast<int>(widths[i]) - PlainWidth(cell) + 2;
    if (pad < 2) pad = 2;
    absl::StrAppend(&line, std::string(pad, ' '));
  }
  while (!line.empty() && line.back() == ' ') line.pop_back();
  return WrapMark(row.row_mark, line);
}

std::string RenderTable(absl::Span<const std::string> header,
                        absl::Span<const Row> rows) {
  const int ncol = static_cast<int>(header.size());
  std::vector<int> widths(ncol);
  for (int i = 0; i < ncol; ++i) widths[i] = static_cast<int>(header[i].size());
  for (const Row& row : rows) {
    for (int i = 0; i < ncol && i < static_cast<int>(row.cells.size()); ++i) {
      widths[i] = std::max(widths[i], PlainWidth(row.cells[i]));
    }
  }
  std::string out;
  // Header line.
  {
    std::string line;
    for (int i = 0; i < ncol; ++i) {
      absl::StrAppend(&line, EscapeHtml(header[i]),
                      std::string(widths[i] - header[i].size() + 2, ' '));
    }
    StripTrailingSpaces(&line);
    absl::StrAppend(&out, line, "\n");
  }
  // Separator line.
  {
    std::string line;
    for (int i = 0; i < ncol; ++i) {
      absl::StrAppend(&line, std::string(widths[i], '-'), "  ");
    }
    StripTrailingSpaces(&line);
    absl::StrAppend(&out, line, "\n");
  }
  for (const Row& row : rows) {
    absl::StrAppend(&out, RenderRow(row, widths), "\n");
  }
  return out;
}

// Builds the per-item diff of two name lists shown inside one table-alias cell.
Cell DiffNameListCell(
    const google::protobuf::RepeatedPtrField<std::string>& in_names,
    const google::protobuf::RepeatedPtrField<std::string>& out_names) {
  std::vector<std::string> a(in_names.begin(), in_names.end());
  std::vector<std::string> b(out_names.begin(), out_names.end());
  Cell cell;
  bool first = true;
  for (const Pairing& p : LcsDiff(a, b)) {
    if (!first) cell.push_back({", ", Mark::kSame});
    first = false;
    if (p.in_index >= 0 && p.out_index >= 0) {
      cell.push_back({QuoteName(b[p.out_index]), Mark::kSame});
    } else if (p.in_index >= 0) {
      cell.push_back({QuoteName(a[p.in_index]), Mark::kDel});
    } else {
      cell.push_back({QuoteName(b[p.out_index]), Mark::kAdd});
    }
  }
  return cell;
}

Cell PlainNameListCell(
    const google::protobuf::RepeatedPtrField<std::string>& names) {
  Cell cell;
  bool first = true;
  for (const std::string& name : names) {
    if (!first) cell.push_back({", ", Mark::kSame});
    first = false;
    cell.push_back({QuoteName(name), Mark::kSame});
  }
  return cell;
}

bool AnyColumnHasTableAlias(const reflection::ResultTable& table) {
  for (const reflection::Column& column : table.column()) {
    if (!column.table_alias().empty()) return true;
  }
  return !table.table_alias().empty();
}

// Builds the rows for a column-or-pseudo-column section, diffing input vs.
// output.  Columns are matched by (table alias, name); a matched column whose
// type changed shows an inline type diff.
std::vector<Row> DiffColumnRows(
    const google::protobuf::RepeatedPtrField<reflection::Column>& in_columns,
    const google::protobuf::RepeatedPtrField<reflection::Column>& out_columns,
    bool has_table_aliases) {
  auto key = [](const reflection::Column& c) {
    return absl::StrCat(c.table_alias(), "\t", c.column_name());
  };
  std::vector<std::string> in_keys, out_keys;
  for (const auto& c : in_columns) in_keys.push_back(key(c));
  for (const auto& c : out_columns) out_keys.push_back(key(c));

  auto basic_cells = [&](const reflection::Column& c, Cell type_cell) {
    Row row;
    if (has_table_aliases) {
      row.cells.push_back({{QuoteName(c.table_alias()), Mark::kSame}});
    }
    row.cells.push_back({{ColumnNameDisplay(c), Mark::kSame}});
    row.cells.push_back(std::move(type_cell));
    return row;
  };

  std::vector<Row> rows;
  for (const Pairing& p : LcsDiff(in_keys, out_keys)) {
    if (p.in_index >= 0 && p.out_index >= 0) {
      const reflection::Column& ci = in_columns[p.in_index];
      const reflection::Column& co = out_columns[p.out_index];
      Cell type_cell;
      if (ci.type() == co.type()) {
        type_cell.push_back({co.type(), Mark::kSame});
      } else {
        type_cell.push_back({ci.type(), Mark::kDel});
        type_cell.push_back({" ", Mark::kSame});
        type_cell.push_back({co.type(), Mark::kAdd});
      }
      rows.push_back(basic_cells(co, std::move(type_cell)));
    } else if (p.in_index >= 0) {
      const reflection::Column& ci = in_columns[p.in_index];
      Row row = basic_cells(ci, {{ci.type(), Mark::kSame}});
      row.row_mark = Mark::kDel;
      rows.push_back(std::move(row));
    } else {
      const reflection::Column& co = out_columns[p.out_index];
      Row row = basic_cells(co, {{co.type(), Mark::kSame}});
      row.row_mark = Mark::kAdd;
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

// Builds the rows for a table-alias-or-CTE section, diffing input vs. output.
// Aliases are matched by name; matched aliases show a per-item diff of their
// column lists.
std::vector<Row> DiffTableAliasRows(
    const google::protobuf::RepeatedPtrField<reflection::TableAlias>& in_aliases,
    const google::protobuf::RepeatedPtrField<reflection::TableAlias>&
        out_aliases,
    bool include_pseudo) {
  std::vector<std::string> in_keys, out_keys;
  for (const auto& t : in_aliases) in_keys.push_back(t.name());
  for (const auto& t : out_aliases) out_keys.push_back(t.name());

  std::vector<Row> rows;
  for (const Pairing& p : LcsDiff(in_keys, out_keys)) {
    Row row;
    if (p.in_index >= 0 && p.out_index >= 0) {
      const reflection::TableAlias& ti = in_aliases[p.in_index];
      const reflection::TableAlias& to = out_aliases[p.out_index];
      row.cells.push_back({{QuoteName(to.name()), Mark::kSame}});
      row.cells.push_back(
          DiffNameListCell(ti.column_name(), to.column_name()));
      if (include_pseudo) {
        row.cells.push_back(DiffNameListCell(ti.pseudo_column_name(),
                                             to.pseudo_column_name()));
      }
    } else {
      const bool deleted = p.in_index >= 0;
      const reflection::TableAlias& t =
          deleted ? in_aliases[p.in_index] : out_aliases[p.out_index];
      row.row_mark = deleted ? Mark::kDel : Mark::kAdd;
      row.cells.push_back({{QuoteName(t.name()), Mark::kSame}});
      row.cells.push_back(PlainNameListCell(t.column_name()));
      if (include_pseudo) {
        row.cells.push_back(PlainNameListCell(t.pseudo_column_name()));
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

bool HasAnyPseudo(
    const google::protobuf::RepeatedPtrField<reflection::TableAlias>& a,
    const google::protobuf::RepeatedPtrField<reflection::TableAlias>& b) {
  for (const auto& t : a) {
    if (!t.pseudo_column_name().empty()) return true;
  }
  for (const auto& t : b) {
    if (!t.pseudo_column_name().empty()) return true;
  }
  return false;
}

}  // namespace

std::string FormatResultTableDiffHtml(const reflection::ResultTable& input,
                                      const reflection::ResultTable& output) {
  std::string out;
  const bool has_table_aliases =
      AnyColumnHasTableAlias(input) || AnyColumnHasTableAlias(output);

  std::vector<std::string> columns_header = {"Column Name", "Type"};
  if (has_table_aliases) {
    columns_header.insert(columns_header.begin(), "Table Alias");
  }

  if (!input.column().empty() || !output.column().empty()) {
    std::vector<Row> rows =
        DiffColumnRows(input.column(), output.column(), has_table_aliases);
    absl::StrAppend(&out, FormatHeader("Columns"),
                    RenderTable(columns_header, rows));
  }

  if (!input.pseudo_column().empty() || !output.pseudo_column().empty()) {
    std::vector<Row> rows = DiffColumnRows(input.pseudo_column(),
                                           output.pseudo_column(),
                                           has_table_aliases);
    absl::StrAppend(&out, "\n", FormatHeader("Pseudo-columns"),
                    RenderTable(columns_header, rows));
  }

  if (has_table_aliases &&
      (!input.table_alias().empty() || !output.table_alias().empty())) {
    const bool include_pseudo =
        HasAnyPseudo(input.table_alias(), output.table_alias());
    std::vector<std::string> header = {"Table Alias", "Columns",
                                       "Pseudo-columns"};
    if (!include_pseudo) header.pop_back();
    std::vector<Row> rows = DiffTableAliasRows(input.table_alias(),
                                               output.table_alias(),
                                               include_pseudo);
    absl::StrAppend(&out, "\n", FormatHeader("Table Aliases"),
                    RenderTable(header, rows));
  }

  if (!input.common_table_expression().empty() ||
      !output.common_table_expression().empty()) {
    const bool include_pseudo = HasAnyPseudo(
        input.common_table_expression(), output.common_table_expression());
    std::vector<std::string> header = {"Name", "Columns", "Pseudo-columns"};
    if (!include_pseudo) header.pop_back();
    std::vector<Row> rows =
        DiffTableAliasRows(input.common_table_expression(),
                           output.common_table_expression(), include_pseudo);
    absl::StrAppend(&out, "\n", FormatHeader("Common table expressions"),
                    RenderTable(header, rows));
  }

  // Trailing description lines for value-table / ordered status.
  auto flag_line = [](bool in_flag, bool out_flag,
                      absl::string_view text) -> std::string {
    if (!in_flag && !out_flag) return "";
    Mark mark = Mark::kSame;
    if (in_flag && !out_flag) mark = Mark::kDel;
    if (!in_flag && out_flag) mark = Mark::kAdd;
    return absl::StrCat(WrapMark(mark, EscapeHtml(text)), "\n");
  };
  std::string value_line = flag_line(input.is_value_table(),
                                     output.is_value_table(),
                                     "Result is a value table.");
  std::string ordered_line =
      flag_line(input.is_ordered(), output.is_ordered(), "Result is ordered.");
  if (!value_line.empty() || !ordered_line.empty()) {
    absl::StrAppend(&out, "\n", value_line, ordered_line);
  }

  return absl::StrReplaceAll(out, {{"\n", "<br>"}});
}

}  // namespace reflection
}  // namespace googlesql
