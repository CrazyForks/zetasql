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

#include "googlesql/public/language_options.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "google/protobuf/descriptor.h"

namespace googlesql {

using ::testing::ContainerEq;
using ::testing::IsEmpty;
using ::testing::IsSupersetOf;
using ::testing::Not;
using ::testing::StartsWith;
using ::absl_testing::StatusIs;

TEST(LanguageOptions, TestAllAcceptingStatementKind) {
  LanguageOptions options;
  options.SetSupportedStatementKinds({});
  EXPECT_TRUE(options.SupportsStatementKind(RESOLVED_QUERY_STMT));
  EXPECT_TRUE(options.SupportsStatementKind(RESOLVED_EXPLAIN_STMT));
}

TEST(LanguageOptions, TestSeSupportsAllStatementKinds) {
  LanguageOptions options;
  options.SetSupportsAllStatementKinds();
  EXPECT_TRUE(options.SupportsStatementKind(RESOLVED_QUERY_STMT));
  EXPECT_TRUE(options.SupportsStatementKind(RESOLVED_EXPLAIN_STMT));
}

TEST(LanguageOptions, TestStatementKindRestriction) {
  LanguageOptions options;
  options.SetSupportedStatementKinds({RESOLVED_QUERY_STMT});
  EXPECT_TRUE(options.SupportsStatementKind(RESOLVED_QUERY_STMT));
  EXPECT_FALSE(options.SupportsStatementKind(RESOLVED_EXPLAIN_STMT));
}

TEST(LanguageOptions, TestStatementKindRestrictionWithDefault) {
  LanguageOptions options;  // Default is RESOLVED_QUERY_STMT.
  EXPECT_TRUE(options.SupportsStatementKind(RESOLVED_QUERY_STMT));
  EXPECT_FALSE(options.SupportsStatementKind(RESOLVED_EXPLAIN_STMT));
}

TEST(LanguageOptions, ProtoInProductExternal) {
  // Create external language options.
  LanguageOptions options;
  options.set_product_mode(PRODUCT_EXTERNAL);
  EXPECT_FALSE(options.SupportsProtoTypes());

  // Disable external proto.
  options.DisableLanguageFeature(FEATURE_PROTO_BASE);
  EXPECT_FALSE(options.SupportsProtoTypes());

  // Enable external proto.
  options.EnableLanguageFeature(FEATURE_PROTO_BASE);
  EXPECT_TRUE(options.SupportsProtoTypes());
}

// Get the set of possible enum values for a proto enum type.
// ENUM is the c++ enum type, and <descriptor> is its EnumDescriptor.
template <class ENUM>
static std::set<ENUM> GetEnumValues(const google::protobuf::EnumDescriptor* descriptor) {
  std::set<ENUM> values;
  for (int i = 0; i < descriptor->value_count(); ++i) {
    const ENUM value = static_cast<ENUM>(descriptor->value(i)->number());
    values.insert(value);
  }
  return values;
}

TEST(LanguageOptions, GetLanguageFeaturesForVersion) {
  EXPECT_THAT(LanguageOptions::GetLanguageFeaturesForVersion(VERSION_1_0),
              IsEmpty());

  EXPECT_TRUE(LanguageOptions::GetLanguageFeaturesForVersion(VERSION_CURRENT)
                  .contains(FEATURE_ORDER_BY_COLLATE));
  EXPECT_FALSE(LanguageOptions::GetLanguageFeaturesForVersion(VERSION_CURRENT)
                   .contains(FEATURE_TABLESAMPLE));

  LanguageOptions::LanguageFeatureSet features_in_current =
      LanguageOptions::GetLanguageFeaturesForVersion(VERSION_CURRENT);

  // Now do some sanity checks on LanguageVersions vs LanguageFeatures.
  // LanguageVersion should include all features under that version, and that
  // under lesser versions. LanguageVersion emums are ordered.
  for (const LanguageVersion version :
       GetEnumValues<LanguageVersion>(LanguageVersion_descriptor())) {
    if (version == LANGUAGE_VERSION_UNSPECIFIED || version == VERSION_CURRENT ||
        version == __LanguageVersion__switch_must_have_a_default__) {
      continue;
    }

    // Make sure LanguageVersion enums match expected format.
    const std::string version_name = LanguageVersion_Name(version);
    EXPECT_EQ("VERSION_", version_name.substr(0, 8));

    // Make sure the result from `GetLanguageFeaturesForVersion(X)` matches the
    // features with the option `language_version = (<=X)`.
    LanguageOptions::LanguageFeatureSet computed_features;
    for (int i = 0; i < LanguageFeature_descriptor()->value_count(); ++i) {
      const google::protobuf::EnumValueDescriptor* value_desc =
          LanguageFeature_descriptor()->value(i);
      auto feature = static_cast<LanguageFeature>(value_desc->number());
      const LanguageFeatureOptions& options =
          value_desc->options().GetExtension(language_feature_options);
      if (!options.ideally_enabled()) {
        // GetLanguageFeaturesForVersion should only include ideally enabled
        // features.
        continue;
      }
      if (options.in_development()) {
        continue;
      }
      if (options.has_language_version() &&
          options.language_version() <= version) {
        computed_features.insert(feature);
      }
    }

    EXPECT_THAT(
        computed_features,
        ContainerEq(LanguageOptions::GetLanguageFeaturesForVersion(version)))
        << "for version " << LanguageVersion_Name(version);

    // Also check that all features included in version X are also included in
    // VERSION_CURRENT.
    for (const LanguageFeature feature :
         LanguageOptions::GetLanguageFeaturesForVersion(version)) {
      EXPECT_TRUE(features_in_current.contains(feature))
          << "Features for VERSION_CURRENT does not include feature " << feature
          << " from " << version_name;
    }
  }
}

namespace {
struct VersionDetails {
  LanguageVersion version_num;
  absl::flat_hash_set<LanguageFeature> features;
  std::optional<size_t> expected_num_features;  // Set for frozen versions.
};

std::vector<VersionDetails> GetKnownVersionDetails() {
  return {
      {VERSION_1_0, LanguageOptions::GetLanguageFeaturesForVersion(VERSION_1_0),
       0},
      {VERSION_1_1, LanguageOptions::GetLanguageFeaturesForVersion(VERSION_1_1),
       11},
      {VERSION_1_2, LanguageOptions::GetLanguageFeaturesForVersion(VERSION_1_2),
       22},
      {VERSION_1_3, LanguageOptions::GetLanguageFeaturesForVersion(VERSION_1_3),
       64},
      {VERSION_1_4, LanguageOptions::GetLanguageFeaturesForVersion(VERSION_1_4),
       148},
      {VERSION_1_5, LanguageOptions::GetLanguageFeaturesForVersion(VERSION_1_5),
       std::nullopt},
  };
}
}  // namespace

TEST(LanguageOptions, LanguageFeaturesKnownVersions) {
  // Ensure that the number of versions in the LanguageVersion enum matches the
  // test data from GetKnownVersionDetails().
  std::vector<VersionDetails> known_versions = GetKnownVersionDetails();
  int num_versions = LanguageVersion_descriptor()->value_count();
  num_versions--;  // VERSION_CURRENT
  num_versions--;  // LANGUAGE_VERSION_UNSPECIFIED
  num_versions--;  // Do not use this in a switch
  EXPECT_EQ(num_versions, known_versions.size())
      << "Did you add a version and forget to update language_options_test?";
}

TEST(LanguageOptions, LanguageFeaturesEnableMax) {
  // Test that EnableMaximumLanguageFeatures[ForDevelopment] functions enable
  // the correct number of features.
  int num_max_features = 0;
  int num_max_dev_features = 0;
  for (int i = 0; i < LanguageFeature_descriptor()->value_count(); ++i) {
    const google::protobuf::EnumValueDescriptor* value_desc =
        LanguageFeature_descriptor()->value(i);
    int tag_number = value_desc->number();
    if (static_cast<LanguageFeature>(tag_number) ==
        __LanguageFeature__switch_must_have_a_default__) {
      continue;
    }
    const LanguageFeatureOptions& options =
        value_desc->options().GetExtension(language_feature_options);
    const bool ideally_enabled = options.ideally_enabled();
    const bool in_development = options.in_development();
    if (ideally_enabled) {
      ++num_max_dev_features;
      if (!in_development) {
        ++num_max_features;
      }
    }
  }
  {
    LanguageOptions options;
    options.EnableMaximumLanguageFeatures();
    EXPECT_EQ(options.GetEnabledLanguageFeatures().size(), num_max_features);
  }
  {
    LanguageOptions options;
    options.EnableMaximumLanguageFeaturesForDevelopment();
    EXPECT_EQ(options.GetEnabledLanguageFeatures().size(),
              num_max_dev_features);
  }
}

TEST(LanguageOptions, LanguageFeaturesNaming) {
  for (int i = 0; i < LanguageFeature_descriptor()->value_count(); ++i) {
    const google::protobuf::EnumValueDescriptor* value_desc =
        LanguageFeature_descriptor()->value(i);
    int tag_number = value_desc->number();
    absl::string_view name = value_desc->name();
    if (static_cast<LanguageFeature>(tag_number) ==
        __LanguageFeature__switch_must_have_a_default__) {
      continue;
    }
    EXPECT_THAT(name, StartsWith("FEATURE_"));
    EXPECT_THAT(name, Not(StartsWith("FEATURE_V_")))
        << "Feature names should not include version numbers like "
        << "FEATURE_V_1_x";
  }
}

TEST(LanguageOptions, LanguageFeaturesInDevelopmentFrozenVersion) {
  // Features marked in_development=true cannot have a frozen version.
  std::vector<VersionDetails> known_versions = GetKnownVersionDetails();
  LanguageVersion unfrozen_version;
  for (const VersionDetails& v : GetKnownVersionDetails()) {
    if (!v.expected_num_features.has_value()) {
      unfrozen_version = v.version_num;
    }
  }

  for (int i = 0; i < LanguageFeature_descriptor()->value_count(); ++i) {
    const google::protobuf::EnumValueDescriptor* value_desc =
        LanguageFeature_descriptor()->value(i);
    int tag_number = value_desc->number();
    absl::string_view name = value_desc->name();
    const LanguageFeatureOptions& options =
        value_desc->options().GetExtension(language_feature_options);
    const bool in_development = options.in_development();

    if (!options.has_language_version()) {
      continue;
    }
    const LanguageVersion version = options.language_version();

    EXPECT_TRUE(!in_development || version == unfrozen_version ||
                version <= VERSION_CURRENT)
        << "Feature " << name << " with tag " << tag_number
        << " is in_development, and associated with a frozen version. If the "
        << "feature is intended to be versioned, increment it to the "
        << "unfrozen version (currently VERSION_1_5).";
  }
}

TEST(LanguageOptions, MaximumFeatures) {
  const LanguageOptions options = LanguageOptions::MaximumFeatures();

  // Some features that are ideally enabled and released.
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ANALYTIC_FUNCTIONS));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_GROUP_BY_ROLLUP));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ORDER_BY_COLLATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_GROUP_BY_STRUCT));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_GROUP_BY_ARRAY));
  EXPECT_TRUE(
      options.LanguageFeatureEnabled(FEATURE_SELECT_STAR_EXCEPT_REPLACE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ORDER_BY_IN_AGGREGATE));
  EXPECT_TRUE(
      options.LanguageFeatureEnabled(FEATURE_CAST_DIFFERENT_ARRAY_TYPES));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ARRAY_EQUALITY));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_LIMIT_IN_AGGREGATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_HAVING_IN_AGGREGATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(
      FEATURE_NULL_HANDLING_MODIFIER_IN_ANALYTIC));
  EXPECT_TRUE(options.LanguageFeatureEnabled(
      FEATURE_NULL_HANDLING_MODIFIER_IN_AGGREGATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_CIVIL_TIME));

  // Some features that are released but not ideally enabled.
  EXPECT_FALSE(
      options.LanguageFeatureEnabled(FEATURE_DISALLOW_NULL_PRIMARY_KEYS));
  EXPECT_FALSE(options.LanguageFeatureEnabled(FEATURE_TEST_IDEALLY_DISABLED));

  // A feature that is ideally enabled but under development.
  EXPECT_FALSE(options.LanguageFeatureEnabled(
      FEATURE_TEST_IDEALLY_ENABLED_BUT_IN_DEVELOPMENT));

  // A feature that is not ideally enabled and is under development.
  EXPECT_FALSE(options.LanguageFeatureEnabled(
      FEATURE_TEST_IDEALLY_DISABLED_AND_IN_DEVELOPMENT));

  EXPECT_FALSE(options.LanguageFeatureEnabled(
      __LanguageFeature__switch_must_have_a_default__));
}

