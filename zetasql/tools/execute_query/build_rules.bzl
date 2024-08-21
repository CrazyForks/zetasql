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

"""
This build rule invokes `execute_query`, running the SQL from `sql_file`.
`args` are passed as additional command-line arguments to `execute_query`.
This just runs the script and validates that it succeeds, without checking
query output.
"""

def execute_query_test(
        name,
        sql_file,
        args = [],
        **kwargs):
    native.sh_test(
        name = name,
        srcs = ["//zetasql/tools/execute_query:run_execute_query_test.sh"],
        args = [
            "$(location //zetasql/tools/execute_query)",
            "$(location " + sql_file + ")",
        ] + args,
        data = [
            sql_file,
            # bazel doesn't support listing this as `tools`.
            "//zetasql/tools/execute_query",
        ],
        **kwargs
    )
