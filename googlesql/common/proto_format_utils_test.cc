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

#include "googlesql/common/proto_format_utils.h"

#include <string>

#include "google/protobuf/descriptor.pb.h"
#include "gtest/gtest.h"
#include "absl/strings/match.h"

namespace googlesql {
namespace {

TEST(ProtoFormatUtilsTest, ToStableShortDebugString) {
  google::protobuf::DescriptorProto proto;
  proto.set_name("TestMessage");

  const std::string output = ToStableShortDebugString(proto);

  EXPECT_EQ(output, "name: \"TestMessage\"");
}

TEST(ProtoFormatUtilsTest, ToStableShortDebugStringNested) {
  google::protobuf::DescriptorProto proto;
  proto.set_name("Outer");
  auto* field = proto.add_field();
  field->set_name("inner_field");
  field->set_number(1);

  const std::string output = ToStableShortDebugString(proto);

  EXPECT_EQ(output,
            "name: \"Outer\" field { name: \"inner_field\" number: 1 }");
}

TEST(ProtoFormatUtilsTest, ToStableDebugString) {
  google::protobuf::DescriptorProto proto;
  proto.set_name("TestMessage");
  const std::string output = ToStableDebugString(proto);
  EXPECT_TRUE(absl::StrContains(output, "name: \"TestMessage\"\n"));
}

TEST(ProtoFormatUtilsTest, ToStableDebugStringNested) {
  google::protobuf::DescriptorProto proto;
  proto.set_name("Outer");
  auto* field = proto.add_field();
  field->set_name("inner_field");
  field->set_number(1);

  const std::string output = ToStableDebugString(proto);

  EXPECT_EQ(output,
            "name: \"Outer\"\n"
            "field {\n"
            "  name: \"inner_field\"\n"
            "  number: 1\n"
            "}\n");
}

TEST(ProtoFormatUtilsTest, EmptyMessage) {
  google::protobuf::DescriptorProto proto;

  EXPECT_EQ(ToStableShortDebugString(proto), "");
  EXPECT_EQ(ToStableDebugString(proto), "");
}

}  // namespace
}  // namespace googlesql
