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

#include "googlesql/public/time_series_tvf_util.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/proto_type.h"
#include "googlesql/public/types/struct_type.h"
#include "googlesql/public/types/type_factory.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "google/protobuf/descriptor.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {

namespace {

// Helper to validate and unquote a single path segment.
absl::StatusOr<std::string> ValidateAndUnquoteSegment(absl::string_view segment,
                                                      bool is_first_segment) {
  // Quoted Segment.
  if (segment.front() == '`') {
    // Extract contents inside the backticks.
    std::string component = std::string(segment.substr(1, segment.size() - 2));
    if (component.empty()) {
      return absl::InvalidArgumentError("Path contains an empty component");
    }
    return component;
  }

  // Unquoted Segment.
  // Allow alphanumeric characters and underscores only.
  for (size_t i = 0; i < segment.size(); ++i) {
    const char c = segment[i];
    if (!absl::ascii_isalnum(c) && c != '_') {
      return absl::InvalidArgumentError(absl::StrCat(
          "Path contains an invalid character '", std::string(1, c), "'"));
    }
  }

  // First component must not start with a digit, if the component is not
  // quoted.
  if (is_first_segment && absl::ascii_isdigit(segment.front())) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Invalid identifier path: first component cannot start with a digit, "
        "found '",
        std::string(1, segment.front()), "'"));
  }

  return std::string(segment);
}

struct ResolvedFieldPath {
  std::vector<TypeFieldPathStep> steps;
  const Type* leaf_type = nullptr;
};

absl::Status FieldPathError(absl::string_view message,
                            absl::string_view component, size_t i) {
  return absl::InvalidArgumentError(
      absl::StrCat("Unable to resolve '", component, "' (path component #",
                   i + 1, "): ", message));
}

// Helper to traverse a type along a path of components and validate
// correctness.
absl::StatusOr<ResolvedFieldPath> ResolveAndValidateFieldPath(
    const Type* start_type, absl::Span<const std::string> full_path_components,
    size_t start_index, TypeFactory* type_factory) {
  ResolvedFieldPath result;
  const Type* current_type = start_type;
  for (size_t i = start_index; i < full_path_components.size(); ++i) {
    const std::string& component = full_path_components[i];
    if (current_type->IsStruct()) {
      const StructType* struct_type = current_type->AsStruct();
      bool is_ambiguous = false;
      int found_idx = -1;
      const StructField* field =
          struct_type->FindField(component, &is_ambiguous, &found_idx);
      if (is_ambiguous) {
        return FieldPathError("Struct field name is ambiguous", component, i);
      }
      if (field == nullptr) {
        return FieldPathError("Field not found in struct", component, i);
      }
      result.steps.push_back(
          {TypeFieldPathStep::STRUCT_FIELD, found_idx, nullptr});
      current_type = field->type;
    } else if (current_type->IsProto()) {
      const google::protobuf::Descriptor* descriptor =
          current_type->AsProto()->descriptor();
      const google::protobuf::FieldDescriptor* field_desc =
          descriptor->FindFieldByName(component);
      if (field_desc == nullptr) {
        return FieldPathError(
            absl::StrCat("Field not found in proto ", descriptor->full_name()),
            component, i);
      }
      if (field_desc->is_map()) {
        return FieldPathError("Proto map field is not supported", component, i);
      }
      if (field_desc->is_repeated()) {
        return FieldPathError("Proto repeated field is not supported",
                              component, i);
      }
      const Type* field_type = nullptr;
      GOOGLESQL_RETURN_IF_ERROR(type_factory->GetProtoFieldType(
          field_desc, absl::Span<const std::string>(), &field_type));
      result.steps.push_back({TypeFieldPathStep::PROTO_FIELD, -1, field_desc});
      current_type = field_type;
    } else {
      return FieldPathError("Cannot traverse non-struct/proto field", component,
                            i);
    }
  }
  result.leaf_type = current_type;
  return result;
}

bool IsValidTimestampType(const Type* type) {
  if (type->IsTimestamp()) {
    return true;
  }
  if (type->IsProto()) {
    return type->AsProto()->descriptor()->full_name() ==
           "google.protobuf.Timestamp";
  }
  return false;
}

}  // namespace

