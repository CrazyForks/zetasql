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

#include "zetasql/reference_impl/functions/uuid.h"

#include "zetasql/public/functions/uuid.h"
#include "zetasql/public/types/type.h"
#include "zetasql/public/types/type_factory.h"
#include "zetasql/public/value.h"
#include "zetasql/reference_impl/evaluation.h"
#include "zetasql/reference_impl/function.h"
#include "zetasql/reference_impl/tuple.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "zetasql/base/ret_check.h"

namespace zetasql {
namespace {
class GenerateUuidFunction : public SimpleBuiltinScalarFunction {
 public:
  GenerateUuidFunction()
      : SimpleBuiltinScalarFunction(FunctionKind::kGenerateUuid,
                                    types::StringType()) {}
  absl::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                             absl::Span<const Value> args,
                             EvaluationContext* context) const override;
};

absl::StatusOr<Value> GenerateUuidFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context) const {
  ZETASQL_RET_CHECK(args.empty());
  return Value::String(
      functions::GenerateUuid(*(context->GetRandomNumberGenerator())));
}

class NewUuidFunction : public SimpleBuiltinScalarFunction {
 public:
  NewUuidFunction()
      : SimpleBuiltinScalarFunction(FunctionKind::kGenerateUuid,
                                    types::UuidType()) {}
  absl::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                             absl::Span<const Value> args,
                             EvaluationContext* context) const override;
};

absl::StatusOr<Value> NewUuidFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context) const {
  ZETASQL_RET_CHECK(args.empty());
  return Value::Uuid(
      functions::NewUuid(*(context->GetRandomNumberGenerator())));
}

}  // namespace

void RegisterBuiltinUuidFunctions() {
  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kGenerateUuid},
      [](FunctionKind kind, const Type* output_type) {
        return new GenerateUuidFunction();
      });
  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kNewUuid}, [](FunctionKind kind, const Type* output_type) {
        return new NewUuidFunction();
      });
}

}  // namespace zetasql
