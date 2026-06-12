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

#ifndef GOOGLESQL_PARSER_TOKENIZER_H_
#define GOOGLESQL_PARSER_TOKENIZER_H_

#include "googlesql/parser/tm_lexer.h"
#include "googlesql/parser/token_stream.h"
#include "googlesql/parser/token_with_location.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace parser {

// A wrapper class for the generated TextMapper lexer class with access to
// the private fields of `Lexer`. This class adapts the Textmapper lexer's API
// to GoogleSql's TokenStream interface.
class GoogleSqlTokenizer final : Lexer, public TokenStream {
 public:
  GoogleSqlTokenizer(absl::string_view filename, absl::string_view input,
                     int start_offset);

  GoogleSqlTokenizer(const GoogleSqlTokenizer&) = delete;
  GoogleSqlTokenizer& operator=(const GoogleSqlTokenizer&) = delete;

  absl::StatusOr<TokenWithLocation> GetNextToken() override;
  int num_consumed_tokens() const override { return num_consumed_tokens_; }

 private:
  absl::string_view input_;
  int num_consumed_tokens_ = 0;
  int last_token_end_offset_ = 0;
};

}  // namespace parser
}  // namespace googlesql

#endif  // GOOGLESQL_PARSER_TOKENIZER_H_
