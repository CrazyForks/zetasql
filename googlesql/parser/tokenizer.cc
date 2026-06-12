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

#include <cstddef>

#include "googlesql/parser/tm_lexer.h"
#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/public/parse_location.h"
#include "absl/flags/flag.h"
#include "googlesql/base/check.h"
#include "googlesql/base/status_macros.h"
#include "absl/strings/string_view.h"

// TODO: Remove flag when references are gone.
ABSL_FLAG(bool, googlesql_use_customized_flex_istream, true, "Unused");

namespace googlesql {
namespace parser {

static absl::string_view GetTextBetween(absl::string_view input, size_t start,
                                        size_t end) {
  ABSL_DCHECK_LE(start, end);
  ABSL_DCHECK_LE(start, input.length());
  size_t len = end - start;
  ABSL_DCHECK_LE(len, input.length());
  return absl::ClippedSubstr(input, start, len);
}

absl::StatusOr<TokenWithLocation> GoogleSqlTokenizer::GetNextToken() {
  int ws_start = last_token_end_offset_;

  Token token_kind = Next();
  GOOGLESQL_RETURN_IF_ERROR(override_error_);
  num_consumed_tokens_++;

  ParseLocationRange location = LastTokenLocationWithStartOffset();
  int tok_start = location.start().GetByteOffset();
  int tok_end = location.end().GetByteOffset();
  last_token_end_offset_ = tok_end;
  return TokenWithLocation{
      .kind = token_kind,
      .location = location,
      .text = GetTextBetween(input_, tok_start, tok_end),
      .preceding_whitespaces = GetTextBetween(input_, ws_start, tok_start)};
}

GoogleSqlTokenizer::GoogleSqlTokenizer(absl::string_view filename,
                                       absl::string_view input,
                                       int start_offset)
    // We do not use Lexer::Rewind() because its time complexity is
    // O(start_offset). See the comment for `Lexer::start_offset_` in
    // googlesql.tm for more information.
    : Lexer(absl::ClippedSubstr(input, start_offset)),
      last_token_end_offset_(start_offset) {
  input_ = input;
  filename_ = filename;
  start_offset_ = start_offset;
}

}  // namespace parser
}  // namespace googlesql
