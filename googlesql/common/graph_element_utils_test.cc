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

#include "googlesql/common/graph_element_utils.h"

#include <vector>

#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/functions/json.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"

namespace googlesql {
namespace {

using ::testing::NotNull;

TEST(GraphElementUtilsTest, IsOrContainsGraphElementTest) {
  TypeFactory factory;
  const Type* string_type = factory.get_string();
  const Type* int_type = factory.get_int64();
  const Type* bytes_type = factory.get_bytes();

  // Basic types
  ASSERT_FALSE(TypeIsOrContainsGraphElement(string_type));
  ASSERT_FALSE(TypeIsOrContainsGraphElement(int_type));
  ASSERT_FALSE(TypeIsOrContainsGraphElement(bytes_type));

  // Node type
  const GraphElementType* node_type;
  GOOGLESQL_ASSERT_OK(factory.MakeGraphElementType(
      {"aml"}, GraphElementType::ElementKind::kNode,
      {{"id", int_type}, {"name", string_type}, {"data", bytes_type}},
      &node_type));
  ASSERT_THAT(node_type, NotNull());
  ASSERT_TRUE(TypeIsOrContainsGraphElement(node_type));

  // Edge type
  const GraphElementType* edge_type;
  GOOGLESQL_ASSERT_OK(factory.MakeGraphElementType(
      {"aml"}, GraphElementType::kEdge,
      {{"transfer_id", int_type}, {"amount", int_type}}, &edge_type));
  ASSERT_THAT(edge_type, NotNull());
  ASSERT_TRUE(TypeIsOrContainsGraphElement(edge_type));

  // Path type
  const GraphPathType* path_type;
  GOOGLESQL_ASSERT_OK(factory.MakeGraphPathType(node_type, edge_type, &path_type));
  ASSERT_THAT(path_type, NotNull());
  ASSERT_TRUE(TypeIsOrContainsGraphElement(path_type));

  // Array of GraphNode
  const ArrayType* array_type;
  GOOGLESQL_ASSERT_OK(factory.MakeArrayType(node_type, &array_type));
  ASSERT_THAT(array_type, NotNull());
  ASSERT_TRUE(TypeIsOrContainsGraphElement(array_type));

  // Struct of GraphEdge and GraphPath
  const StructType* struct_type;
  GOOGLESQL_ASSERT_OK(factory.MakeStructType({{"edge", edge_type}, {"path", path_type}},
                                   &struct_type));
  ASSERT_THAT(struct_type, NotNull());
  ASSERT_TRUE(TypeIsOrContainsGraphElement(struct_type));
}

TEST(GraphElementUtilsTest, MakePropertiesJsonValueTest) {
  const LanguageOptions language_options = LanguageOptions::MaximumFeatures();
  const Value p1_value = Value::Bool(true);
  const Value p2_value = Value::Double(3.14);
  std::vector<Value::Property> properties = {{"p1", p1_value},
                                             {"p2", p2_value}};
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      JSONValue json_value,
      MakePropertiesJsonValue(absl::MakeSpan(properties), language_options));
  EXPECT_EQ(json_value.GetConstRef().GetMembers().size(), 2);

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const bool p1,
      functions::ConvertJsonToBool(json_value.GetConstRef().GetMember("p1")));
  EXPECT_EQ(p1, p1_value.bool_value());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const double p2,
      functions::ConvertJsonToDouble(json_value.GetConstRef().GetMember("p2"),
                                     functions::WideNumberMode::kExact,
                                     ProductMode::PRODUCT_INTERNAL));
  EXPECT_EQ(p2, p2_value.double_value());
}

TEST(GraphElementUtilsTest, IsRestrictivePathModeTest) {
  EXPECT_TRUE(IsRestrictivePathMode(ResolvedGraphPathMode::TRAIL));
  EXPECT_TRUE(IsRestrictivePathMode(ResolvedGraphPathMode::ACYCLIC));
  EXPECT_TRUE(IsRestrictivePathMode(ResolvedGraphPathMode::SIMPLE));
  EXPECT_FALSE(IsRestrictivePathMode(ResolvedGraphPathMode::WALK));
}

TEST(GraphElementUtilsTest, HasSearchPrefixTest) {
  EXPECT_TRUE(HasSearchPrefix(ResolvedGraphPathSearchPrefix::ANY));
  EXPECT_TRUE(HasSearchPrefix(ResolvedGraphPathSearchPrefix::SHORTEST));
  EXPECT_TRUE(HasSearchPrefix(ResolvedGraphPathSearchPrefix::CHEAPEST));
  EXPECT_FALSE(HasSearchPrefix(
      ResolvedGraphPathSearchPrefix::PATH_SEARCH_PREFIX_TYPE_UNSPECIFIED));
}

TEST(GraphElementUtilsTest, IntersectPeriodsTest) {
  Value start1 = Value::TimestampFromUnixMicros(1000);
  Value end1 = Value::TimestampFromUnixMicros(2000);
  Value period1 = Value::MakeRange(start1, end1).value();

  Value start2 = Value::TimestampFromUnixMicros(1500);
  Value end2 = Value::TimestampFromUnixMicros(2500);
  Value period2 = Value::MakeRange(start2, end2).value();

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(Value intersection, IntersectPeriods(period1, period2));
  EXPECT_TRUE(intersection.start().Equals(start2));
  EXPECT_TRUE(intersection.end().Equals(end1));

  // Test unbounded start/end
  Value unbounded_start = Value::NullTimestamp();
  Value unbounded_end = Value::NullTimestamp();
  Value period3 = Value::MakeRange(unbounded_start, end2).value();
  Value period4 = Value::MakeRange(start1, unbounded_end).value();

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(Value intersection2, IntersectPeriods(period3, period4));
  EXPECT_TRUE(intersection2.start().Equals(start1));
  EXPECT_TRUE(intersection2.end().Equals(end2));
}

TEST(GraphElementUtilsTest, PeriodContainsPointTest) {
  Value start1 = Value::TimestampFromUnixMicros(1000);
  Value end1 = Value::TimestampFromUnixMicros(2000);
  Value period1 = Value::MakeRange(start1, end1).value();

  Value point_inside = Value::TimestampFromUnixMicros(1500);
  Value point_outside_before = Value::TimestampFromUnixMicros(500);
  Value point_outside_after = Value::TimestampFromUnixMicros(2500);

  EXPECT_TRUE(PeriodContainsPoint(period1, point_inside).value());
  EXPECT_TRUE(
      PeriodContainsPoint(period1, start1).value());  // start is inclusive
  EXPECT_FALSE(PeriodContainsPoint(period1, end1).value());  // end is exclusive
  EXPECT_FALSE(PeriodContainsPoint(period1, point_outside_before).value());
  EXPECT_FALSE(PeriodContainsPoint(period1, point_outside_after).value());
}

}  // namespace
}  // namespace googlesql
