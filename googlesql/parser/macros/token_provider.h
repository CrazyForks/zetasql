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

#ifndef GOOGLESQL_PARSER_MACROS_TOKEN_PROVIDER_H_
#define GOOGLESQL_PARSER_MACROS_TOKEN_PROVIDER_H_

#include <memory>
#include <optional>

#include "googlesql/parser/macros/token_provider_base.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/parser/tokenizer.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace parser {
namespace macros {

// Provides the next token from a tokenizer without any macro expansion.
// This is the normal case, where we only have the text and we need to
// tokenize it from the start.
class TokenProvider : public TokenProviderBase {
 public:
  TokenProvider(absl::string_view filename, absl::string_view input,
                int start_offset, std::optional<int> end_offset,
                int offset_in_original_input);

  TokenProvider(const TokenProvider&) = delete;
  TokenProvider& operator=(const TokenProvider&) = delete;

  std::unique_ptr<TokenProviderBase> CreateNewInstance(
      absl::string_view filename, absl::string_view input, int start_offset,
      std::optional<int> end_offset,
      int offset_in_original_input) const override;

  // Returns Lookahead 1 (token N+1) without consuming from the input.
  //
  // Token output rules:
  // - If a previous call to `GetNextToken()` or `PeekNextToken()` returned an
  //   error status or an End-of-Input (EOI) token, which can be because:
  //   - The underlying token stream produced an error.
  //   - The stream reached the real end of input.
  //
  //   all future calls to `PeekNextToken()` and `GetNextToken()` return the
  //   exact same error status, or an EOI token with empty preceding
  //   whitespaces.
  absl::StatusOr<TokenWithLocation> PeekNextToken() override;

  // Same as `PeekNextToken()`.
  absl::StatusOr<TokenWithLocation> PeekLookahead1() override {
    return PeekNextToken();
  }

  // Returns Lookahead 2 (token N+2) without consuming from the input.
  //
  // Lookahead invariants:
  // - If `PeekNextToken()` or a lookahead is an error status or Token::EOI, all
  //   further lookaheads return the exact same error status or Token::EOI with
  //   empty preceding whitespaces.
  absl::StatusOr<TokenWithLocation> PeekLookahead2() override;

  // Returns Lookahead 3 (token N+3) without consuming from the input.
  //
  // Lookahead invariants:
  // - If `PeekLookahead2()` or a lookahead is an error status or Token::EOI,
  //   all further lookaheads return the exact same error status or Token::EOI
  //   with empty preceding whitespaces.
  absl::StatusOr<TokenWithLocation> PeekLookahead3() override;

 protected:
  // Consumes the next token from the buffer, or pull one from Flex if the
  // buffer is empty.
  absl::StatusOr<TokenWithLocation> ConsumeNextTokenImpl() override;

 private:
  // Helper method to fetch the next lookahead token or propagate EOI / errors.
  absl::StatusOr<TokenWithLocation> FetchNextLookahead(
      const absl::StatusOr<TokenWithLocation>& prev);

  // Pulls the next token from the lexer.
  absl::StatusOr<TokenWithLocation> GetToken();

  // The GoogleSQL tokenizer which gives us all the tokens.
  std::unique_ptr<GoogleSqlTokenizer> tokenizer_;

  // Fixed lookahead buffers storing up to three lookahead tokens (1, 2, 3).
  // Any tokens here are still unprocessed by the expander.
  absl::StatusOr<TokenWithLocation> lookahead_1_;
  absl::StatusOr<TokenWithLocation> lookahead_2_;
  absl::StatusOr<TokenWithLocation> lookahead_3_;
};

}  // namespace macros
}  // namespace parser
}  // namespace googlesql

#endif  // GOOGLESQL_PARSER_MACROS_TOKEN_PROVIDER_H_
