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

#include <string>

#include "googlesql/common/reflection.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace googlesql {
namespace reflection {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

Column MakeColumn(absl::string_view name, absl::string_view type,
                  absl::string_view table_alias = "") {
  Column column;
  column.set_column_name(std::string(name));
  column.set_type(std::string(type));
  if (!table_alias.empty()) column.set_table_alias(std::string(table_alias));
  return column;
}

TEST(ReflectionHelperDiffTest, IdenticalTablesHaveNoDiffMarkers) {
  ResultTable table;
  *table.add_column() = MakeColumn("a", "INT64");
  *table.add_column() = MakeColumn("b", "STRING");

  std::string html = FormatResultTableDiffHtml(table, table);
  EXPECT_THAT(html, Not(HasSubstr("nl-add")));
  EXPECT_THAT(html, Not(HasSubstr("nl-del")));
  EXPECT_THAT(html, HasSubstr("a"));
  EXPECT_THAT(html, HasSubstr("b"));
}

TEST(ReflectionHelperDiffTest, AddedColumnMarkedAdded) {
  ResultTable input;
  *input.add_column() = MakeColumn("a", "INT64");
  ResultTable output;
  *output.add_column() = MakeColumn("a", "INT64");
  *output.add_column() = MakeColumn("b", "STRING");

  std::string html = FormatResultTableDiffHtml(input, output);
  EXPECT_THAT(html, HasSubstr("nl-add"));
  EXPECT_THAT(html, Not(HasSubstr("nl-del")));
}

TEST(ReflectionHelperDiffTest, RemovedColumnMarkedRemoved) {
  ResultTable input;
  *input.add_column() = MakeColumn("a", "INT64");
  *input.add_column() = MakeColumn("b", "STRING");
  ResultTable output;
  *output.add_column() = MakeColumn("a", "INT64");

  std::string html = FormatResultTableDiffHtml(input, output);
  EXPECT_THAT(html, HasSubstr("nl-del"));
  EXPECT_THAT(html, Not(HasSubstr("nl-add")));
}

TEST(ReflectionHelperDiffTest, TypeChangeShowsInlineTypeDiff) {
  ResultTable input;
  *input.add_column() = MakeColumn("a", "INT64");
  ResultTable output;
  *output.add_column() = MakeColumn("a", "STRING");

  std::string html = FormatResultTableDiffHtml(input, output);
  // Both the old and new type appear, marked deleted/added respectively.
  EXPECT_THAT(html, HasSubstr("nl-del"));
  EXPECT_THAT(html, HasSubstr("nl-add"));
  EXPECT_THAT(html, HasSubstr("INT64"));
  EXPECT_THAT(html, HasSubstr("STRING"));
  // The column name itself is unchanged, so the row is not a whole-row diff.
  EXPECT_THAT(html, HasSubstr("a"));
}

TEST(ReflectionHelperDiffTest, TableAliasColumnListDiffsIndividualItems) {
  ResultTable input;
  TableAlias* in_alias = input.add_table_alias();
  in_alias->set_name("t");
  in_alias->add_column_name("a");
  in_alias->add_column_name("b");

  ResultTable output;
  TableAlias* out_alias = output.add_table_alias();
  out_alias->set_name("t");
  out_alias->add_column_name("a");
  out_alias->add_column_name("c");

  std::string html = FormatResultTableDiffHtml(input, output);
  // `b` removed, `c` added inside the matched alias `t`.
  EXPECT_THAT(html, HasSubstr("nl-del"));
  EXPECT_THAT(html, HasSubstr("nl-add"));
  EXPECT_THAT(html, HasSubstr("Table Aliases"));
}

TEST(ReflectionHelperDiffTest, OutputIsHtmlWithBreaksNotNewlines) {
  ResultTable input;
  *input.add_column() = MakeColumn("a", "INT64");
  ResultTable output;
  *output.add_column() = MakeColumn("a", "INT64");

  std::string html = FormatResultTableDiffHtml(input, output);
  EXPECT_THAT(html, HasSubstr("<br>"));
  EXPECT_THAT(html, Not(HasSubstr("\n")));
}

}  // namespace
}  // namespace reflection
}  // namespace googlesql
