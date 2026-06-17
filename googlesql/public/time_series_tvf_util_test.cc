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

#include "googlesql/public/time_series_tvf_util.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "googlesql/public/testing/error_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace {

using ::testing::HasSubstr;

struct ParseSimpleIdentifierPathTestCase {
  std::string name;
  std::string input;
  std::vector<std::string> expected_output;
  std::string expected_error;  // If empty, the test is expected to succeed
};

class ParseSimpleIdentifierPathTest
    : public ::testing::TestWithParam<ParseSimpleIdentifierPathTestCase> {};

// TODO - Add a fuzzer test for ParseSimpleIdentifierPath.
TEST_P(ParseSimpleIdentifierPathTest, ParseSimpleIdentifierPath) {
  const auto& test = GetParam();

  // Prepare a non-null-terminated string view of the input to verify scanner
  // boundary safety.
  std::unique_ptr<char[]> input_buf(new char[test.input.size()]());
  std::memcpy(input_buf.get(), test.input.c_str(), test.input.size());
  absl::string_view input_no_null_term(input_buf.get(), test.input.size());

  if (test.expected_error.empty()) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> result,
                         ParseSimpleIdentifierPath(test.input));
    EXPECT_EQ(result, test.expected_output);

    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> result_no_term,
                         ParseSimpleIdentifierPath(input_no_null_term));
    EXPECT_EQ(result_no_term, test.expected_output);
  } else {
    absl::StatusOr<std::vector<std::string>> result =
        ParseSimpleIdentifierPath(test.input);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(result.status().message(), HasSubstr(test.expected_error));

    absl::StatusOr<std::vector<std::string>> result_no_term =
        ParseSimpleIdentifierPath(input_no_null_term);
    EXPECT_THAT(result_no_term, StatusIs(absl::StatusCode::kInvalidArgument,
                                         HasSubstr(test.expected_error)));
  }
}

INSTANTIATE_TEST_SUITE_P(
    ParseSimpleIdentifierPathTests, ParseSimpleIdentifierPathTest,
    ::testing::ValuesIn(std::vector<ParseSimpleIdentifierPathTestCase>{
        // ==========================================
        // Success Cases
        // ==========================================
        // {"name", "input", {"expected_output"}, "expected_error" (empty if
        // success)}
        {"SingleIdentifier", "abc", {"abc"}},
        {"SingleDot", "abc.def", {"abc", "def"}},
        {"MultiDottedPath", "a.b.c", {"a", "b", "c"}},
        {"NumericPathTicks", "abc.`123`", {"abc", "123"}},
        {"NumericPathNoTicks", "abc.123", {"abc", "123"}},
        {"NumericPathBothTicks", "abc.`123`.456", {"abc", "123", "456"}},

        // Keywords are parsed as normal identifiers (not treated as reserved)
        {"UnquotedKeywords", "select.from", {"select", "from"}},
        {"QuotedAndUnquotedKeywords",
         "abc.`select`.from.`table`",
         {"abc", "select", "from", "table"}},

        {"SpacesInBackquotes", "`a c`.def.`g h`", {"a c", "def", "g h"}},
        {"DotInBackquotes", "abc.def.`ghi.jkl`", {"abc", "def", "ghi.jkl"}},
        {"QuotedNonAscii", "`你好，世界`", {"你好，世界"}},
        {"QuotedNonAsciiInner", "abc.`你好，世界`", {"abc", "你好，世界"}},
        {"SlashInQuoted", "`abc/def`", {"abc/def"}},
        {"BackslashInQuotedUnprocessed", "`abc\\def`", {"abc\\def"}},
        {"HexEscapeInQuoted", "`abc\\x2e\\x60`", {"abc\\x2e\\x60"}},

        // ==========================================
        // Failure Cases
        // ==========================================
        // {"name", "input", {"expected_output"} (empty if error),
        // "expected_error"}
        {"NonAsciiUnquoted", "abc.äöü", {}, "invalid character"},
        {"EmptyPath", "", {}, "cannot be empty"},
        {"LeadingDot", ".abc", {}, "cannot begin with '.'"},
        {"TrailingDot", "abc.", {}, "cannot end with '.'"},
        {"EmptyPathComponent", "abc..def", {}, "empty component"},
        {"EmptyQuotedPathComponent", "abc.``", {}, "empty component"},

        // Digits not allowed at the start of the first path component.
        {"PathStartsWithNumber0", "123", {}, "start with a digit, found '1'"},
        {"PathStartsWithNumber1", "123a", {}, "start with a digit, found '1'"},
        {"PathStartsWithNumber2", "123.a", {}, "start with a digit, found '1'"},

        // Unclosed Backticks.
        {"UnmatchedBackquoteStart", "`abc", {}, "unmatched backtick '`'"},
        {"UnmatchedBackquoteMiddle", "abc.`def", {}, "unmatched backtick '`'"},

        // Unquoted slashes and backslashes are disallowed.
        {"SlashInUnquoted", "abc/def", {}, "invalid character '/'"},
        {"BackslashInUnquoted", "abc\\def", {}, "invalid character '\\'"},

        // Backquotes in invalid positions.
        {"BackquoteInMiddleOfSegment",
         "a`b`",
         {},
         "only allowed at the start of a path component"},

        {"BackquoteInMiddleOfSegment2",
         "abc.d`ef`",
         {},
         "only allowed at the start of a path component"},

        {"TextAfterClosingBackquote",
         "`abc`def",
         {},
         "Expected '.' or end of string after closing backtick"},

        {"BackquoteConsecutive",
         "`abc``def`",
         {},
         "Expected '.' or end of string after closing backtick"},

        // Invalid characters not quoted.
        {"AllWhitespace", "     ", {}, "invalid character"},
        {"StarWithinComponent", "abc.123*def", {}, "invalid character"},
        {"PercentStartComponent", "abc.%def", {}, "invalid character"},
        {"PoundEndComponent", "abc.def#", {}, "invalid character"},
    }),
    [](const ::testing::TestParamInfo<ParseSimpleIdentifierPathTest::ParamType>&
           info) { return info.param.name; });

}  // namespace
}  // namespace googlesql
