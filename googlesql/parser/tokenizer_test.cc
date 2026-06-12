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

#include "googlesql/parser/tokenizer.h"

#include <string>

#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace parser {
namespace {

using ::absl_testing::IsOkAndHolds;

static constexpr absl::string_view kFileName = "test.sql";

TEST(GoogleSqlTokenizerTest, BasicTokenization) {
  absl::string_view input = "SELECT 123";
  GoogleSqlTokenizer tokenizer(kFileName, input, /*start_offset=*/0);

  auto first_token = tokenizer.GetNextToken();
  ASSERT_THAT(first_token, IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                                       Token::KW_SELECT)));
  EXPECT_EQ(first_token->text, "SELECT");
  EXPECT_EQ(first_token->preceding_whitespaces, "");
  EXPECT_EQ(first_token->location.start().GetByteOffset(), 0);
  EXPECT_EQ(first_token->location.end().GetByteOffset(), 6);

  auto second_token = tokenizer.GetNextToken();
  ASSERT_THAT(second_token,
              IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                          Token::DECIMAL_INTEGER_LITERAL)));
  EXPECT_EQ(second_token->text, "123");
  EXPECT_EQ(second_token->preceding_whitespaces, " ");
  EXPECT_EQ(second_token->location.start().GetByteOffset(), 7);
  EXPECT_EQ(second_token->location.end().GetByteOffset(), 10);

  auto third_token = tokenizer.GetNextToken();
  ASSERT_THAT(third_token, IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                                       Token::EOI)));
  EXPECT_EQ(third_token->text, "");
  EXPECT_EQ(third_token->preceding_whitespaces, "");
  EXPECT_EQ(third_token->location.start().GetByteOffset(), 10);
  EXPECT_EQ(third_token->location.end().GetByteOffset(), 10);
}

TEST(GoogleSqlTokenizerTest, TokenizationWithOffsetAndFollowingWhitespace) {
  absl::string_view input = "SELECT 1; SELECT 2";
  // We want to tokenize " SELECT 2" which starts at index 9.
  int start_offset = 9;
  GoogleSqlTokenizer tokenizer(kFileName, input, start_offset);

  auto first_token = tokenizer.GetNextToken();
  ASSERT_THAT(first_token, IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                                       Token::KW_SELECT)));
  EXPECT_EQ(first_token->text, "SELECT");
  EXPECT_EQ(first_token->preceding_whitespaces, " ");
  EXPECT_EQ(first_token->location.start().GetByteOffset(), 10);
  EXPECT_EQ(first_token->location.end().GetByteOffset(), 16);

  auto second_token = tokenizer.GetNextToken();
  ASSERT_THAT(second_token,
              IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                          Token::DECIMAL_INTEGER_LITERAL)));
  EXPECT_EQ(second_token->text, "2");
  EXPECT_EQ(second_token->preceding_whitespaces, " ");
  EXPECT_EQ(second_token->location.start().GetByteOffset(), 17);
  EXPECT_EQ(second_token->location.end().GetByteOffset(), 18);
}

TEST(GoogleSqlTokenizerTest, TokenizationWithOffset) {
  absl::string_view input = "SELECT 1; SELECT 2";
  // We want to tokenize "SELECT 2" which starts at index 9.
  int start_offset = 10;
  GoogleSqlTokenizer tokenizer(kFileName, input, start_offset);

  auto first_token = tokenizer.GetNextToken();
  ASSERT_THAT(first_token, IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                                       Token::KW_SELECT)));
  EXPECT_EQ(first_token->text, "SELECT");
  // Whitespace before SELECT does not include whitespace before the start
  // offset.
  EXPECT_EQ(first_token->preceding_whitespaces, "");
  EXPECT_EQ(first_token->location.start().GetByteOffset(), 10);
  EXPECT_EQ(first_token->location.end().GetByteOffset(), 16);

  auto second_token = tokenizer.GetNextToken();
  ASSERT_THAT(second_token,
              IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                          Token::DECIMAL_INTEGER_LITERAL)));
  EXPECT_EQ(second_token->text, "2");
  EXPECT_EQ(second_token->preceding_whitespaces, " ");
  EXPECT_EQ(second_token->location.start().GetByteOffset(), 17);
  EXPECT_EQ(second_token->location.end().GetByteOffset(), 18);
}

TEST(GoogleSqlTokenizerTest, ConsumedTokensCount) {
  absl::string_view input = "SELECT 1";
  GoogleSqlTokenizer tokenizer(kFileName, input, 0);

  EXPECT_EQ(tokenizer.num_consumed_tokens(), 0);
  GOOGLESQL_ASSERT_OK(tokenizer.GetNextToken());
  EXPECT_EQ(tokenizer.num_consumed_tokens(), 1);
  GOOGLESQL_ASSERT_OK(tokenizer.GetNextToken());
  EXPECT_EQ(tokenizer.num_consumed_tokens(), 2);
  GOOGLESQL_ASSERT_OK(tokenizer.GetNextToken());  // EOI
  EXPECT_EQ(tokenizer.num_consumed_tokens(), 3);
}

TEST(GoogleSqlTokenizerTest, EmptyInputWithOffset) {
  absl::string_view input = "SELECT 1;";
  // start_offset is at the end of the input.
  int start_offset = 9;
  GoogleSqlTokenizer tokenizer(kFileName, input, start_offset);

  auto token = tokenizer.GetNextToken();
  ASSERT_THAT(token, IsOkAndHolds(
                         testing::Field(&TokenWithLocation::kind, Token::EOI)));
  EXPECT_EQ(token->text, "");
  EXPECT_EQ(token->preceding_whitespaces, "");
  EXPECT_EQ(token->location.start().GetByteOffset(), 9);
  EXPECT_EQ(token->location.end().GetByteOffset(), 9);
}

TEST(GoogleSqlTokenizerTest, TokenAtStartOfSubstr) {
  absl::string_view input = "SELECT 1;SELECT 2";
  // The second "SELECT" starts exactly at index 9.
  int start_offset = 9;
  GoogleSqlTokenizer tokenizer(kFileName, input, start_offset);

  auto token = tokenizer.GetNextToken();
  ASSERT_THAT(token, IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                                 Token::KW_SELECT)));
  EXPECT_EQ(token->text, "SELECT");
  EXPECT_EQ(token->preceding_whitespaces, "");
  EXPECT_EQ(token->location.start().GetByteOffset(), 9);
  EXPECT_EQ(token->location.end().GetByteOffset(), 15);
}

TEST(GoogleSqlTokenizerTest, TokenizesBackslash) {
  absl::string_view input = "\\";
  GoogleSqlTokenizer tokenizer(kFileName, input, 0);

  auto first_token = tokenizer.GetNextToken();
  ASSERT_THAT(first_token, IsOkAndHolds(testing::Field(&TokenWithLocation::kind,
                                                       Token::BACKSLASH)));
  EXPECT_EQ(first_token->text, "\\");
  EXPECT_EQ(first_token->location.start().GetByteOffset(), 0);
  EXPECT_EQ(first_token->location.end().GetByteOffset(), 1);
}

}  // namespace
}  // namespace parser
}  // namespace googlesql
