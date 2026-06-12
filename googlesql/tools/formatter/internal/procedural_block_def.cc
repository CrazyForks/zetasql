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

#include "googlesql/tools/formatter/internal/procedural_block_def.h"

#include <iterator>

#include "absl/algorithm/container.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace googlesql::formatter::internal {

const ProceduralBlockDef* GetProceduralBlockDef(absl::string_view opener) {
  static constexpr absl::string_view kIfPrecedingKeywords[] = {";", "THEN",
                                                               "ELSE"};

  static constexpr ProceduralContinuerDef kIfContinuers[] = {
      {"THEN", true, ProceduralContinuerType::BODY_STARTER,
       ProceduralIndentBehavior::INDENTED},
      {"ELSE", true, ProceduralContinuerType::BRANCH_DIVIDER,
       ProceduralIndentBehavior::SAME_LEVEL},
      {"ELSEIF", false, ProceduralContinuerType::BRANCH_DIVIDER,
       ProceduralIndentBehavior::SAME_LEVEL}};

  static constexpr ProceduralContinuerDef kCaseContinuers[] = {
      {"WHEN", false, ProceduralContinuerType::BRANCH_DIVIDER,
       ProceduralIndentBehavior::INDENTED},
      {"THEN", true, ProceduralContinuerType::BODY_STARTER,
       ProceduralIndentBehavior::INDENTED},
      {"ELSE", true, ProceduralContinuerType::BRANCH_DIVIDER,
       ProceduralIndentBehavior::INDENTED}};

  static constexpr ProceduralBlockDef kBlockDefs[] = {
      {"IF", absl::MakeConstSpan(kIfContinuers), "IF", false,
       absl::MakeConstSpan(kIfPrecedingKeywords)},
      {"CASE", absl::MakeConstSpan(kCaseContinuers), "CASE", true, {}},
  };

  auto it = absl::c_find_if(kBlockDefs, [opener](const ProceduralBlockDef& d) {
    return d.opener_keyword == opener;
  });
  return it == std::end(kBlockDefs) ? nullptr : &*it;
}

}  // namespace googlesql::formatter::internal
