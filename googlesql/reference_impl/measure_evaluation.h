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

#ifndef GOOGLESQL_REFERENCE_IMPL_MEASURE_EVALUATION_H_
#define GOOGLESQL_REFERENCE_IMPL_MEASURE_EVALUATION_H_

#include <string>

#include "googlesql/public/catalog.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace googlesql {

class MeasureType;

// Maintains the mapping between a MEASURE-typed ResolvedColumn and the
// expression used to compute the measure. The expression used to compute the
// measure is stored in the catalog (not the ResolvedColumn or the type), hence
// why we need to track it here.
class MeasureColumnToExprMapping {
 public:
  MeasureColumnToExprMapping() = default;
  // Disallow copy, but allow move.
  MeasureColumnToExprMapping(const MeasureColumnToExprMapping& other) = delete;
  MeasureColumnToExprMapping& operator=(
      const MeasureColumnToExprMapping& other) = delete;
  MeasureColumnToExprMapping(MeasureColumnToExprMapping&& other) = default;
  MeasureColumnToExprMapping& operator=(MeasureColumnToExprMapping&& other) =
      default;

  // Track expressions for any measure columns emitted by `table_scan`.
  absl::Status TrackMeasureColumnsEmittedByTableScan(
      const ResolvedTableScan& table_scan);

  // Track expressions for any measure columns emitted by `tvf_scan`.
  absl::Status TrackMeasureColumnsEmittedByTVFScan(
      const ResolvedTVFScan& tvf_scan);

  // Find the measure expression for the given `measure_type`.
  absl::StatusOr<const ResolvedExpr*> GetMeasureExpr(
      const MeasureType* measure_type) const;

 private:
  // Tracks measure columns emitted by `scan`.
  template <typename ScanType>
  absl::Status TrackMeasureColumnsEmittedByScan(const ScanType& scan);

  // Adds a MEASURE-typed `measure_type` with the given `expr`.
  absl::Status AddMeasureColumnWithExpr(const MeasureType* measure_type,
                                        const ResolvedExpr* expr);

  absl::flat_hash_map<const MeasureType*, const ResolvedExpr*>
      measure_column_to_expr_;
};

}  // namespace googlesql

#endif  // GOOGLESQL_REFERENCE_IMPL_MEASURE_EVALUATION_H_
