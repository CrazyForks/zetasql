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

#ifndef GOOGLESQL_TOOLS_FORMATTER_INTERNAL_PROCEDURAL_BLOCK_DEF_H_
#define GOOGLESQL_TOOLS_FORMATTER_INTERNAL_PROCEDURAL_BLOCK_DEF_H_

#include "absl/algorithm/container.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace googlesql::formatter::internal {

// Classifies the role of a continuer keyword in the structure of the block.
enum class ProceduralContinuerType {
  // Separates different logical branches within the same block (e.g. ELSE,
  // ELSEIF, WHEN).
  BRANCH_DIVIDER,
  // Precedes a block of statements or an expression (e.g. THEN).
  BODY_STARTER,
};

// Describes indentation behavior for procedural blocks.
enum class ProceduralIndentBehavior {
  // Aligned with the block opener (e.g., ELSE in an IF statement).
  SAME_LEVEL,
  // Indented relative to the block opener (e.g., WHEN in a CASE statement).
  INDENTED,
};

// Defines a keyword that continues a procedural block (e.g. ELSE in IF, or
// WHEN in CASE).
struct ProceduralContinuerDef {
  absl::string_view keyword;

  // Whether a statement is expected after this continuer.
  bool expects_statement;

  // The role of this continuer.
  ProceduralContinuerType type;

  // How to indent this continuer.
  ProceduralIndentBehavior indent_behavior;
};

// Defines a procedural block statement (e.g. IF, WHILE, CASE).
struct ProceduralBlockDef {
  absl::string_view opener_keyword;
  absl::Span<const ProceduralContinuerDef> continuers;

  // Expected keyword after END (e.g. "IF", or empty if none).
  absl::string_view closer_suffix;

  // Whether the block can be used as an expression (like CASE).
  bool is_expression_capable;

  // If not empty, `opener_keyword` starts a block only when it is
  // immediately preceded by one of the keywords in this list. If empty,
  // `opener_keyword` always starts a block.
  absl::Span<const absl::string_view> allowed_previous_keywords;

  const ProceduralContinuerDef* FindContinuer(absl::string_view keyword) const {
    auto it = absl::c_find_if(
        continuers, [keyword](const auto& c) { return c.keyword == keyword; });
    return it == continuers.end() ? nullptr : &*it;
  }
};

// Returns the unified definition for a given procedural block opener.
const ProceduralBlockDef* GetProceduralBlockDef(absl::string_view opener);

}  // namespace googlesql::formatter::internal

#endif  // GOOGLESQL_TOOLS_FORMATTER_INTERNAL_PROCEDURAL_BLOCK_DEF_H_