TEST(LanguageOptions, EnableMaximumLanguageFeaturesForDevelopment) {
  LanguageOptions options;
  options.EnableMaximumLanguageFeaturesForDevelopment();

  // Some features that are ideally enabled and released.
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ANALYTIC_FUNCTIONS));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_GROUP_BY_ROLLUP));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ORDER_BY_COLLATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_GROUP_BY_STRUCT));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_GROUP_BY_ARRAY));
  EXPECT_TRUE(
      options.LanguageFeatureEnabled(FEATURE_SELECT_STAR_EXCEPT_REPLACE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ORDER_BY_IN_AGGREGATE));
  EXPECT_TRUE(
      options.LanguageFeatureEnabled(FEATURE_CAST_DIFFERENT_ARRAY_TYPES));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_ARRAY_EQUALITY));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_LIMIT_IN_AGGREGATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_HAVING_IN_AGGREGATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(
      FEATURE_NULL_HANDLING_MODIFIER_IN_ANALYTIC));
  EXPECT_TRUE(options.LanguageFeatureEnabled(
      FEATURE_NULL_HANDLING_MODIFIER_IN_AGGREGATE));
  EXPECT_TRUE(options.LanguageFeatureEnabled(FEATURE_CIVIL_TIME));

  // Some features that are released but not ideally enabled.
  EXPECT_FALSE(
      options.LanguageFeatureEnabled(FEATURE_DISALLOW_NULL_PRIMARY_KEYS));
  EXPECT_FALSE(options.LanguageFeatureEnabled(FEATURE_TEST_IDEALLY_DISABLED));

  // A feature that is ideally enabled but under development.
  EXPECT_TRUE(options.LanguageFeatureEnabled(
      FEATURE_TEST_IDEALLY_ENABLED_BUT_IN_DEVELOPMENT));

  // A feature that is not ideally enabled and is under development.
  EXPECT_FALSE(options.LanguageFeatureEnabled(
      FEATURE_TEST_IDEALLY_DISABLED_AND_IN_DEVELOPMENT));

  EXPECT_FALSE(options.LanguageFeatureEnabled(
      __LanguageFeature__switch_must_have_a_default__));
}

