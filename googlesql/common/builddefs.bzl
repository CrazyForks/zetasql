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

""" Macros for the creation of module test targets """

load("@rules_cc//cc:cc_test.bzl", "cc_test")

def gen_module_test(filename):
    """ Create a module_test using module definitions in testdata/<filename>."""
    name = "module_" + filename.replace(".", "_")
    datafile = "testdata/" + filename
    cc_test(
        name = name,
        size = "small",
        data = [datafile],
        args = ["--test_file_spec_pattern=" + filename],
        deps = [":run_modules_test_lib"],
    )
    return ":" + name
