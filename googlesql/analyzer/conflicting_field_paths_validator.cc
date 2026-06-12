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

#include "googlesql/analyzer/conflicting_field_paths_validator.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/analyzer/resolver.h"
#include "googlesql/parser/ast_node.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "google/protobuf/descriptor.h"
#include "googlesql/base/map_util.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {

absl::Status ConflictingFieldPathsValidator::AddFieldPath(
    const ASTNode* path_location, const Resolver::FindFieldsOutput& field_path,
    ConflictInfo* conflict) {
  std::string path_string;
  bool conflicting_oneof = false;
  std::string shortest_oneof_path;
  for (const Resolver::FindFieldsOutput::StructFieldInfo& struct_field :
       field_path.struct_path) {
    if (!path_string.empty()) {
      absl::StrAppend(&path_string, ".");
    }
    absl::StrAppend(&path_string, struct_field.field->name);
  }
  for (const google::protobuf::FieldDescriptor* field :
       field_path.field_descriptor_path) {
    if (field->real_containing_oneof() != nullptr &&
        shortest_oneof_path.empty()) {
      shortest_oneof_path = path_string;
      if (!shortest_oneof_path.empty()) {
        absl::StrAppend(&shortest_oneof_path, ".");
      }
      absl::StrAppend(&shortest_oneof_path,
                      field->real_containing_oneof()->name());
      if (oneof_path_to_full_path_.contains(shortest_oneof_path)) {
        conflicting_oneof = true;
      }
    }
    if (!path_string.empty()) {
      absl::StrAppend(&path_string, ".");
    }
    if (field->is_extension()) {
      absl::StrAppend(&path_string, "(");
    }
    absl::StrAppend(&path_string,
                    field->is_extension() ? field->full_name() : field->name());
    if (field->is_extension()) {
      absl::StrAppend(&path_string, ")");
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(RecordOneOfConflict(path_location, path_string,
                                      shortest_oneof_path, conflicting_oneof,
                                      conflict));
  GOOGLESQL_RETURN_IF_ERROR(RecordPathConflict(path_location, path_string, conflict));
  return absl::OkStatus();
}

absl::Status ConflictingFieldPathsValidator::RecordOneOfConflict(
    const ASTNode* path_location, const std::string& path_string,
    const std::string& shortest_oneof_path, bool conflicting_oneof,
    ConflictInfo* conflict) {
  if (conflicting_oneof) {
    std::string conflicting_path =
        googlesql_base::FindOrDie(oneof_path_to_full_path_, shortest_oneof_path);
    if (conflict != nullptr) {
      conflict->path = path_string;
      conflict->conflicting_path = conflicting_path;
      conflict->is_oneof_conflict = true;
    }
    return absl::InvalidArgumentError(absl::StrCat(
        "Modifying multiple fields from the same OneOf is unsupported. "
        "Field path '",
        path_string, "' overlaps with field path '", conflicting_path, "'"));
  }
  if (!conflicting_oneof && !shortest_oneof_path.empty()) {
    GOOGLESQL_RET_CHECK(googlesql_base::InsertIfNotPresent(&oneof_path_to_full_path_,
                                      shortest_oneof_path, path_string));
  }
  return absl::OkStatus();
}

absl::Status ConflictingFieldPathsValidator::RecordPathConflict(
    const ASTNode* path_location, absl::string_view path_string,
    ConflictInfo* conflict) {
  // Determine if a prefix of 'path_string' is already present in the trie.
  int match_length = 0;
  const ASTNode* prefix_location = field_path_trie_.GetDataForMaximalPrefix(
      path_string, match_length, /*is_terminator=*/{});
  std::vector<std::pair<std::string, const ASTNode*>> matching_paths;
  bool prefix_exists = false;
  if (prefix_location != nullptr) {
    // If the max prefix is equal to 'path_string' or if the next character of
    // 'path_string' after the max prefix is a "." then a conflicting path is
    // already present in the trie.
    prefix_exists =
        path_string.size() == match_length
            ? true
            : (std::strncmp(&path_string.at(match_length), ".", 1) == 0);
  } else {
    // Determine if 'path_string' is the prefix of a path already in the trie.
    field_path_trie_.GetAllMatchingStrings(absl::StrCat(path_string, "."),
                                           &matching_paths);
  }
  if (prefix_exists || !matching_paths.empty()) {
    if (conflict != nullptr) {
      conflict->path = path_string;
      conflict->conflicting_path = prefix_exists
                                       ? path_string.substr(0, match_length)
                                       : matching_paths.at(0).first;
      conflict->is_oneof_conflict = false;
    }

    return absl::InvalidArgumentError(absl::StrCat(
        "Field path '", path_string, "' overlaps with field path '",
        prefix_exists ? path_string.substr(0, match_length)
                      : matching_paths.at(0).first,
        "'"));
  }
  field_path_trie_.Insert(path_string, path_location);
  return absl::OkStatus();
}
}  // namespace googlesql
