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

#include <cmath>
#include <vector>

#include "googlesql/compliance/functions_testlib.h"
#include "googlesql/public/cast.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/value.h"
#include "googlesql/testing/test_function.h"
#include "googlesql/testing/test_value.h"

namespace googlesql {

namespace {
bool ContainsNonFiniteFloat(const Value& v) {
  if (v.is_null()) return false;
  switch (v.type()->kind()) {
    case TYPE_FLOAT:
      return !std::isfinite(v.float_value());
    case TYPE_DOUBLE:
      return !std::isfinite(v.double_value());
    case TYPE_ARRAY:
      for (const Value& element : v.elements()) {
        if (ContainsNonFiniteFloat(element)) return true;
      }
      return false;
    case TYPE_STRUCT:
      for (const Value& field : v.fields()) {
        if (ContainsNonFiniteFloat(field)) return true;
      }
      return false;
    case TYPE_RANGE:
      if (ContainsNonFiniteFloat(v.start())) return true;
      if (ContainsNonFiniteFloat(v.end())) return true;
      return false;
    case TYPE_MAP:
      for (const auto& [key, value] : v.map_entries()) {
        if (ContainsNonFiniteFloat(key)) return true;
        if (ContainsNonFiniteFloat(value)) return true;
      }
      return false;
    default:
      return false;
  }
}
}  // namespace

std::vector<QueryParamsWithResult> GetFunctionTestsCastJson() {
  std::vector<QueryParamsWithResult> tests;
  LanguageOptions language_options;
  language_options.EnableMaximumLanguageFeatures();

  for (const FunctionTestCall& test_call : GetFunctionTestsToJson()) {
    QueryParamsWithResult test_case = test_call.params;
    // Only take the cases without the second argument (stringify_wide_numbers).
    if (test_case.num_params() != 1) {
      continue;
    }

    const Value& from_value = test_case.param(0);
    const Type* from_type = from_value.type();

    // Skip types not supported in CAST as JSON.
    // TODO: b/500863341 - Once TO_JSON and CAST AS JSON are unified, we can
    // remove this check.
    if (!IsTypeCastableToJson(from_type, language_options)) {
      continue;
    }

    // Skip values containing infinite/NaN floats as they are unsupported in
    // CAST.
    if (ContainsNonFiniteFloat(from_value)) {
      continue;
    }

    // Adjust expected result for NULL inputs.
    // TO_JSON(NULL) returns JSON 'null' (non-NULL Value),
    // but CAST(NULL AS JSON) returns GoogleSQL NULL.
    if (from_value.is_null()) {
      test_case.SetResult(values::NullJson());
    }

    tests.push_back(test_case);
  }
  return tests;
}

}  // namespace googlesql
