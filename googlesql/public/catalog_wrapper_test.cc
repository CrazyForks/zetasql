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

#include "googlesql/public/catalog_wrapper.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/cast.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/constant.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/procedure.h"
#include "googlesql/public/property_graph.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/simple_property_graph.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "googlesql/testdata/sample_catalog.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"

namespace googlesql {

using ::testing::StartsWith;

TEST(CatalogWrapperTest, FunctionCatalogWrapperTest) {
  TypeFactory type_factory;
  SampleCatalog sample_catalog(LanguageOptions(), &type_factory);
  FunctionCatalogWrapper wrapped_catalog("wrapped_catalog", &type_factory,
                                         sample_catalog.catalog());

  // The sample_catalog has tables, types, functions, etc.
  // The wrapped sample_catalog does not expose anything except functions.
  const Table* table;
  GOOGLESQL_EXPECT_OK(sample_catalog.catalog()->FindTable({"KeyValue"}, &table));
  EXPECT_THAT(wrapped_catalog.FindTable({"KeyValue"}, &table),
              absl_testing::StatusIs(absl::StatusCode::kNotFound));

  const Type* type;
  GOOGLESQL_EXPECT_OK(sample_catalog.catalog()->FindType({"TestStruct"}, &type));
  EXPECT_THAT(wrapped_catalog.FindType({"TestStruct"}, &type),
              absl_testing::StatusIs(absl::StatusCode::kNotFound));

  const Procedure* procedure;
  GOOGLESQL_EXPECT_OK(
      sample_catalog.catalog()->FindProcedure({"proc_no_args"}, &procedure));
  EXPECT_THAT(wrapped_catalog.FindProcedure({"proc_no_args"}, &procedure),
              absl_testing::StatusIs(absl::StatusCode::kNotFound));

  const TableValuedFunction* tvf;
  GOOGLESQL_EXPECT_OK(
      sample_catalog.catalog()->FindTableValuedFunction({"tvf_no_args"}, &tvf));
  EXPECT_THAT(wrapped_catalog.FindTableValuedFunction({"tvf_no_args"}, &tvf),
              absl_testing::StatusIs(absl::StatusCode::kNotFound));

  const Function* function;
  GOOGLESQL_EXPECT_OK(sample_catalog.catalog()->FindFunction({"concat"}, &function));
  GOOGLESQL_EXPECT_OK(wrapped_catalog.FindFunction({"concat"}, &function));

  const Constant* constant;
  GOOGLESQL_EXPECT_OK(
      sample_catalog.catalog()->FindConstant({"TestConstantInt64"}, &constant));
  EXPECT_THAT(wrapped_catalog.FindConstant({"TestConstantInt64"}, &constant),
              absl_testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST(CatalogWrapperTest, WrappedCatalog) {
  TypeFactory type_factory;
  auto simple_catalog =
      std::make_unique<SimpleCatalog>("simple", &type_factory);

  std::unique_ptr<SimpleConstant> new_constant;
  GOOGLESQL_ASSERT_OK(SimpleConstant::Create(std::vector<std::string>{"foo"},
                                   Value::Int64(1UL), &new_constant));
  simple_catalog->AddOwnedConstant(new_constant.release());

  const Constant* constant;
  {
    CatalogWrapper wrapper(simple_catalog.get());

    GOOGLESQL_ASSERT_OK(simple_catalog->FindConstant({"foo"}, &constant));
    EXPECT_EQ(constant->GetAs<SimpleConstant>()->value().int64_value(), 1);

    constant = nullptr;
    GOOGLESQL_ASSERT_OK(wrapper.FindConstant({"foo"}, &constant));
    EXPECT_EQ(constant->GetAs<SimpleConstant>()->value().int64_value(), 1);
  }
  // Since the wrapper does not own the wrapped <simple_catalog>, it going out
  // of scope should not have affected the wrapped catalog.
  constant = nullptr;
  GOOGLESQL_ASSERT_OK(simple_catalog->FindConstant({"foo"}, &constant));
  EXPECT_EQ(constant->GetAs<SimpleConstant>()->value().int64_value(), 1);

  {
    CatalogWrapper wrapper(std::move(simple_catalog));
    constant = nullptr;
    GOOGLESQL_ASSERT_OK(wrapper.FindConstant({"foo"}, &constant));
    EXPECT_EQ(constant->GetAs<SimpleConstant>()->value().int64_value(), 1);
  }
  // Now the std::unique_ptr that owned the <simple_catalog> is empty.
  EXPECT_EQ(simple_catalog, nullptr);
}

// A catalog for testing features not supported by SimpleCatalog.
class TestCatalog : public SimpleCatalog {
 public:
  TestCatalog(absl::string_view name, TypeFactory* type_factory)
      : SimpleCatalog(name, type_factory) {}

  absl::Status FindSequence(const absl::Span<const std::string>& path,
                            const Sequence** sequence,
                            const FindOptions& options) override {
    return absl::OkStatus();
  }

  absl::Status FindPropertyGraph(absl::Span<const std::string> path,
                                 const PropertyGraph*& property_graph,
                                 const FindOptions& options) override {
    return absl::OkStatus();
  }

  absl::Status FindConversion(
      const googlesql::Type* from_type, const googlesql::Type* to_type,
      const googlesql::Catalog::FindConversionOptions& options,
      googlesql::Conversion* conversion) override {
    return absl::OkStatus();
  }

 private:
  std::unique_ptr<Sequence> sequence_ =
      std::make_unique<SimpleSequence>("TestSequence");
  std::unique_ptr<PropertyGraph> property_graph_ =
      std::make_unique<SimplePropertyGraph>(
          std::vector<std::string>{"TestPropertyGraph"});
};

// Helper function to populate a SimpleCatalog with one of each object type.
void PopulateCatalog(SimpleCatalog* catalog, TypeFactory* type_factory) {
  catalog->AddOwnedTable(new SimpleTable("TestTable"));
  catalog->AddOwnedModel(new SimpleModel(
      "TestModel", {new SimpleColumn("SM2", "C1", type_factory->get_int64())},
      {new SimpleColumn("SM2", "O1", type_factory->get_double())},
      true /* take_ownership */));
  catalog->AddOwnedFunction(
      new Function("TestFunction", "test_group", Function::SCALAR));
  TVFRelation::ColumnList columns;
  columns.emplace_back("col0", types::BoolType());
  TVFRelation output_schema(columns);
  FunctionSignature tvf_sig(FunctionArgumentType::RelationWithSchema(
                                output_schema,
                                /*extra_relation_input_columns_allowed=*/false),
                            FunctionArgumentTypeList(), /*context_id=*/-1);

  catalog->AddOwnedTableValuedFunction(
      new FixedOutputSchemaTVF({"TestTVF"}, {tvf_sig}, output_schema));
  catalog->AddOwnedProcedure(
      new Procedure({"TestProcedure"}, {type_factory->get_int64(),
                                        {type_factory->get_int64()},
                                        -1 /* context */}));
  catalog->AddType("TestType", type_factory->get_int64());
  std::unique_ptr<SimpleConstant> constant;
  GOOGLESQL_ASSERT_OK(
      SimpleConstant::Create({"TestConstant"}, Value::Int64(1), &constant));
  catalog->AddOwnedConstant(constant.release());
}

// Parameterized test fixture for CatalogWrapper::Options.
class CatalogWrapperOptionsTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    catalog_ = std::make_unique<TestCatalog>("simple", &type_factory_);
    PopulateCatalog(catalog_.get(), &type_factory_);
  }

  TypeFactory type_factory_;
  std::unique_ptr<TestCatalog> catalog_;
};

INSTANTIATE_TEST_SUITE_P(AllOptions, CatalogWrapperOptionsTest,
                         ::testing::Bool());

TEST_P(CatalogWrapperOptionsTest, TestAllowTable) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_table = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const Table* table;
  EXPECT_EQ(wrapper.FindTable({"TestTable"}, &table).code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
  EXPECT_EQ(wrapper.SuggestTable({"TestTabl"}), allow ? "TestTable" : "");
}

TEST_P(CatalogWrapperOptionsTest, TestAllowModel) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_model = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const Model* model;
  EXPECT_EQ(wrapper.FindModel({"TestModel"}, &model).code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
}

TEST_P(CatalogWrapperOptionsTest, TestAllowFunction) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_function = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const Function* function;
  EXPECT_EQ(wrapper.FindFunction({"TestFunction"}, &function).code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
  EXPECT_EQ(wrapper.SuggestFunction({"TestFunctio"}),
            allow ? "testfunction" : "");
}

TEST_P(CatalogWrapperOptionsTest, TestAllowTableValuedFunction) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_table_valued_function = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const TableValuedFunction* tvf;
  EXPECT_EQ(wrapper.FindTableValuedFunction({"TestTVF"}, &tvf).code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
  EXPECT_EQ(wrapper.SuggestTableValuedFunction({"TestTV"}),
            allow ? "testtvf" : "");
}

TEST_P(CatalogWrapperOptionsTest, TestAllowProcedure) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_procedure = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const Procedure* procedure;
  EXPECT_EQ(wrapper.FindProcedure({"TestProcedure"}, &procedure).code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
}

TEST_P(CatalogWrapperOptionsTest, TestAllowType) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_type = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const Type* type;
  EXPECT_EQ(wrapper.FindType({"TestType"}, &type).code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
}

TEST_P(CatalogWrapperOptionsTest, TestAllowConstant) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_constant = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const Constant* constant;
  EXPECT_EQ(wrapper.FindConstant({"TestConstant"}, &constant).code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
  EXPECT_EQ(wrapper.SuggestConstant({"TestConstan"}),
            allow ? "TestConstant" : "");
}

TEST_P(CatalogWrapperOptionsTest, TestAllowSequence) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_sequence = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const Sequence* sequence;
  EXPECT_EQ(
      wrapper.FindSequence({"TestSequence"}, &sequence, Catalog::FindOptions())
          .code(),
      allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
}

TEST_P(CatalogWrapperOptionsTest, TestAllowPropertyGraph) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_property_graph = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  const PropertyGraph* property_graph;
  EXPECT_EQ(wrapper
                .FindPropertyGraph({"TestPropertyGraph"}, property_graph,
                                   Catalog::FindOptions())
                .code(),
            allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
}

TEST_P(CatalogWrapperOptionsTest, TestAllowConversion) {
  bool allow = GetParam();
  CatalogWrapper::Options options = CatalogWrapper::Options::AllDisabled();
  options.allow_conversion = allow;
  CatalogWrapper wrapper(catalog_.get(), options);

  EXPECT_EQ(
      wrapper
          .FindConversion(type_factory_.get_int64(), type_factory_.get_string(),
                          Catalog::FindConversionOptions(
                              /*is_explicit=*/false,
                              Catalog::ConversionSourceExpressionKind::kOther),
                          nullptr)
          .code(),
      allow ? absl::StatusCode::kOk : absl::StatusCode::kNotFound);
}

}  // namespace googlesql
