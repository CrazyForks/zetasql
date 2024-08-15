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

#ifndef ZETASQL_EXAMPLES_TPCH_CATALOG_TPCH_CATALOG_H_
#define ZETASQL_EXAMPLES_TPCH_CATALOG_TPCH_CATALOG_H_

#include <memory>

#include "zetasql/public/simple_catalog.h"
#include "absl/status/statusor.h"

namespace zetasql {

// Make a SimpleCatalog containing the eight tables from TPC-H.
// The tables include a callback to return data.
// A new catalog is made each time, but data is shared across all instances.
// This uses 1MB of source data linked in as a cc_embed_data.
absl::StatusOr<std::unique_ptr<SimpleCatalog>> MakeTpchCatalog();

}  // namespace zetasql

#endif  // ZETASQL_EXAMPLES_TPCH_CATALOG_TPCH_CATALOG_H_
