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

#include <any>
#include <functional>
#include <string>

#include "googlesql/public/catalog.h"
#include "googlesql/public/types/row_type.h"
#include "googlesql/public/types/type.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql {

static absl::StatusOr<Type::HasFieldResult> TableHasColumn(
    const Table& table, absl::string_view name, std::any* context) {
  if (table.GetColumnListMode() == Table::ColumnListMode::DEFAULT) {
    // With non-lazy tables, we can check for columns cheaply and accurately.
    const Column* column = table.FindColumnByName(std::string(name));
    return column == nullptr ? Type::HAS_NO_FIELD : Type::HAS_FIELD;
  } else {
    absl::StatusOr<const Column*> find_result =
        table.FindLazyColumn(name, context);
    if (!find_result.ok()) {
      // kNotFound can be used to indicate not found, other errors are returned.
      if (absl::IsNotFound(find_result.status())) {
        return Type::HAS_NO_FIELD;
      }
      return find_result.status();
    }
    // nullptr can be used to indicate not found.
    return *find_result == nullptr ? Type::HAS_NO_FIELD : Type::HAS_FIELD;
  }
}

// This is the setter in row_type.cc used to install the callback.
using HasColumnCallbackType =
    std::function<absl::StatusOr<Type::HasFieldResult>(
        const Table&, absl::string_view, std::any*)>;
void SetRowTypeHasColumnColumnCallback(HasColumnCallbackType callback);

// Install callbacks to call Catalog methods.
// To avoid a circular dependency, these are installed from a static
// initializer in the `:type_with_catalog_impl` build target.
static void RegisterCatalogCallbacks() {
  SetRowTypeHasColumnColumnCallback(&TableHasColumn);
}

namespace {
static bool module_initialization_complete = []() {
  RegisterCatalogCallbacks();
  return true;
} ();
}  // namespace

}  // namespace googlesql
