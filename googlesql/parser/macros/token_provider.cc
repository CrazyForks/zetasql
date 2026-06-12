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

#include "googlesql/parser/macros/token_provider_base.h"
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
          filename, input.substr(0, this->end_offset()), start_offset)) {}

std::unique_ptr<TokenProviderBase> TokenProvider::CreateNewInstance(
    absl::string_view filename, absl::string_view input, int start_offset,
    std::optional<int> end_offset, int offset_in_original_input) const {
  return std::make_unique<TokenProvider>(filename, input, start_offset,
                                         end_offset, offset_in_original_input);
}

absl::StatusOr<TokenWithLocation> TokenProvider::ConsumeNextTokenImpl() {
  if (!input_token_buffer_.empty()) {
    // Check for any unused tokens first, before we pull any more
    const TokenWithLocation front_token = input_token_buffer_.front();
    input_token_buffer_.pop();
    return front_token;
  }

  return GetToken();
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
