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

#include "googlesql/parser/keywords.h"

#include <cstdlib>
#include <set>
#include <string>

#include "googlesql/parser/tm_parser.h"
#include "googlesql/parser/tm_token.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace parser {
namespace {

using ::testing::ContainerEq;

TEST(GetKeywordInfo, Hit) {
  const KeywordInfo* info = GetKeywordInfo("select");
  ASSERT_TRUE(info != nullptr);
  EXPECT_TRUE(info->CanBeReserved());
  EXPECT_TRUE(info->IsAlwaysReserved());

  info = GetKeywordInfo("row");
  ASSERT_TRUE(info != nullptr);
  EXPECT_FALSE(info->CanBeReserved());
  EXPECT_FALSE(info->IsAlwaysReserved());

  info = GetKeywordInfo("qualify");
  ASSERT_TRUE(info != nullptr);
  EXPECT_TRUE(info->CanBeReserved());
  EXPECT_FALSE(info->IsAlwaysReserved());
}

TEST(GetKeywordInfo, NonHit) {
  const KeywordInfo* info = GetKeywordInfo("selected");
  EXPECT_FALSE(info != nullptr);
}

// Gets token names for reserved or non-reserved keywords depending on
// 'reserved', in lowercase.
//
// Keywords are returned here, as they appear in the Bison grammar
// (e.g. "kw_qualify_reserved" or "kw_qualify_nonreserved", not "qualify").
std::set<std::string> GetKeywordsSet(bool reserved) {
  std::set<std::string> result;
  for (const KeywordInfo& keyword_info : GetAllKeywords()) {
    if (keyword_info.IsConditionallyReserved()) {
      std::string keyword =
          absl::StrCat("kw_", keyword_info.keyword(),
                       reserved ? "_reserved" : "_nonreserved");
      result.insert(absl::AsciiStrToUpper(keyword));
    } else if (keyword_info.IsAlwaysReserved() == reserved) {
      result.insert(absl::AsciiStrToUpper(keyword_info.keyword()));
    }
  }
  return result;
}

TEST(GetAllKeywords, NonReservedMatchesGrammarKeywordAsIdentifier) {
  std::set<std::string> grammar_non_reserved_kws;
  for (const Token token : Parser::NonReservedKeywordTokens()) {
    grammar_non_reserved_kws.emplace(tokenName[static_cast<size_t>(token)]);
  }
  std::set<std::string> non_reserved_kws = GetKeywordsSet(/*reserved=*/false);
  EXPECT_THAT(grammar_non_reserved_kws, ContainerEq(non_reserved_kws));
}

TEST(GetAllKeywords, NonReservedMatchesGrammarNonReserved) {
  std::set<std::string> lexer_non_reserved_kws;
  for (size_t t = static_cast<size_t>(Token::SENTINEL_NONRESERVED_KW_START) + 1;
       t < static_cast<size_t>(Token::SENTINEL_NONRESERVED_KW_END); ++t) {
    lexer_non_reserved_kws.emplace(tokenName[t]);
  }
  std::set<std::string> non_reserved_kws = GetKeywordsSet(/*reserved=*/false);
  EXPECT_THAT(lexer_non_reserved_kws, ContainerEq(non_reserved_kws));
}

TEST(GetAllKeywords, ReservedKeywordsMatchGrammarReserved) {
  std::set<std::string> lexer_reserved_keywords;
  for (size_t t = static_cast<size_t>(Token::SENTINEL_RESERVED_KW_START) + 1;
       t < static_cast<size_t>(Token::SENTINEL_RESERVED_KW_END); ++t) {
    lexer_reserved_keywords.emplace(tokenName[t]);
  }

  std::set<std::string> reserved_kws = GetKeywordsSet(/*reserved=*/true);

  // TODO: "kw_define_for_macros" is a special form of "DEFINE" used only under
  // the "define macro" context. GetKeywordsSet() contains (1) the tokens that
  // can be produced by the lexer, and (2) their reserved variants, if
  // applicable, so it doesn't include "kw_define_for_macros". We should
  // evaluate whether we should remove "kw_define_for_macros" from the
  // "SENTINEL_RESERVED_KW" section.
  reserved_kws.insert("KW_DEFINE_FOR_MACROS");

  EXPECT_THAT(reserved_kws, ContainerEq(lexer_reserved_keywords));
}

TEST(ParserTest, DontAddNewReservedKeywords) {
  int num_reserved = 0;
  for (const KeywordInfo& keyword_info : GetAllKeywords()) {
    if (keyword_info.IsAlwaysReserved()) {
      ++num_reserved;
    }
  }
  // *** BE VERY CAREFUL CHANGING THIS. ***
  // Adding reserved keywords is a breaking change.  Removing reserved keywords
  // allows new queries to work that will not work on older code.
  // Before changing this, co-ordinate with all engines to make sure the change
  // is done safely.
  //
  // New reserved keywords should generally be marked as conditionally reserved
  // instead, so that engines desiring backward compatibility can opt out.
  EXPECT_EQ(95 /* CAUTION */, num_reserved);
}

}  // namespace
}  // namespace parser
}  // namespace googlesql
