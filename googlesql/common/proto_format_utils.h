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

#ifndef GOOGLESQL_COMMON_PROTO_FORMAT_UTILS_H_
#define GOOGLESQL_COMMON_PROTO_FORMAT_UTILS_H_

#include <string>

#include "google/protobuf/message.h"

namespace googlesql {

// Returns a stable, single-line, human-readable representation of the given
// protobuf message.
//
// Unlike the standard `proto2::Message::ShortDebugString()`, this function
// produces a deterministic output that:
// 1. Does not contain randomized URL prefixes (e.g., "goo.gle/debugstr").
// 2. Does not automatically redact sensitive fields.
std::string ToStableShortDebugString(const google::protobuf::Message& message);

// Returns a stable, multi-line, human-readable representation of the given
// protobuf message with standard indentation.
//
// Similar to `ToStableShortDebugString`, this function provides a deterministic
// alternative to `proto2::Message::DebugString()` by avoiding randomized
// markers and automatic field redaction introduced in Protobuf v30+.
std::string ToStableDebugString(const google::protobuf::Message& message);

}  // namespace googlesql

#endif  // GOOGLESQL_COMMON_PROTO_FORMAT_UTILS_H_