TEST(LanguageOptions, FeatureSetSubsetting) {
  // Test that VERSION_x_y is a superset of all features of lower versions.
  std::vector<LanguageOptions::LanguageFeatureSet> feature_sets;
  LanguageOptions opts;
  opts.EnableMaximumLanguageFeaturesForDevelopment();
  feature_sets.push_back(opts.GetEnabledLanguageFeatures());
  opts.DisableAllLanguageFeatures();
  opts.EnableMaximumLanguageFeatures();
  feature_sets.push_back(opts.GetEnabledLanguageFeatures());
  feature_sets.push_back(
      LanguageOptions::GetLanguageFeaturesForVersion(VERSION_CURRENT));
  std::vector<VersionDetails> known_versions = GetKnownVersionDetails();
  // Iterate backward to get descending version order.
  for (size_t i = known_versions.size() - 1; i < known_versions.size(); --i) {
    feature_sets.push_back(known_versions[i].features);
  }

  for (size_t i = 0; i < feature_sets.size(); ++i) {
    for (size_t j = i + 1; j < feature_sets.size(); ++j) {
      EXPECT_THAT(feature_sets[i], IsSupersetOf(feature_sets[j]));
    }
  }
}

TEST(LanguageOptions, Serialization) {
  LanguageOptionsProto proto;
  proto.set_product_mode(PRODUCT_EXTERNAL);
  proto.set_name_resolution_mode(NAME_RESOLUTION_STRICT);
  proto.set_error_on_deprecated_syntax(true);
  proto.add_enabled_language_features(FEATURE_SELECT_STAR_EXCEPT_REPLACE);
  proto.add_enabled_language_features(FEATURE_TABLESAMPLE);
  proto.add_supported_statement_kinds(RESOLVED_EXPLAIN_STMT);
  proto.add_supported_generic_entity_types("NEW_TYPE");
  proto.add_supported_generic_sub_entity_types("NEW_SUB_TYPE");
  proto.add_reserved_keywords("QUALIFY");

  LanguageOptions options(proto);
  ASSERT_EQ(PRODUCT_EXTERNAL, options.product_mode());
  ASSERT_EQ(NAME_RESOLUTION_STRICT, options.name_resolution_mode());
  ASSERT_TRUE(options.error_on_deprecated_syntax());
  ASSERT_TRUE(
      options.LanguageFeatureEnabled(FEATURE_SELECT_STAR_EXCEPT_REPLACE));
  ASSERT_TRUE(options.LanguageFeatureEnabled(FEATURE_TABLESAMPLE));
  ASSERT_FALSE(options.LanguageFeatureEnabled(FEATURE_ORDER_BY_COLLATE));
  ASSERT_TRUE(options.SupportsStatementKind(RESOLVED_EXPLAIN_STMT));
  ASSERT_FALSE(options.SupportsStatementKind(RESOLVED_QUERY_STMT));
  ASSERT_TRUE(options.GenericEntityTypeSupported("NEW_TYPE"));
  ASSERT_TRUE(options.GenericEntityTypeSupported("new_type"));
  ASSERT_FALSE(options.GenericEntityTypeSupported("unsupported"));
  ASSERT_TRUE(options.GenericSubEntityTypeSupported("NEW_SUB_TYPE"));
  ASSERT_TRUE(options.GenericSubEntityTypeSupported("new_sub_type"));
  ASSERT_FALSE(options.GenericSubEntityTypeSupported("unsupported_sub_type"));
  ASSERT_TRUE(
      options.GenericEntityTypeSupported(absl::string_view("NEW_TYPE")));
  ASSERT_TRUE(
      options.GenericEntityTypeSupported(absl::string_view("new_type")));
  ASSERT_FALSE(
      options.GenericEntityTypeSupported(absl::string_view("unsupported")));
  ASSERT_TRUE(
      options.GenericSubEntityTypeSupported(absl::string_view("NEW_SUB_TYPE")));
  ASSERT_TRUE(
      options.GenericSubEntityTypeSupported(absl::string_view("new_sub_type")));
  ASSERT_FALSE(options.GenericSubEntityTypeSupported(
      absl::string_view("unsupported_sub_type")));

  ASSERT_TRUE(options.IsReservedKeyword("QUALIFY"));
}

