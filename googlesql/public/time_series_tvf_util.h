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

#ifndef GOOGLESQL_PUBLIC_TIME_SERIES_TVF_UTIL_H_
#define GOOGLESQL_PUBLIC_TIME_SERIES_TVF_UTIL_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {

// Convert a dotted identifier path string to a vector of path components,
// unquoting identifiers if necessary.
//
// Reserved keywords (such as SELECT, FROM, or INTERVAL) are not treated as
// reserved and are parsed as an identifier, even at the start of the path.
//
// Unquoted slashes ('/') and backslashes ('\') are not
// supported. Quoted components may contain these characters, but backslashes
// are treated as literals and not processed as escape sequences. (e.g.
// "`a\\b`.c" => {"a\\b", "c"})
//
//  Examples:
//   "abc" => {"abc"}
//   "abc.def" => {"abc", "def"}
//   "select.from.interval" => {"select", "from", "interval"}
//   "abc.`def`" => {"abc", "def"}
//   "`abc.def`.ghi" => {"abc.def", "ghi"}
//   "abc/def" => error
//   "abc..def" => error
//
// Returned errors are of status InvalidArgument, appropriate for returning to
// the user at analysis-time.
// - Path structure errors:
//   - if the input string is empty.
//   - if the input begins or ends with '.'.
//   - if the path contains empty components (e.g. "a..b", "abc.``").
//   - if the path contains unclosed backticks (e.g. "a.`b").
// - Quoted component errors:
//   - if any backslash-enclosed component is not immediately preceded by the
//     start of the string or a dot ('.') (e.g. "a`b`").
//   - if any backslash-enclosed component is not immediately followed by a dot
//     ('.') or the end of the string (e.g. "`a`bc").
// - Unquoted component errors:
//   - if the first component starts with a digit (e.g. "123.abc").
//   - if it contains a slash ('/') or backslash ('\') character.
//   - if it contains any character that is not ASCII alphanumeric or an
//     underscore (e.g. "a*b", "%b", "a#", "你好").
absl::StatusOr<std::vector<std::string>> ParseSimpleIdentifierPath(
    absl::string_view path);

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_TIME_SERIES_TVF_UTIL_H_
