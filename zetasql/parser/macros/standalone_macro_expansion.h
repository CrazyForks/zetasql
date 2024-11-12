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

#ifndef ZETASQL_PARSER_MACROS_STANDALONE_MACRO_EXPANSION_H_
#define ZETASQL_PARSER_MACROS_STANDALONE_MACRO_EXPANSION_H_

#include <string>
#include <vector>

#include "zetasql/parser/token_with_location.h"
#include "absl/base/macros.h"
#include "absl/types/span.h"

namespace zetasql {
namespace parser {
namespace macros {

// Converts the given tokens to a string.
//
// IMPORTANT: The function prevents splicing by inserting an extra single
// whitespace where needed:
// 1. Unquoted identifier, keyword, or a macro invocation followed by a token
//    that starts with a character that could continue the previous token.
//    Any potential lenient splicing should have already been done by the
//    expander.
// 2. Symbols that may cause comment-out, i.e. --, /*, or //
// 3. Integer and floating point literals, e.g. `1.` and `2`
//
// Note however, if two tokens are adjacent, i.e. `token1.AdjacentlyPrecedes(
// token2)` returns true, no spaces will be inserted in between even if they
// belong to the cases mentioned above.
std::string TokensToString(absl::Span<const TokenWithLocation> tokens);

}  // namespace macros
}  // namespace parser
}  // namespace zetasql

#endif  // ZETASQL_PARSER_MACROS_STANDALONE_MACRO_EXPANSION_H_
