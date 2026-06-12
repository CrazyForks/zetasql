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

#include "googlesql/public/catalog_wrapper.h"

#include <string>

namespace googlesql {

absl::Status FunctionCatalogWrapper::FindFunction(
    const absl::Span<const std::string>& path,
    const ::googlesql::Function** function, const FindOptions& options) {
  return wrapped_catalog_->FindFunction(path, function, options);
}

}  // namespace googlesql
