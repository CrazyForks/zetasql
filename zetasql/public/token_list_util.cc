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

#include "zetasql/public/token_list_util.h"

#include <string>
#include <utility>
#include <vector>

#include "zetasql/public/simple_token_list.h"
#include "zetasql/public/value.h"
#include "absl/status/statusor.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

Value TokenListFromStringArray(std::vector<std::string> tokens,
                               const bool make_index) {
  tokens::TokenListBuilder tlb;
  for (const auto& t : tokens) {
    if (make_index) {
      tlb.Add(tokens::TextToken::MakeIndex(std::move(t)));
    } else {
      tlb.Add(tokens::TextToken::Make(std::move(t)));
    }
  }
  return Value::TokenList(tlb.Build());
}

absl::StatusOr<std::vector<std::string>> StringArrayFromTokenList(
    const Value& token_list) {
  std::vector<std::string> strings;

  ZETASQL_ASSIGN_OR_RETURN(auto iter, token_list.tokenlist_value().GetIterator());
  tokens::TextToken token;
  while (!iter.done()) {
    ZETASQL_RETURN_IF_ERROR(iter.Next(token));
    strings.push_back(std::string(token.text()));
  }

  return strings;
}

Value TokenListFromBytes(std::string bytes) {
  return zetasql::Value::TokenList(
      tokens::TokenList::FromBytes(std::move(bytes)));
}

}  // namespace zetasql
