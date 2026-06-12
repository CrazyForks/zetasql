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

#include <memory>

#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/types/row_type.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/log_severity.h"

namespace googlesql {

namespace {
using testing::HasSubstr;
}  // namespace

// Test RowOrTableType::HasField, which only works if `:type_with_catalog_impl`
// is linked in.
TEST(TypeWithCatalogTest, RowOrTableType) {
  SimpleTable t1("T1");
  GOOGLESQL_ASSERT_OK(t1.AddColumn(std::make_unique<SimpleColumn>(t1.FullName(), "Col1",
                                                        types::Int32Type())));
  GOOGLESQL_ASSERT_OK(t1.AddColumn(std::make_unique<SimpleColumn>(t1.FullName(), "Col2",
                                                        types::Int32Type())));

  TypeFactory factory;
  const RowType* row_type;
  GOOGLESQL_ASSERT_OK(factory.MakeRowType(&t1, t1.FullName(), &row_type));

  EXPECT_TRUE(row_type->HasAnyFields());
  // ROW type may return a status from field lookup, so callers need to migrate
  // from HasField to FindField. HasFieldImpl has a ABSL_LOG(ERROR) to catch this
  // early in testing.
  EXPECT_DEBUG_DEATH(
      row_type->HasField("Col1"),
      HasSubstr("ROW type field lookup may produce an error, callers must call "
                "FindField() instead of HasField()"));
  if (!GOOGLESQL_DEBUG_MODE) {
    EXPECT_EQ(Type::HAS_FIELD, row_type->HasField("Col1"));
    EXPECT_EQ(Type::HAS_FIELD, row_type->HasField("cOL2"));
    EXPECT_EQ(Type::HAS_NO_FIELD, row_type->HasField("xxx"));
  }

  Type::FindFieldResult result;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(result, row_type->FindField("Col1"));
  EXPECT_EQ(Type::HAS_FIELD, result.has_field);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(result, row_type->FindField("cOL2"));
  EXPECT_EQ(Type::HAS_FIELD, result.has_field);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(result, row_type->FindField("xxx"));
  EXPECT_EQ(Type::HAS_NO_FIELD, result.has_field);
}

}  // namespace googlesql
