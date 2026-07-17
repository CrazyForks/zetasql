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

#include "googlesql/parser/macros/token_provider.h"

#include <memory>
#include <optional>
#include <utility>

#include "googlesql/parser/macros/token_provider_base.h"
#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/parser/tokenizer.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/base/check.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace parser {
namespace macros {

TokenProvider::TokenProvider(absl::string_view filename,
                             absl::string_view input, int start_offset,
                             std::optional<int> end_offset,
                             int offset_in_original_input)
    : TokenProviderBase(filename, input, start_offset, end_offset,
                        offset_in_original_input),
      tokenizer_(std::make_unique<GoogleSqlTokenizer>(
          filename, input.substr(0, this->end_offset()), start_offset)) {
  lookahead_1_ = GetToken();
  lookahead_2_ = FetchNextLookahead(lookahead_1_);
  lookahead_3_ = FetchNextLookahead(lookahead_2_);
}

std::unique_ptr<TokenProviderBase> TokenProvider::CreateNewInstance(
    absl::string_view filename, absl::string_view input, int start_offset,
    std::optional<int> end_offset, int offset_in_original_input) const {
  return std::make_unique<TokenProvider>(filename, input, start_offset,
                                         end_offset, offset_in_original_input);
}

// Derives the next lookahead token from `prev`:
// - If `prev` is an error, propagates the exact same error.
// - If `prev` is Token::EOI, propagates EOI with empty preceding whitespace.
// - Otherwise, fetches the next token from the underlying lexer.
absl::StatusOr<TokenWithLocation> TokenProvider::FetchNextLookahead(
    const absl::StatusOr<TokenWithLocation>& prev) {
  if (!prev.ok()) return prev;
  if (prev->kind == Token::EOI) {
    TokenWithLocation eoi = *prev;
    eoi.preceding_whitespaces = "";
    return absl::StatusOr<TokenWithLocation>(eoi);
  }
  return GetToken();
}

// Peeks Lookahead 1 (token N+1). Does not call GetToken().
absl::StatusOr<TokenWithLocation> TokenProvider::PeekNextToken() {
  return lookahead_1_;
}

// Peeks Lookahead 2 (token N+2). Does not call GetToken().
absl::StatusOr<TokenWithLocation> TokenProvider::PeekLookahead2() {
  return lookahead_2_;
}

// Peeks Lookahead 3 (token N+3). Does not call GetToken().
absl::StatusOr<TokenWithLocation> TokenProvider::PeekLookahead3() {
  return lookahead_3_;
}

// Consumes Lookahead 1, shifts lookahead buffers left, and refills the buffers.
absl::StatusOr<TokenWithLocation> TokenProvider::ConsumeNextTokenImpl() {
  absl::StatusOr<TokenWithLocation> token = lookahead_1_;
  // If the current token is an error, the lookahead buffers are intentionally
  // not shifted, meaning subsequent calls will continually return the exact
  // same error without fetching more tokens from the underlying token stream.
  if (lookahead_1_.ok()) {
    lookahead_1_ = std::move(lookahead_2_);
    lookahead_2_ = std::move(lookahead_3_);
    lookahead_3_ = FetchNextLookahead(lookahead_2_);
  }
  return token;
}

absl::StatusOr<TokenWithLocation> TokenProvider::GetToken() {
  GOOGLESQL_ASSIGN_OR_RETURN(TokenWithLocation token, tokenizer_->GetNextToken());
  if (offset_in_original_input() != 0) {
    token.location.mutable_start().IncrementByteOffset(
        offset_in_original_input());
    token.location.mutable_end().IncrementByteOffset(
        offset_in_original_input());
  }
  return token;
}

}  // namespace macros
}  // namespace parser
}  // namespace googlesql
