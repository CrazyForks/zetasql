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

#include "googlesql/analyzer/conflicting_field_paths_validator.h"

#include <memory>
#include <string>
#include <vector>

#include "googlesql/analyzer/resolver.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/testdata/test_schema.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/check.h"
#include "absl/status/status.h"

namespace googlesql {
namespace {

using ::googlesql_test::KitchenSinkPB;
using ::testing::HasSubstr;
using ::absl_testing::StatusIs;

class ConflictingFieldPathsValidatorTest : public ::testing::Test {
 protected:
  ConflictingFieldPathsValidatorTest() = default;

  void SetUp() override {
    kitchen_sink_descriptor_ = KitchenSinkPB::descriptor();
    nested_descriptor_ =
        GetField(kitchen_sink_descriptor_, "nested_value")->message_type();
    ParserOptions options;
    GOOGLESQL_CHECK_OK(ParseExpression("1", options, &parser_output_));
    ast_node_ = parser_output_->expression();
  }

  absl::Status AddFieldPath(
      ConflictingFieldPathsValidator* validator,
      const std::vector<const google::protobuf::FieldDescriptor*>& fields) {
    Resolver::FindFieldsOutput output;
    output.field_descriptor_path = fields;
    return validator->AddFieldPath(ast_node_, output);
  }

  const google::protobuf::FieldDescriptor* GetField(const google::protobuf::Descriptor* descriptor,
                                          const std::string& field_name) {
    const google::protobuf::FieldDescriptor* field =
        descriptor->FindFieldByName(field_name);
    ABSL_CHECK(field != nullptr) << "Field " << field_name << " not found in "
                            << descriptor->full_name();
    return field;
  }

  ConflictingFieldPathsValidator validator_;
  LanguageOptions language_options_;
  const google::protobuf::Descriptor* kitchen_sink_descriptor_ = nullptr;
  const google::protobuf::Descriptor* nested_descriptor_ = nullptr;
  std::unique_ptr<ParserOutput> parser_output_;
  const ASTNode* ast_node_ = nullptr;
};

TEST_F(ConflictingFieldPathsValidatorTest, NoConflict) {
  GOOGLESQL_EXPECT_OK(AddFieldPath(&validator_,
                         {GetField(kitchen_sink_descriptor_, "int64_key_1")}));
  GOOGLESQL_EXPECT_OK(AddFieldPath(&validator_,
                         {GetField(kitchen_sink_descriptor_, "int64_key_2")}));
  GOOGLESQL_EXPECT_OK(AddFieldPath(
      &validator_, {GetField(kitchen_sink_descriptor_, "nested_value"),
                    GetField(nested_descriptor_, "nested_repeated_int64")}));
}

TEST_F(ConflictingFieldPathsValidatorTest, PathConflict) {
  GOOGLESQL_EXPECT_OK(AddFieldPath(&validator_,
                         {GetField(kitchen_sink_descriptor_, "nested_value")}));
  EXPECT_THAT(
      AddFieldPath(&validator_,
                   {GetField(kitchen_sink_descriptor_, "nested_value"),
                    GetField(nested_descriptor_, "value")}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Field path 'nested_value.value' overlaps with "
                         "field path 'nested_value'")));
}

TEST_F(ConflictingFieldPathsValidatorTest, PathConflictInverse) {
  GOOGLESQL_EXPECT_OK(AddFieldPath(&validator_,
                         {GetField(kitchen_sink_descriptor_, "nested_value"),
                          GetField(nested_descriptor_, "value")}));
  EXPECT_THAT(
      AddFieldPath(&validator_,
                   {GetField(kitchen_sink_descriptor_, "nested_value")}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Field path 'nested_value' "
                         "overlaps with field path 'nested_value.value'")));
}

TEST_F(ConflictingFieldPathsValidatorTest, PathConflictSamePath) {
  GOOGLESQL_EXPECT_OK(AddFieldPath(&validator_,
                         {GetField(kitchen_sink_descriptor_, "nested_value")}));
  EXPECT_THAT(AddFieldPath(&validator_, {GetField(kitchen_sink_descriptor_,
                                                  "nested_value")}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Field path 'nested_value' overlaps "
                                 "with field path 'nested_value'")));
}

TEST_F(ConflictingFieldPathsValidatorTest, OneOfConflict) {
  GOOGLESQL_EXPECT_OK(AddFieldPath(&validator_,
                         {GetField(kitchen_sink_descriptor_, "int32_one_of")}));
  EXPECT_THAT(
      AddFieldPath(&validator_,
                   {GetField(kitchen_sink_descriptor_, "string_one_of")}),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Modifying multiple fields from the same OneOf is "
                    "unsupported. Field path 'string_one_of' overlaps with "
                    "field path 'int32_one_of'")));
}

}  // namespace
}  // namespace googlesql
