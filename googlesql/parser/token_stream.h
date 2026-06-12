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

#ifndef GOOGLESQL_PARSER_TOKEN_STREAM_H_
#define GOOGLESQL_PARSER_TOKEN_STREAM_H_

#include "googlesql/parser/token_with_location.h"
#include "absl/status/statusor.h"

namespace googlesql::parser {

// Interface for the token stream.
class TokenStream {
 public:
  virtual ~TokenStream() = default;
  virtual absl::StatusOr<TokenWithLocation> GetNextToken() = 0;
  virtual int num_consumed_tokens() const = 0;
};

}  // namespace googlesql::parser

#endif  // GOOGLESQL_PARSER_TOKEN_STREAM_H_
