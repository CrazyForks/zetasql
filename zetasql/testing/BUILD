#
# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

package(default_visibility = ["//zetasql/base:zetasql_implementation"])

cc_library(
    name = "sql_types_test",
    testonly = 1,
    srcs = ["sql_types_test.cc"],
    hdrs = ["sql_types_test.h"],
    deps = [
        "//zetasql/base:check",
        "//zetasql/base:map_util",
        "//zetasql/base/testing:zetasql_gtest_main",
        "//zetasql/public:civil_time",
        "//zetasql/public:coercer",
        "//zetasql/public:function",
        "//zetasql/public:language_options",
        "//zetasql/public:numeric_value",
        "//zetasql/public:options_cc_proto",
        "//zetasql/public:type",
        "//zetasql/public:type_cc_proto",
        "//zetasql/public:value",
        "//zetasql/public/types",
        "//zetasql/testdata:test_schema_cc_proto",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/strings:cord",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_library(
    name = "test_module_contents_fetcher",
    testonly = 1,
    srcs = ["test_module_contents_fetcher.cc"],
    hdrs = ["test_module_contents_fetcher.h"],
    deps = [
        "//zetasql/base:map_util",
        "//zetasql/base:ret_check",
        "//zetasql/base:status",
        "//zetasql/public:file_module_contents_fetcher",
        "//zetasql/public:module_contents_fetcher",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_library(
    name = "test_catalog",
    testonly = 1,
    srcs = ["test_catalog.cc"],
    hdrs = ["test_catalog.h"],
    deps = [
        "//zetasql/base:map_util",
        "//zetasql/base:status",
        "//zetasql/public:catalog",
        "//zetasql/public:function",
        "//zetasql/public:simple_catalog",
        "//zetasql/public:type",
        "//zetasql/public/types",
        "@com_google_absl//absl/container:node_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "test_catalog_test",
    size = "small",
    srcs = ["test_catalog_test.cc"],
    deps = [
        ":test_catalog",
        "//zetasql/base/testing:status_matchers",
        "//zetasql/base/testing:zetasql_gtest_main",
        "//zetasql/public:catalog",
        "//zetasql/public:function",
        "//zetasql/public:simple_catalog",
        "//zetasql/public:type",
        "@com_google_absl//absl/status",
    ],
)

cc_library(
    name = "test_value",
    testonly = 1,
    srcs = ["test_value.cc"],
    hdrs = [
        "test_value.h",
        "using_test_value.cc",
    ],
    deps = [
        "//zetasql/base:check",
        "//zetasql/base:status",
        "//zetasql/common:float_margin",
        "//zetasql/common:internal_value",
        "//zetasql/compliance:test_driver",
        "//zetasql/public:json_value",
        "//zetasql/public:numeric_value",
        "//zetasql/public:timestamp_picos_value",
        "//zetasql/public:type",
        "//zetasql/public:value",
        "//zetasql/public/types",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_library(
    name = "test_function",
    testonly = 1,
    srcs = ["test_function.cc"],
    hdrs = ["test_function.h"],
    deps = [
        ":test_value",
        "//zetasql/base",
        "//zetasql/base:check",
        "//zetasql/base:map_util",
        "//zetasql/base:status",
        "//zetasql/common:float_margin",
        "//zetasql/public:options_cc_proto",
        "//zetasql/public:type",
        "//zetasql/public:type_cc_proto",
        "//zetasql/public:value",
        "//zetasql/public/types",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "type_util",
    testonly = 1,
    srcs = ["type_util.cc"],
    hdrs = ["type_util.h"],
    deps = [
        "//zetasql/base:check",
        "//zetasql/public:type",
        "//zetasql/public:type_cc_proto",
        "//zetasql/public/types",
        "//zetasql/testdata:test_proto3_cc_proto",
        "//zetasql/testdata:test_schema_cc_proto",
        "@com_google_googleapis//google/type:date_cc_proto",
        "@com_google_googleapis//google/type:latlng_cc_proto",
        "@com_google_googleapis//google/type:timeofday_cc_proto",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_test(
    name = "type_util_test",
    size = "small",
    srcs = ["type_util_test.cc"],
    deps = [
        ":sql_types_test",
        ":type_util",
        "//zetasql/base/testing:status_matchers",
        "//zetasql/base/testing:zetasql_gtest_main",
        "//zetasql/public:type",
    ],
)