TEST(LanguageOptions, GetEnabledLanguageFeaturesAsString) {
  LanguageOptions options;
  EXPECT_EQ("", options.GetEnabledLanguageFeaturesAsString());
  options.EnableLanguageFeature(FEATURE_TABLESAMPLE);
  EXPECT_EQ("FEATURE_TABLESAMPLE",
            options.GetEnabledLanguageFeaturesAsString());
  options.EnableLanguageFeature(FEATURE_ANALYTIC_FUNCTIONS);
  EXPECT_EQ("FEATURE_ANALYTIC_FUNCTIONS, FEATURE_TABLESAMPLE",
            options.GetEnabledLanguageFeaturesAsString());
  options.EnableLanguageFeature(FEATURE_CIVIL_TIME);
  EXPECT_EQ(
      "FEATURE_ANALYTIC_FUNCTIONS, FEATURE_CIVIL_TIME, FEATURE_TABLESAMPLE",
      options.GetEnabledLanguageFeaturesAsString());
}

TEST(LanguageOptions, ReservedKeywords) {
  // GetReservableKeywords
  EXPECT_TRUE(LanguageOptions::GetReservableKeywords().contains("QUALIFY"));
  EXPECT_TRUE(LanguageOptions::GetReservableKeywords().contains("qualify"));
  EXPECT_TRUE(LanguageOptions::GetReservableKeywords().contains(
      absl::string_view("qualify")));
  EXPECT_FALSE(LanguageOptions::GetReservableKeywords().contains("SELECT"));
  EXPECT_FALSE(LanguageOptions::GetReservableKeywords().contains("DECIMAL"));
  EXPECT_FALSE(LanguageOptions::GetReservableKeywords().contains(""));

  // Initial LanguageOptions
  LanguageOptions options;
  EXPECT_FALSE(options.IsReservedKeyword("QUALIFY"));
  EXPECT_FALSE(options.IsReservedKeyword(absl::string_view("QUALIFY")));
  EXPECT_FALSE(options.IsReservedKeyword("qualify"));
  EXPECT_FALSE(options.IsReservedKeyword(""));
  EXPECT_FALSE(options.IsReservedKeyword("DECIMAL"));
  EXPECT_TRUE(options.IsReservedKeyword("SELECT"));

  // Reserving a keyword (uppercase)
  GOOGLESQL_EXPECT_OK(options.EnableReservableKeyword("QUALIFY", true));
  EXPECT_TRUE(options.IsReservedKeyword("QUALIFY"));
  EXPECT_TRUE(options.IsReservedKeyword("qualify"));
  EXPECT_TRUE(options.IsReservedKeyword(absl::string_view("qualify")));
  EXPECT_TRUE(options.IsReservedKeyword("SELECT"));
  EXPECT_FALSE(options.IsReservedKeyword("DECIMAL"));

  // Reserving a keyword already reserved earlier is ok
  GOOGLESQL_EXPECT_OK(options.EnableReservableKeyword("QUALIFY", true));
  EXPECT_TRUE(options.IsReservedKeyword("QUALIFY"));

  // Equality test
  EXPECT_TRUE(options == options);
  EXPECT_FALSE(options == LanguageOptions());

  // Unreserving a keyword
  GOOGLESQL_EXPECT_OK(options.EnableReservableKeyword("QUALIFY", false));
  EXPECT_FALSE(options.IsReservedKeyword("QUALIFY"));

  // Unreserving a keyword already unreserved keyword is ok
  EXPECT_FALSE(options.IsReservedKeyword("QUALIFY"));
  EXPECT_FALSE(options.IsReservedKeyword("qualify"));

  // Reserving all reservable keywords
  options.EnableAllReservableKeywords(true);
  EXPECT_TRUE(options.IsReservedKeyword("QUALIFY"));
  EXPECT_TRUE(options.IsReservedKeyword("SELECT"));
  EXPECT_FALSE(options.IsReservedKeyword("DECIMAL"));

  // Unreserving all reservable keywords
  options.EnableAllReservableKeywords(false);
  EXPECT_FALSE(options.IsReservedKeyword("QUALIFY"));
  EXPECT_TRUE(options.IsReservedKeyword("SELECT"));
  EXPECT_FALSE(options.IsReservedKeyword("DECIMAL"));

  // EnableMaximumLanguageFeatures() also reserves all keywords
  options.EnableMaximumLanguageFeatures();
  EXPECT_TRUE(options.IsReservedKeyword("QUALIFY"));

  // Same with EnableMaximumLanguageFeaturesForDevelopment().
  options.EnableAllReservableKeywords(false);
  options.EnableMaximumLanguageFeaturesForDevelopment();
  EXPECT_TRUE(options.IsReservedKeyword("QUALIFY"));

  // Attempting to configure a keyword that cannot be configured
  EXPECT_THAT(options.EnableReservableKeyword("SELECT", true),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(options.EnableReservableKeyword("SELECT", false),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(options.EnableReservableKeyword("", true),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(options.EnableReservableKeyword("DECIMAL", true),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(options.EnableReservableKeyword("not a keyword", true),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(LanguageOptions, ClassAndProtoSize) {
  LanguageOptions options;

  EXPECT_EQ(16, sizeof(options) - sizeof(options.supported_statement_kinds_) -
                    sizeof(options.enabled_language_features_) -
                    sizeof(options.supported_generic_entity_types_) -
                    sizeof(options.supported_generic_sub_entity_types_) -
                    sizeof(options.reserved_keywords_))
      << "The size of LanguageOptions class has changed, please also update "
      << "the proto and serialization code if you added/removed fields in it.";
  EXPECT_EQ(8, LanguageOptionsProto::descriptor()->field_count())
      << "The number of fields in LanguageOptionsProto has changed, please "
      << "also update the serialization code accordingly.";
}

TEST(LanguageOptions, EnableMaximumFeaturesDoesNotReserveGraphTable) {
  LanguageOptions options;
  options.EnableMaximumLanguageFeatures();
  EXPECT_TRUE(options.IsReservedKeyword("QUALIFY"));
  EXPECT_FALSE(options.IsReservedKeyword("GRAPH_TABLE"));
}

TEST(LanguageOptions, LanguageFeatureVersion) {
  EXPECT_EQ(LanguageFeatureVersion(FEATURE_TABLESAMPLE),
            LANGUAGE_VERSION_UNSPECIFIED);
  EXPECT_EQ(LanguageFeatureVersion(FEATURE_ORDER_BY_COLLATE), VERSION_1_1);
  EXPECT_EQ(LanguageFeatureVersion(FEATURE_CIVIL_TIME), VERSION_1_2);
  EXPECT_EQ(LanguageFeatureVersion(FEATURE_INLINE_LAMBDA_ARGUMENT),
            VERSION_1_3);
}

TEST(LanguageOptions, LanguageFeatureDefaultValueTest) {
  LanguageFeatureOptions proto;
  auto features_from_proto_default =
      LanguageOptions::GetLanguageFeaturesForVersion(proto.language_version());
  auto features_from_current =
      LanguageOptions::GetLanguageFeaturesForVersion(VERSION_CURRENT);
  EXPECT_EQ(features_from_proto_default, features_from_current);
  auto features_from_unspecified =
      LanguageOptions::GetLanguageFeaturesForVersion(
          LANGUAGE_VERSION_UNSPECIFIED);
  EXPECT_EQ(features_from_proto_default, features_from_unspecified);
}

}  // namespace googlesql