absl::StatusOr<std::vector<std::string>> ParseSimpleIdentifierPath(
    absl::string_view path) {
  if (path.empty()) {
    return absl::InvalidArgumentError("Path string cannot be empty");
  }

  // Fail fast if path begins or ends with '.'
  if (path.front() == '.') {
    return absl::InvalidArgumentError("Path string cannot begin with '.'");
  }
  if (path.back() == '.') {
    return absl::InvalidArgumentError("Path string cannot end with '.'");
  }

  // raw_segments contains the unvalidated, unquoted segments of the path,
  // including the backtick characters if present.
  std::vector<absl::string_view> raw_segments;
  auto segment_start = path.begin();
  bool in_backticks = false;

  // Split the path into segments based on '.' delimiters.
  for (auto p = path.begin(); p != path.end(); ++p) {
    const char c = *p;

    if ((c == '/' || c == '\\') && !in_backticks) {
      return absl::InvalidArgumentError(
          absl::StrCat("Path contains an invalid character '",
                       std::string(1, c), "' at index ", (p - path.begin())));
    }

    if (c == '`') {
      if (!in_backticks) {
        // Start of backquoted segment. Must be at the start of the current
        // segment.
        if (p != segment_start) {
          return absl::InvalidArgumentError(absl::StrCat(
              "Backtick '`' is only allowed at the start of a path component, "
              "found at index ",
              (p - path.begin())));
        }
        in_backticks = true;
      } else {
        // End of backquoted segment.
        in_backticks = false;
        // Look ahead to check the next character.
        if (p + 1 < path.end() && *(p + 1) != '.') {
          return absl::InvalidArgumentError(absl::StrCat(
              "Expected '.' or end of string after closing backtick at index ",
              (p - path.begin())));
        }
      }
    } else if (c == '.' && !in_backticks) {
      // Dot outside backticks separates components.
      if (p == segment_start) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Path contains an empty component at index ", (p - path.begin())));
      }
      raw_segments.push_back(
          path.substr(segment_start - path.begin(), p - segment_start));
      segment_start = p + 1;
    }
  }

  if (in_backticks) {
    // If we reach the end of the path while inside backticks, it means there
    // was an unmatched backtick.
    return absl::InvalidArgumentError(
        "Path contains an unmatched backtick '`'");
  }

  // Add the last segment.
  raw_segments.push_back(
      path.substr(segment_start - path.begin(), path.end() - segment_start));

  // Validate and unquote each segment.
  std::vector<std::string> components;
  components.reserve(raw_segments.size());
  for (size_t i = 0; i < raw_segments.size(); ++i) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::string component,
                     ValidateAndUnquoteSegment(raw_segments[i],
                                               /*is_first_segment=*/i == 0));
    components.push_back(std::move(component));
  }

  return components;
}

absl::StatusOr<ResolvedTimestampColumnPath> ResolveTimestampColumnPath(
    const TVFRelation& input_relation, absl::string_view path_string,
    TypeFactory* type_factory) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<std::string> components,
                   ParseSimpleIdentifierPath(path_string));

  ResolvedTimestampColumnPath result;
  const Type* start_type = nullptr;
  size_t start_index;

  if (input_relation.is_value_table()) {
    // Value table resolution.
    // The first column in the input relation for a value table is an unnamed
    // column representing the value of the row.
    // Get the type of the value table.
    const Type* value_table_type = input_relation.column(0).type;
    result.column_index = 0;

    GOOGLESQL_RET_CHECK(value_table_type->IsStructOrProto())
        << "Value table type must be a struct or proto";

    start_type = value_table_type;
    start_index = 0;
  } else {
    // SQL Table resolution.
    // Find the column in the input relation that matches the first component.
    int matched_col_idx = -1;
    for (int i = 0; i < input_relation.num_columns(); ++i) {
      if (googlesql_base::CaseEqual(input_relation.column(i).name,
                                 components[0])) {
        if (matched_col_idx != -1) {
          return FieldPathError("Ambiguous column reference", components[0], 0);
        }
        matched_col_idx = i;
      }
    }

    if (matched_col_idx == -1) {
      return FieldPathError("Column not found", components[0], 0);
    }

    result.column_index = matched_col_idx;
    start_type = input_relation.column(matched_col_idx).type;
    start_index = 1;
  }

  // Resolve the path, with full path components. The first component refers
  // to a field in the value table (for value tables) or the field in the column
  // (for SQL tables).
  GOOGLESQL_ASSIGN_OR_RETURN(ResolvedFieldPath field_path_result,
                   ResolveAndValidateFieldPath(start_type, components,
                                               start_index, type_factory));
  result.steps = std::move(field_path_result.steps);
  result.leaf_type = field_path_result.leaf_type;

  if (!IsValidTimestampType(result.leaf_type)) {
    return FieldPathError(
        "Leaf field type must be TIMESTAMP or google.protobuf.Timestamp",
        components.back(), components.size() - 1);
  }

  return result;
}

}  // namespace googlesql
