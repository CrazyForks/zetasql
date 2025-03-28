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

load("//zetasql/tools/execute_query/web:embed_files.bzl", "gen_string_constant_header_file")

package(
    default_visibility = ["//zetasql/base:zetasql_implementation"],
)

gen_string_constant_header_file(
    name = "customer_tbl",
    src = "customer.tbl",
    variable = "kTpchData_customer",
)

gen_string_constant_header_file(
    name = "lineitem_tbl",
    src = "lineitem.tbl",
    variable = "kTpchData_lineitem",
)

gen_string_constant_header_file(
    name = "nation_tbl",
    src = "nation.tbl",
    variable = "kTpchData_nation",
)

gen_string_constant_header_file(
    name = "orders_tbl",
    src = "orders.tbl",
    variable = "kTpchData_orders",
)

gen_string_constant_header_file(
    name = "part_tbl",
    src = "part.tbl",
    variable = "kTpchData_part",
)

gen_string_constant_header_file(
    name = "partsupp_tbl",
    src = "partsupp.tbl",
    variable = "kTpchData_partsupp",
)

gen_string_constant_header_file(
    name = "region_tbl",
    src = "region.tbl",
    variable = "kTpchData_region",
)

gen_string_constant_header_file(
    name = "supplier_tbl",
    src = "supplier.tbl",
    variable = "kTpchData_supplier",
)

cc_library(
    name = "catalog",
    srcs = [
        "customer.tbl.h",
        "lineitem.tbl.h",
        "nation.tbl.h",
        "orders.tbl.h",
        "part.tbl.h",
        "partsupp.tbl.h",
        "region.tbl.h",
        "supplier.tbl.h",
        "tpch_catalog.cc",
    ],
    hdrs = ["tpch_catalog.h"],
    deps = [
        "//zetasql/base:clock",
        "//zetasql/base:map_util",
        "//zetasql/base:ret_check",
        "//zetasql/base:status",
        "//zetasql/common:simple_evaluator_table_iterator",
        "//zetasql/public:analyzer_options",
        "//zetasql/public:analyzer_output",
        "//zetasql/public:catalog",
        "//zetasql/public:evaluator_table_iterator",
        "//zetasql/public:simple_catalog",
        "//zetasql/public:simple_catalog_util",
        "//zetasql/public:type",
        "//zetasql/public:value",
        "//zetasql/public/functions:date_time_util",
        "//zetasql/public/types",
        "//zetasql/resolved_ast:resolved_node_kind_cc_proto",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@com_google_riegeli//riegeli/base:initializer",
        "@com_google_riegeli//riegeli/bytes:string_reader",
        "@com_google_riegeli//riegeli/csv:csv_reader",
    ],
)

cc_test(
    name = "tpch_catalog_test",
    srcs = ["tpch_catalog_test.cc"],
    deps = [
        ":catalog",
        "//zetasql/base/testing:status_matchers",
        "//zetasql/base/testing:zetasql_gtest_main",
        "//zetasql/public:catalog",
        "//zetasql/public:evaluator_table_iterator",
        "//zetasql/public:simple_catalog",
    ],
)
