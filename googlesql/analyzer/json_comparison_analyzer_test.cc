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
#include <string>
#include <tuple>
#include <vector>

#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "googlesql/testdata/sample_catalog.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/no_destructor.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"

namespace googlesql {
namespace {

using ::testing::Not;
using ::absl_testing::IsOk;

struct CatalogHelper {
  TypeFactory type_factory;
  AnalyzerOptions analyzer_options;
  std::unique_ptr<SampleCatalog> sample_catalog;

  CatalogHelper() {
    analyzer_options.mutable_language()
        ->EnableMaximumLanguageFeaturesForDevelopment();
    analyzer_options.mutable_language()->EnableLanguageFeature(
        FEATURE_JSON_TYPE);
    analyzer_options.mutable_language()->EnableLanguageFeature(
        FEATURE_JSON_TYPE_COMPARISON_COERCION);
    analyzer_options.mutable_language()->EnableLanguageFeature(
        FEATURE_COLLATION_SUPPORT);
    sample_catalog = std::make_unique<SampleCatalog>(
        analyzer_options.language(), &type_factory);
  }
};

CatalogHelper& GetCatalogHelper() {
  static absl::NoDestructor<CatalogHelper> helper;
  return *helper;
}

absl::Status Analyze(absl::string_view sql) {
  CatalogHelper& helper = GetCatalogHelper();
  std::unique_ptr<const AnalyzerOutput> output;
  return AnalyzeStatement(sql, helper.analyzer_options,
                          helper.sample_catalog->catalog(),
                          &helper.type_factory, &output);
}

struct QueryTmpl {
  std::string name;
  std::string sql;
};

struct JSONComparisonTestParam {
  std::string name;
  std::string literal_sql;
  bool expect_equality_success;
  bool expect_order_success;
};

std::vector<JSONComparisonTestParam> GetTestParams() {
  return {{"JSON", "JSON '1'", true, true},
          // Types that support both equality and ordering cast to JSON.
          {"BOOL", "true", true, true},
          {"INT32", "CAST(1 AS INT32)", true, true},
          {"INT64", "1", true, true},
          {"UINT32", "CAST(1 AS UINT32)", true, true},
          {"UINT64", "CAST(1 AS UINT64)", true, true},
          {"FLOAT", "CAST(1.0 AS FLOAT)", true, true},
          {"DOUBLE", "1.0", true, true},
          {"NUMERIC", "CAST(1.0 AS NUMERIC)", true, true},
          {"BIGNUMERIC", "CAST(1.0 AS BIGNUMERIC)", true, true},
          {"STRING", "'abc'", true, true},
          {"COLLATED_STRING", "COLLATE('abc', 'und:ci')", true, true},
          {"COLLATED_ARRAY_STRING", "[COLLATE('abc', 'und:ci')]", true, true},
          {"DATE", "DATE '2025-12-31'", true, true},
          {"TIMESTAMP", "TIMESTAMP '1987-05-31 14:21:00'", true, true},
          {"DATETIME", "DATETIME '2025-12-31 12:34:56'", true, true},
          {"TIME", "TIME '12:34:56'", true, true},
          {"UUID", "CAST('00000000-0000-0000-0000-000000000000' AS UUID)", true,
           true},
          {"ARRAY", "[1, 2]", true, true},
          {"ARRAY_JSON", "ARRAY<JSON>[JSON '1', JSON '2']", true, true},

          // Types that support equality but not ordering when cast to JSON.
          {"BYTES", "b'abc'", true, false},
          {"RANGE", "RANGE<DATE> '[2025-01-01, 2026-01-01)'", true, false},
          {"ARRAY_BYTES", "[b'abc', b'def']", true, false},
          {"ARRAY_ENUM", "[CAST('TESTENUM1' AS googlesql_test.TestEnum)]", true,
           false},
          {"ENUM", "CAST('TESTENUM1' AS googlesql_test.TestEnum)", true, false},

          // Unsupported types for equality and ordering when cast to JSON.
          {"INTERVAL", "INTERVAL '1' HOUR", false, false},
          {"STRUCT", "STRUCT(1 AS a, 'b' AS b)", false, false},
          {"GEOGRAPHY", "ST_GEOGFROMTEXT('POINT(1 1)')", false, false}};
}

using JSONComparisonAnalyzerTestWithParam =
    ::testing::TestWithParam<JSONComparisonTestParam>;

TEST_P(JSONComparisonAnalyzerTestWithParam, LiteralEquality) {
  const auto& param = GetParam();
  SCOPED_TRACE(param.name);
  std::string sql = absl::StrCat("SELECT JSON '1' = ", param.literal_sql);
  if (param.expect_equality_success) {
    EXPECT_THAT(Analyze(sql), IsOk()) << "SQL: " << sql;
  } else {
    EXPECT_THAT(Analyze(sql), Not(IsOk())) << "SQL: " << sql;
  }
}

TEST_P(JSONComparisonAnalyzerTestWithParam, ReversedLiteralEquality) {
  const auto& param = GetParam();
  SCOPED_TRACE(param.name);
  std::string sql = absl::StrCat("SELECT ", param.literal_sql, " = JSON '1'");
  if (param.expect_equality_success) {
    EXPECT_THAT(Analyze(sql), IsOk()) << "SQL: " << sql;
  } else {
    EXPECT_THAT(Analyze(sql), Not(IsOk())) << "SQL: " << sql;
  }
}

TEST_P(JSONComparisonAnalyzerTestWithParam, ColumnEquality) {
  const auto& param = GetParam();
  SCOPED_TRACE(param.name);
  std::string sql = absl::StrCat(
      "WITH MyTable AS (SELECT JSON 'null' AS json_col, ", param.literal_sql,
      " AS other_col) ", "SELECT json_col = other_col FROM MyTable");
  if (param.expect_equality_success) {
    EXPECT_THAT(Analyze(sql), IsOk()) << "SQL: " << sql;
  } else {
    EXPECT_THAT(Analyze(sql), Not(IsOk())) << "SQL: " << sql;
  }
}

TEST_P(JSONComparisonAnalyzerTestWithParam, InMembership) {
  const auto& param = GetParam();
  SCOPED_TRACE(param.name);
  std::string sql = absl::StrCat("SELECT JSON '1' IN (", param.literal_sql,
                                 ", ", param.literal_sql, ")");
  if (param.expect_equality_success) {
    EXPECT_THAT(Analyze(sql), IsOk()) << "SQL: " << sql;
  } else {
    EXPECT_THAT(Analyze(sql), Not(IsOk())) << "SQL: " << sql;
  }
}

std::vector<QueryTmpl> GetInMembershipWithMoreThanTwoElementsQueries() {
  return {
      {"Ints", "SELECT JSON '1' IN (1, 2, 3)"},
      {"IntJsonInt", "SELECT JSON '1' IN (1, JSON '2', 3)"},
      {"IntIntJsonInt", "SELECT JSON '1' IN (1, 2, JSON '3', 4)"},
  };
}

using JSONInMembershipWithMoreThanTwoElementsTest =
    ::testing::TestWithParam<QueryTmpl>;

TEST_P(JSONInMembershipWithMoreThanTwoElementsTest, AnalyzeSucceeds) {
  const auto& query_tmpl = GetParam();
  SCOPED_TRACE(query_tmpl.name);
  EXPECT_THAT(Analyze(query_tmpl.sql), IsOk()) << "SQL: " << query_tmpl.sql;
}

INSTANTIATE_TEST_SUITE_P(
    JSONInMembershipWithMoreThanTwoElementsTests,
    JSONInMembershipWithMoreThanTwoElementsTest,
    ::testing::ValuesIn(GetInMembershipWithMoreThanTwoElementsQueries()),
    [](const ::testing::TestParamInfo<QueryTmpl>& info) {
      return info.param.name;
    });

TEST_P(JSONComparisonAnalyzerTestWithParam, CastAsJSON) {
  const auto& param = GetParam();
  if (!param.expect_equality_success) {
    return;
  }
  SCOPED_TRACE(param.name);
  std::string sql =
      absl::StrCat("SELECT CAST(", param.literal_sql, " AS JSON)");
  // CAST as JSON is supported for types that are equality coercible.
  EXPECT_THAT(Analyze(sql), IsOk()) << "SQL: " << sql;
}

std::vector<QueryTmpl> GetLiteralOrderingQueryTmpls() {
  return {
      {"JsonLtLiteral", "SELECT JSON '1' < $0"},
      {"JsonLeLiteral", "SELECT JSON '1' <= $0"},
      {"JsonGreaterThanLiteral", "SELECT JSON '1' > $0"},
      {"JsonGeLiteral", "SELECT JSON '1' >= $0"},
      {"JsonBetweenLiteral", "SELECT JSON '2' BETWEEN JSON '1' AND $0"},
      {"LiteralBetweenJson", "SELECT JSON '2' BETWEEN $0 AND 3"},
      {"LiteralLtJson", "SELECT $0 < JSON '1'"},
      {"LiteralLeJson", "SELECT $0 <= JSON '1'"},
      {"LiteralGtJson", "SELECT $0 > JSON '1'"},
      {"LiteralGeJson", "SELECT $0 >= JSON '1'"},
  };
}
std::vector<QueryTmpl> GetColumnOrderingQueryTmpls() {
  return {
      {"JsonColLtOtherCol",
       "WITH MyTable AS (SELECT JSON 'true' AS json_col, $0 AS other_col) "
       "SELECT json_col < other_col FROM MyTable"},
      {"OtherColLtJsonCol",
       "WITH MyTable AS (SELECT JSON '1' AS json_col, $0 AS other_col) "
       "SELECT other_col < json_col FROM MyTable"},
      {"JsonColBetweenOtherCol",
       "WITH MyTable AS (SELECT JSON 'null' AS json_col, $0 AS other_col) "
       "SELECT json_col BETWEEN JSON '2' AND other_col FROM MyTable"},
  };
}

using JSONOrderingAnalyzerTest =
    ::testing::TestWithParam<std::tuple<JSONComparisonTestParam, QueryTmpl>>;

TEST_P(JSONOrderingAnalyzerTest, OrderingSupported) {
  const auto& [param, query_tmpl] = GetParam();
  SCOPED_TRACE(absl::StrCat(param.name, "_", query_tmpl.name));
  std::string sql = absl::Substitute(query_tmpl.sql, param.literal_sql);
  if (param.expect_order_success) {
    EXPECT_THAT(Analyze(sql), IsOk()) << "SQL: " << sql;
  } else {
    EXPECT_THAT(Analyze(sql), Not(IsOk())) << "SQL: " << sql;
  }
}

INSTANTIATE_TEST_SUITE_P(
    JSONLiteralOrderingAnalyzerTests, JSONOrderingAnalyzerTest,
    ::testing::Combine(::testing::ValuesIn(GetTestParams()),
                       ::testing::ValuesIn(GetLiteralOrderingQueryTmpls())),
    ([](const ::testing::TestParamInfo<JSONOrderingAnalyzerTest::ParamType>&
            info) {
      const auto& [param, query_tmpl] = info.param;
      return absl::StrCat(param.name, "_", query_tmpl.name);
    }));

INSTANTIATE_TEST_SUITE_P(
    JSONColumnOrderingAnalyzerTests, JSONOrderingAnalyzerTest,
    ::testing::Combine(::testing::ValuesIn(GetTestParams()),
                       ::testing::ValuesIn(GetColumnOrderingQueryTmpls())),
    ([](const ::testing::TestParamInfo<JSONOrderingAnalyzerTest::ParamType>&
            info) {
      const auto& [param, query_tmpl] = info.param;
      return absl::StrCat(param.name, "_", query_tmpl.name);
    }));

INSTANTIATE_TEST_SUITE_P(
    JSONEqualityComparisonAnalyzerTests, JSONComparisonAnalyzerTestWithParam,
    ::testing::ValuesIn(GetTestParams()),
    [](const ::testing::TestParamInfo<JSONComparisonTestParam>& info) {
      return info.param.name;
    });

TEST(JSONComparisonAnalyzerTest, MixedTypesBetweenSucceeds) {
  std::string sql = "SELECT JSON '2' BETWEEN 1 AND '3'";
  EXPECT_THAT(Analyze(sql), IsOk()) << "SQL: " << sql;
}

std::vector<QueryTmpl> GetNullOrderingQueries() {
  return {
      {"JsonLtNullInt", "SELECT JSON '1' < CAST(NULL AS INT64)"},
      {"NullJsonLtInt", "SELECT CAST(NULL AS JSON) < 1"},
      {"NullJsonBetweenInts", "SELECT CAST(NULL AS JSON) BETWEEN 1 AND 2"},
      {"JsonBetweenNullIntAndInt",
       "SELECT JSON '1' BETWEEN CAST(NULL AS INT64) AND 2"},
  };
}

using JSONNullOrderingTest = ::testing::TestWithParam<QueryTmpl>;

TEST_P(JSONNullOrderingTest, AnalyzeSucceeds) {
  const auto& query_tmpl = GetParam();
  SCOPED_TRACE(query_tmpl.name);
  EXPECT_THAT(Analyze(query_tmpl.sql), IsOk())
      << "SQL should pass: " << query_tmpl.sql;
}

INSTANTIATE_TEST_SUITE_P(JSONNullOrderingTests, JSONNullOrderingTest,
                         ::testing::ValuesIn(GetNullOrderingQueries()),
                         [](const ::testing::TestParamInfo<QueryTmpl>& info) {
                           return info.param.name;
                         });

std::vector<QueryTmpl> GetUnsupportedOperationsQueries() {
  return {
      {"IntBetweenJsonAndJson", "SELECT 2 BETWEEN JSON '1' AND JSON '3'"},
      {"IntBetweenJsonAndInt", "SELECT 2 BETWEEN JSON '1' AND 3"},
      {"StringBetweenJsonAndJson",
       "SELECT 'abc' BETWEEN JSON '1' AND JSON '3'"},
      {"JsonInUnnestInt", "SELECT JSON '1' IN UNNEST([1, 2, 3])"},
      {"JsonInSubqueryInt", "SELECT JSON '1' IN (SELECT 1 UNION ALL SELECT 2)"},
      {"IntInJsonJson", "SELECT 1 IN (JSON '1', JSON '2')"},
      {"IntInJsonJsonJson", "SELECT 1 IN (JSON '1', JSON '2', JSON '3')"},
      {"IntInJsonInt", "SELECT 1 IN (JSON '1', 2)"},
      {"IntInJsonInt2", "SELECT 1 IN (1.0, JSON '1', 2)"},
      {"JsonEqInterval",
       "SELECT JSON '\"P1Y2M3DT4H5M6.7S\"' = INTERVAL '1-2 3 4:5:6.7' YEAR TO "
       "SECOND"},
      {"JsonEqStructWithNull",
       "SELECT JSON '{\"a\":1,\"b\":null}' = STRUCT(1 AS a, CAST(NULL AS "
       "INT64) "
       "AS b)"},
      {"JsonEqStructWithTwoFields",
       "SELECT JSON '{\"a\": 1}' = STRUCT(1 AS a, \"abc\" AS b)"},
      {"JsonInStruct",
       "SELECT JSON '{\"a\": 1}' IN (STRUCT(1 AS a, \"abc\" AS b))"},
      {"JsonArrayEqGeographyArray",
       "SELECT JSON '[1]' = [ST_GEOGFROMTEXT('POINT(1 1)')]"},

      // General quantified comparisons are unsupported with JSON and non-JSON
      // types.
      {"JsonEqAnySubquery",
       "SELECT JSON '1' = ANY(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonEqAnyArray", "SELECT JSON '1' = ANY(ARRAY[1, 2])"},
      {"JsonEqAllSubquery",
       "SELECT JSON '1' = ALL(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonEqAllArray", "SELECT JSON '1' = ALL(ARRAY[1, 2])"},
      {"JsonNeAnySubquery",
       "SELECT JSON '1' != ANY(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonNeAnyArray", "SELECT JSON '1' != ANY(ARRAY[1, 2])"},
      {"JsonNeAllSubquery",
       "SELECT JSON '1' != ALL(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonNeAllArray", "SELECT JSON '1' != ALL(ARRAY[1, 2])"},
      {"JsonLtAnySubquery",
       "SELECT JSON '1' < ANY(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonLtAnyArray", "SELECT JSON '1' < ANY(ARRAY[1, 2])"},
      {"JsonLtAllSubquery",
       "SELECT JSON '1' < ALL(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonLtAllArray", "SELECT JSON '1' < ALL(ARRAY[1, 2])"},
      {"JsonLeAnySubquery",
       "SELECT JSON '1' <= ANY(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonLeAnyArray", "SELECT JSON '1' <= ANY(ARRAY[1, 2])"},
      {"JsonLeAllSubquery",
       "SELECT JSON '1' <= ALL(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonLeAllArray", "SELECT JSON '1' <= ALL(ARRAY[1, 2])"},
      {"JsonGtAnySubquery",
       "SELECT JSON '1' > ANY(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonGtAnyArray", "SELECT JSON '1' > ANY(ARRAY[1, 2])"},
      {"JsonGtAllSubquery",
       "SELECT JSON '1' > ALL(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonGtAllArray", "SELECT JSON '1' > ALL(ARRAY[1, 2])"},
      {"JsonGeAnySubquery",
       "SELECT JSON '1' >= ANY(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonGeAnyArray", "SELECT JSON '1' >= ANY(ARRAY[1, 2])"},
      {"JsonGeAllSubquery",
       "SELECT JSON '1' >= ALL(SELECT 1 UNION ALL SELECT 2)"},
      {"JsonGeAllArray", "SELECT JSON '1' >= ALL(ARRAY[1, 2])"},
  };
}

using JSONUnsupportedOperationsTest = ::testing::TestWithParam<QueryTmpl>;

TEST_P(JSONUnsupportedOperationsTest, UnsupportedOperationsFail) {
  const auto& query_tmpl = GetParam();
  SCOPED_TRACE(query_tmpl.name);
  EXPECT_THAT(Analyze(query_tmpl.sql), Not(IsOk()))
      << "SQL should fail: " << query_tmpl.sql;
}

INSTANTIATE_TEST_SUITE_P(JSONUnsupportedOperationsTests,
                         JSONUnsupportedOperationsTest,
                         ::testing::ValuesIn(GetUnsupportedOperationsQueries()),
                         [](const ::testing::TestParamInfo<QueryTmpl>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace googlesql
