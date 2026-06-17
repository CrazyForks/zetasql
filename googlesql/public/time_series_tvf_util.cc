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

#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

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

}  // namespace googlesql
