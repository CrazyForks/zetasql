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

#ifndef GOOGLESQL_ANALYZER_CONFLICTING_FIELD_PATHS_VALIDATOR_H_
#define GOOGLESQL_ANALYZER_CONFLICTING_FIELD_PATHS_VALIDATOR_H_

#include <string>

#include "googlesql/analyzer/resolver.h"
#include "googlesql/parser/parse_tree.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/general_trie.h"

namespace googlesql {

// This class validates that proto field paths are not conflicting with field
// paths that have already been added.
//
// A conflict occurs if:
// 1. A path is a prefix of another path (e.g., "a.b" and "a.b.c").
// 2. A path is the same as another path (e.g., "a.b" and "a.b").
// 3. Two paths belong to the same OneOf field (e.g., "oneof_a" and "oneof_b")
class ConflictingFieldPathsValidator {
 public:
  struct ConflictInfo {
    std::string path;
    std::string conflicting_path;
    bool is_oneof_conflict = false;
  };

  ConflictingFieldPathsValidator() = default;

  // Adds a field path to validator, returning an error if it conflicts with
  // paths that have already been added.
  // If `conflict` is not null, it will be populated with the conflict details
  // if validation fails.
  absl::Status AddFieldPath(const ASTNode* path_location,
                            const Resolver::FindFieldsOutput& field_path,
                            ConflictInfo* conflict = nullptr);

 private:
  // Validates that a field path is not conflicting with oneof paths that
  // have already been added.
  absl::Status RecordOneOfConflict(const ASTNode* path_location,
                                   const std::string& path_string,
                                   const std::string& shortest_oneof_path,
                                   bool conflicting_oneof,
                                   ConflictInfo* conflict);

  // Validates that a field path is not conflicting with field paths that
  // have already been added.
  absl::Status RecordPathConflict(const ASTNode* path_location,
                                  absl::string_view path_string,
                                  ConflictInfo* conflict);

  // Maps from paths of OneOf fields that have already been modified to the
  // corresponding path expression that accessed the OneOf path.
  absl::flat_hash_map<std::string, std::string> oneof_path_to_full_path_;
  // Trie to keep track of conflicting field paths.
  googlesql_base::GeneralTrie<const ASTNode*, nullptr> field_path_trie_;
};

}  // namespace googlesql

#endif  // GOOGLESQL_ANALYZER_CONFLICTING_FIELD_PATHS_VALIDATOR_H_
