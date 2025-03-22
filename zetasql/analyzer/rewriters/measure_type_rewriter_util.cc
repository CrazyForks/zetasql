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

#include "zetasql/analyzer/rewriters/measure_type_rewriter_util.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zetasql/public/builtin_function.pb.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/function.h"
#include "zetasql/public/function_signature.h"
#include "zetasql/public/id_string.h"
#include "zetasql/public/types/struct_type.h"
#include "zetasql/public/types/type.h"
#include "zetasql/public/types/type_factory.h"
#include "zetasql/resolved_ast/column_factory.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "zetasql/resolved_ast/resolved_ast_builder.h"
#include "zetasql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "zetasql/resolved_ast/resolved_ast_rewrite_visitor.h"
#include "zetasql/resolved_ast/resolved_ast_visitor.h"
#include "zetasql/resolved_ast/resolved_column.h"
#include "zetasql/resolved_ast/resolved_node.h"
#include "zetasql/resolved_ast/rewrite_utils.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

static constexpr char kReferencedColumnsFieldName[] = "referenced_columns";
static constexpr char kKeyColumnsFieldName[] = "key_columns";
static constexpr int kReferencedColumnsFieldIndex = 0;
static constexpr int kKeyColumnsFieldIndex = 1;

////////////////////////////////////////////////////////////////////////
// Utility functions.
////////////////////////////////////////////////////////////////////////

static bool IsMeasureAggFunction(const ResolvedExpr* expr) {
  if (!expr->Is<ResolvedAggregateFunctionCall>()) {
    return false;
  }
  const ResolvedAggregateFunctionCall* agg_fn =
      expr->GetAs<ResolvedAggregateFunctionCall>();
  const Function* function = agg_fn->function();
  return function->NumSignatures() == 1 &&
         function->signatures()[0].context_id() == FN_AGG &&
         function->IsZetaSQLBuiltin();
}

// Assumes `aggregate_fn` is the builtin function `AGGREGATE(MEASURE<T>) => T`.
static absl::StatusOr<ResolvedColumn> GetInvokedMeasureColumn(
    const ResolvedAggregateFunctionCall* aggregate_fn) {
  ZETASQL_RET_CHECK(aggregate_fn != nullptr);
  ZETASQL_RET_CHECK(aggregate_fn->argument_list().size() == 1);
  const ResolvedExpr* arg = aggregate_fn->argument_list()[0].get();
  ZETASQL_RET_CHECK(arg->Is<ResolvedColumnRef>());
  return arg->GetAs<ResolvedColumnRef>()->column();
}

static absl::btree_set<int> GetRowIdentityColumnIndexes(const Table* table) {
  std::vector<int> tmp =
      table->RowIdentityColumns().value_or(std::vector<int>{});
  return absl::btree_set<int>(tmp.begin(), tmp.end());
}

////////////////////////////////////////////////////////////////////////
// Measure grain scan retrieval logic.
////////////////////////////////////////////////////////////////////////

absl::StatusOr<GrainScanInfo> GrainScanInfo::CreateFromTableScan(
    const ResolvedTableScan* grain_scan,
    MeasureExpansionInfoMap& measure_expansion_info_map,
    ColumnFactory& column_factory) {
  ZETASQL_RET_CHECK_EQ(grain_scan->column_list_size(),
               grain_scan->column_index_list_size());

  // Gather row identity columns, since we will always need to project them.
  absl::btree_set<int> row_id_column_indexes =
      GetRowIdentityColumnIndexes(grain_scan->table());
  ZETASQL_RET_CHECK(!row_id_column_indexes.empty());
  // Create the `GrainScanInfo` object, and track columns to project.
  GrainScanInfo grain_scan_info(grain_scan->table()->Name(), column_factory);
  // Go over columns already projected by the grain scan. If the column is a
  // measure column that needs to be expanded, track it accordingly. Else, mark
  // it for projection.
  const Table* table = grain_scan->table();
  bool at_least_one_measure_column_to_expand = false;
  for (int idx = 0; idx < grain_scan->column_list_size(); ++idx) {
    const int table_column_index = grain_scan->column_index_list(idx);
    const Column* column = table->GetColumn(table_column_index);
    const ResolvedColumn& resolved_column = grain_scan->column_list(idx);
    if (measure_expansion_info_map.contains(resolved_column)) {
      ZETASQL_RET_CHECK(column->HasMeasureExpression());
      const ResolvedExpr* measure_expr =
          column->GetExpression()->GetResolvedExpression();
      ZETASQL_RET_CHECK(measure_expr != nullptr);
      measure_expansion_info_map[resolved_column].measure_expr = measure_expr;
      at_least_one_measure_column_to_expand = true;
      continue;
    }
    ZETASQL_RETURN_IF_ERROR(grain_scan_info.MarkColumnForProjection(
        column->Name(), grain_scan,
        /*mark_row_identity_column=*/
        row_id_column_indexes.contains(table_column_index)));
  };
  ZETASQL_RET_CHECK(at_least_one_measure_column_to_expand);

  // Next, mark all row identity columns for projection. If a row identity
  // column is already projected, it will be a no-op.
  for (int idx : row_id_column_indexes) {
    const Column* column = table->GetColumn(idx);
    ZETASQL_RETURN_IF_ERROR(grain_scan_info.MarkColumnForProjection(
        column->Name(), grain_scan, /*mark_row_identity_column=*/true));
  }
  return grain_scan_info;
}

absl::Status GrainScanInfo::MarkColumnForProjection(
    std::string column_name, const ResolvedTableScan* grain_scan,
    bool mark_row_identity_column) {
  if (columns_to_project_.contains(column_name)) {
    return absl::OkStatus();
  }
  // Find the catalog column index for the column name.
  int catalog_column_index = -1;
  const Table* table = grain_scan->table();
  const Type* column_type = nullptr;
  for (int idx = 0; idx < table->NumColumns(); ++idx) {
    const Column* column = table->GetColumn(idx);
    if (column->Name() == column_name) {
      catalog_column_index = idx;
      column_type = column->GetType();
      break;
    }
  }
  ZETASQL_RET_CHECK(catalog_column_index >= 0);
  ZETASQL_RET_CHECK(column_type != nullptr);
  ZETASQL_RET_CHECK_EQ(grain_scan->column_index_list_size(),
               grain_scan->column_list_size());
  // Check if this column is already projected.
  for (int idx = 0; idx < grain_scan->column_index_list_size(); ++idx) {
    if (grain_scan->column_index_list(idx) == catalog_column_index) {
      // Column is already projected; just use that column.
      const ResolvedColumn& already_projected_column =
          grain_scan->column_list(idx);
      columns_to_project_.insert(
          {column_name,
           {.resolved_column = already_projected_column,
            .is_row_identity_column = mark_row_identity_column,
            .catalog_column_index = catalog_column_index}});
      return absl::OkStatus();
    }
  }

  // If the column is not already projected, create a new column.
  ResolvedColumn projected_column =
      column_factory_.MakeCol(scan_name_, column_name, column_type);
  columns_to_project_.insert(
      {column_name,
       {.resolved_column = projected_column,
        .is_row_identity_column = mark_row_identity_column,
        .catalog_column_index = catalog_column_index}});
  return absl::OkStatus();
}

absl::btree_set<std::string> GrainScanInfo::GetRowIdentityColumnNames() const {
  absl::btree_set<std::string> row_identity_column_names;
  for (const auto& [column_name, column_to_project] : columns_to_project_) {
    if (column_to_project.is_row_identity_column) {
      row_identity_column_names.insert(column_name);
    }
  }
  return row_identity_column_names;
}

// `GrainScanFinder` traverses the ResolvedAST twice to gather grain scan
// information.
//
// The 1st phase (`GATHER_MEASURE_INFO`) gathers information about measure
// columns that need to be expanded, (including any renamed measure columns).
//
// The 2nd phase (`CONSTRUCT_GRAIN_SCAN_INFO`) uses information gathered during
// the 1st phase to identify grain scans that require rewriting, and construct
// `GrainScanInfo` objects accordingly.
//
// Measure columns that directly originate from grain scans are added as keys to
// `MeasureExpansionInfoMap`, with renamed measure columns populated in the
// `renamed_measure_columns` field of the corresponding `MeasureExpansionInfo`
// value.
class GrainScanFinder : public ResolvedASTVisitor {
 public:
  static absl::StatusOr<
      absl::flat_hash_map<const ResolvedTableScan*, GrainScanInfo>>
  GetGrainScanInfo(const ResolvedNode* input,
                   MeasureExpansionInfoMap& measure_expansion_info_map,
                   ColumnFactory& column_factory) {
    // First, gather information about measure columns that need to be expanded.
    GrainScanFinder grain_scan_finder(input, measure_expansion_info_map,
                                      column_factory);
    ZETASQL_RETURN_IF_ERROR(
        grain_scan_finder.PerformVisit(VisitPhase::GATHER_MEASURE_INFO));

    // Then, track renamed measure columns.
    ZETASQL_RETURN_IF_ERROR(grain_scan_finder.TrackRenamedMeasureColumns());

    // Lastly, construct `GrainScanInfo` objects for each grain scan that needs
    // to be rewritten.
    ZETASQL_RETURN_IF_ERROR(
        grain_scan_finder.PerformVisit(VisitPhase::CONSTRUCT_GRAIN_SCAN_INFO));
    return std::move(grain_scan_finder.grain_scan_info_);
  }

  // Find measure columns invoked via the `AGGREGATE` function and place them in
  // `invoked_measure_columns_`.
  absl::Status VisitResolvedAggregateScan(
      const ResolvedAggregateScan* node) override {
    if (visit_phase_ == VisitPhase::GATHER_MEASURE_INFO) {
      for (const std::unique_ptr<const ResolvedComputedColumnBase>&
               computed_column : node->aggregate_list()) {
        if (IsMeasureAggFunction(computed_column->expr())) {
          ZETASQL_ASSIGN_OR_RETURN(ResolvedColumn invoked_measure_column,
                           GetInvokedMeasureColumn(
                               computed_column->expr()
                                   ->GetAs<ResolvedAggregateFunctionCall>()));
          invoked_measure_columns_.insert(invoked_measure_column);
        }
      }
    }
    return DefaultVisit(node);
  }

  // `WithRefScan` is a scan type that can rename a measure columns. Track
  // information about measure columns renamed by `WithRefScan` in
  // `renamed_measure_column_to_with_ref_scan_`. This information will be used
  // by `TrackRenamedMeasureColumns` to perform a rename chain traversal to
  // find the original measure column that needs to be expanded.
  //
  // See comments on `renamed_measure_column_to_with_ref_scan_` and
  // `with_query_name_to_with_entry_` for more information on measure column
  // renaming.
  absl::Status VisitResolvedWithRefScan(
      const ResolvedWithRefScan* node) override {
    if (visit_phase_ == VisitPhase::GATHER_MEASURE_INFO) {
      for (const ResolvedColumn& column : node->column_list()) {
        if (column.type()->IsMeasureType()) {
          ZETASQL_RET_CHECK(
              renamed_measure_column_to_with_ref_scan_.insert({column, node})
                  .second);
        }
      }
    }
    return DefaultVisit(node);
  }

  // Used to track information about renamed measure columns. See comments on
  // `VisitResolvedWithRefScan` for more.
  absl::Status VisitResolvedWithEntry(const ResolvedWithEntry* node) override {
    if (visit_phase_ == VisitPhase::GATHER_MEASURE_INFO) {
      ZETASQL_RET_CHECK(
          with_query_name_to_with_entry_.insert({node->with_query_name(), node})
              .second);
    }
    return DefaultVisit(node);
  }

  absl::Status VisitResolvedTableScan(const ResolvedTableScan* node) override {
    if (visit_phase_ == VisitPhase::CONSTRUCT_GRAIN_SCAN_INFO) {
      for (const ResolvedColumn& column : node->column_list()) {
        if (!measure_expansion_info_map_.contains(column)) {
          continue;
        }
        // `node` is a grain scan for a measure column that needs to be
        // expanded.
        ZETASQL_ASSIGN_OR_RETURN(
            GrainScanInfo grain_scan_info,
            GrainScanInfo::CreateFromTableScan(
                node, measure_expansion_info_map_, column_factory_));
        ZETASQL_RET_CHECK(
            grain_scan_info_.insert({node, std::move(grain_scan_info)}).second);
        break;
      }
    }
    return DefaultVisit(node);
  }

 private:
  explicit GrainScanFinder(const ResolvedNode* input,
                           MeasureExpansionInfoMap& measure_expansion_info_map,
                           ColumnFactory& column_factory)
      : input_(input),
        measure_expansion_info_map_(measure_expansion_info_map),
        column_factory_(column_factory) {}
  GrainScanFinder(const GrainScanFinder& other) = delete;
  GrainScanFinder& operator=(const GrainScanFinder& other) = delete;

  enum class VisitPhase { GATHER_MEASURE_INFO, CONSTRUCT_GRAIN_SCAN_INFO };

  absl::Status PerformVisit(VisitPhase visit_phase) {
    visit_phase_ = visit_phase;
    return input_->Accept(this);
  }

  // Measure columns propagating through `WithRefScan` are renamed to use new
  // column ids. Given `renamed_measure_column`, this function returns the
  // original measure column that needs to be expanded.
  absl::StatusOr<ResolvedColumn> GetOriginalMeasureColumn(
      const ResolvedColumn& renamed_measure_column) {
    if (!renamed_measure_column_to_with_ref_scan_.contains(
            renamed_measure_column)) {
      return renamed_measure_column;
    }
    const ResolvedWithRefScan* with_ref_scan =
        renamed_measure_column_to_with_ref_scan_.at(renamed_measure_column);
    ZETASQL_RET_CHECK(with_query_name_to_with_entry_.contains(
        with_ref_scan->with_query_name()));
    const ResolvedWithEntry* with_entry =
        with_query_name_to_with_entry_.at(with_ref_scan->with_query_name());
    ZETASQL_RET_CHECK(with_ref_scan->column_list().size() ==
              with_entry->with_subquery()->column_list_size());
    auto it =
        std::find(with_ref_scan->column_list().begin(),
                  with_ref_scan->column_list().end(), renamed_measure_column);
    ZETASQL_RET_CHECK(it != with_ref_scan->column_list().end());
    int index = static_cast<int>(
        std::distance(with_ref_scan->column_list().begin(), it));
    return with_entry->with_subquery()->column_list(index);
  }

  // Columns in `invoked_measure_columns_` may be renamed measure columns;
  // `TrackRenamedMeasureColumns` will read each column from this set and
  // perform a rename chain traversal to find the original measure column that
  // needs to be expanded.
  absl::Status TrackRenamedMeasureColumns() {
    ZETASQL_RET_CHECK(measure_expansion_info_map_.empty());
    for (const ResolvedColumn& invoked_measure_column :
         invoked_measure_columns_) {
      constexpr int kMaxIterations = 10;
      int depth = 0;
      std::vector<ResolvedColumn> renamed_measure_columns;
      ResolvedColumn current_measure_column = invoked_measure_column;
      while (true) {
        ZETASQL_ASSIGN_OR_RETURN(ResolvedColumn original_measure_column,
                         GetOriginalMeasureColumn(current_measure_column));
        if (original_measure_column == current_measure_column) {
          std::reverse(renamed_measure_columns.begin(),
                       renamed_measure_columns.end());
          ZETASQL_RET_CHECK(measure_expansion_info_map_
                        .insert({original_measure_column,
                                 {.renamed_measure_columns =
                                      std::move(renamed_measure_columns)}})
                        .second);
          break;
        }
        renamed_measure_columns.push_back(current_measure_column);
        current_measure_column = original_measure_column;
        if (++depth > kMaxIterations) {
          return absl::InvalidArgumentError(absl::StrCat(
              "Measure column ", invoked_measure_column.DebugString(),
              " was renamed more than ", kMaxIterations, " times."));
        }
      }
    }
    return absl::OkStatus();
  }

  // The input ResolvedAST.
  const ResolvedNode* input_;
  // Holds information about measure columns that need to be expanded.
  MeasureExpansionInfoMap& measure_expansion_info_map_;
  // A column factory used to create new columns.
  ColumnFactory& column_factory_;
  // The current phase of the visit.
  VisitPhase visit_phase_ = VisitPhase::GATHER_MEASURE_INFO;
  // The set of measure columns that are invoked in an `AGGREGATE` function
  // call. Populated during the `GATHER_MEASURE_INFO` phase.
  absl::flat_hash_set<ResolvedColumn> invoked_measure_columns_;
  // `renamed_measure_column_to_with_ref_scan_` and
  // `with_query_name_to_with_entry_` are populated during the
  // `GATHER_MEASURE_INFO` phase and used to track renamed measure columns.
  //
  // Certain query shapes can trigger measure column renames; consider this
  // ResolvedAST subtree, where 'renamed_measure#5' is a rename of
  // 'original_measure#4':
  //
  // +-WithScan
  //   +-column_list=[$aggregate.$agg1#6]
  //   +-with_entry_list=
  //   | +-WithEntry
  //   |   +-with_query_name="t"
  //   |   +-with_subquery=
  //   |     +-ProjectScan
  //   |       +-column_list=[MeasureTable.original_measure#4]
  //   |       +-input_scan=
  //   |         +-TableScan(column_list=[MeasureTable.original_measure#4]...)
  //   +-query=
  //     +-ProjectScan
  //       +-column_list=[$aggregate.$agg1#6]
  //       +-input_scan=
  //         +-AggregateScan
  //           +-column_list=[$aggregate.$agg1#6]
  //           +-input_scan=
  //           | +-ProjectScan
  //           |   +-column_list=[t.renamed_measure#5]
  //           |   +-input_scan=
  //           |     +-WithRefScan(
  //           |         column_list=[t.renamed_measure#5],
  //           |         with_query_name="t")
  //           +-aggregate_list=
  //             +-$agg1#6 :=
  //               +-AggregateFunctionCall(ZetaSQL:AGGREGATE(...))
  //                 +-ColumnRef(type=..., column=t.renamed_measure#5)
  //
  // In this scenario, 'renamed_measure#5' is the invoked measure column, but
  // the measure column that needs to be expanded is 'original_measure#4'.
  // Handling this correctly requires tracking additional information and
  // performing a rename chain traversal to find the original measure column.
  absl::flat_hash_map<ResolvedColumn, const ResolvedWithRefScan*>
      renamed_measure_column_to_with_ref_scan_;
  absl::flat_hash_map<std::string, const ResolvedWithEntry*>
      with_query_name_to_with_entry_;

  // A mapping from a grain scan to the `GrainScanInfo` needed to rewrite it.
  // Populated during the `CONSTRUCT_GRAIN_SCAN_INFO` phase.
  absl::flat_hash_map<const ResolvedTableScan*, GrainScanInfo> grain_scan_info_;
};

absl::StatusOr<absl::flat_hash_map<const ResolvedTableScan*, GrainScanInfo>>
GetGrainScanInfo(const ResolvedNode* input,
                 MeasureExpansionInfoMap& measure_expansion_info_map,
                 ColumnFactory& column_factory) {
  return GrainScanFinder::GetGrainScanInfo(input, measure_expansion_info_map,
                                           column_factory);
}

////////////////////////////////////////////////////////////////////////
// Measure expansion logic.
////////////////////////////////////////////////////////////////////////

// `ReferencedColumnFinder` finds the set of columns referenced by a measure
// expression and marks them for projection in the corresponding
// `GrainScanInfo`.
class ReferencedColumnFinder : public ResolvedASTVisitor {
 public:
  static absl::StatusOr<absl::btree_set<std::string>> GetReferencedColumns(
      const ResolvedExpr* expr, const ResolvedTableScan* grain_scan,
      GrainScanInfo& grain_scan_info) {
    ReferencedColumnFinder finder(grain_scan, grain_scan_info);
    ZETASQL_RETURN_IF_ERROR(expr->Accept(&finder));
    return finder.GetReferencedColumns();
  }

  absl::btree_set<std::string> GetReferencedColumns() {
    return referenced_columns_;
  }

  absl::Status VisitResolvedExpressionColumn(
      const ResolvedExpressionColumn* node) override {
    if (referenced_columns_.contains(node->name())) {
      return absl::OkStatus();
    }
    referenced_columns_.insert(node->name());
    // `node` is either a row identity column or a column referenced by the
    // measure expression. If it is a row identity column, it has already been
    // marked for projection when `grain_scan_info_` was created, so
    // MarkColumnForProjection is a no-op. So it is safe to set
    // `mark_row_identity_column` as false.
    ZETASQL_RETURN_IF_ERROR(grain_scan_info_.MarkColumnForProjection(
        node->name(), grain_scan_, /*mark_row_identity_column=*/false));
    return DefaultVisit(node);
  }

 private:
  explicit ReferencedColumnFinder(const ResolvedTableScan* grain_scan,
                                  GrainScanInfo& grain_scan_info)
      : grain_scan_(grain_scan), grain_scan_info_(grain_scan_info) {};
  ReferencedColumnFinder(const ReferencedColumnFinder& other) = delete;
  ReferencedColumnFinder& operator=(const ReferencedColumnFinder& other) =
      delete;

  const ResolvedTableScan* grain_scan_;
  GrainScanInfo& grain_scan_info_;
  absl::btree_set<std::string> referenced_columns_;
};

// Creates a `MakeStruct` expression from `column_names` projected by
// `grain_scan_info`.
absl::StatusOr<std::unique_ptr<ResolvedExpr>> MakeStructExprFromColumnNames(
    const absl::btree_set<std::string>& column_names,
    const GrainScanInfo& grain_scan_info, TypeFactory& type_factory) {
  std::vector<StructField> struct_fields;
  std::vector<std::unique_ptr<const ResolvedExpr>> struct_field_exprs;
  for (const std::string& column_name : column_names) {
    ZETASQL_ASSIGN_OR_RETURN(GrainScanInfo::ColumnToProject column_to_project,
                     grain_scan_info.GetColumnToProject(column_name));
    const ResolvedColumn& resolved_column = column_to_project.resolved_column;
    struct_fields.push_back(StructField(column_name, resolved_column.type()));
    struct_field_exprs.push_back(
        MakeResolvedColumnRef(resolved_column.type(), resolved_column,
                              /*is_correlated=*/false));
  }
  const StructType* struct_type = nullptr;
  ZETASQL_RETURN_IF_ERROR(
      type_factory.MakeStructTypeFromVector(struct_fields, &struct_type));
  return MakeResolvedMakeStruct(struct_type, std::move(struct_field_exprs));
}

// `MakeAndProjectStructColumn` populates `measure_expansion_info` with a
// `STRUCT` typed column that will be used to replace the measure column, and
// updates `grain_scan_info` to project the `STRUCT` column and columns needed
// to construct the `STRUCT` column.
static absl::Status MakeAndProjectStructColumn(
    const ResolvedColumn& resolved_measure_column,
    MeasureExpansionInfo& measure_expansion_info,
    const ResolvedTableScan* grain_scan, GrainScanInfo& grain_scan_info,
    TypeFactory& type_factory, IdStringPool& id_string_pool) {
  const ResolvedExpr* measure_expr = measure_expansion_info.measure_expr;
  ZETASQL_RET_CHECK(measure_expr != nullptr);
  // Step 1: Traverse the measure expression to find referenced columns and
  // update `grain_scan_info` to project the referenced columns (if not already
  // projected).
  ZETASQL_ASSIGN_OR_RETURN(absl::btree_set<std::string> referenced_columns,
                   ReferencedColumnFinder::GetReferencedColumns(
                       measure_expr, grain_scan, grain_scan_info));
  // Step 2: Construct a `ResolvedMakeStruct` expression wrapping the
  // referenced columns.
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedExpr> referenced_columns_struct_expr,
                   MakeStructExprFromColumnNames(
                       referenced_columns, grain_scan_info, type_factory));

  // Step 3: Construct a `ResolvedMakeStruct` expression wrapping key columns
  // from the source scan.
  ZETASQL_ASSIGN_OR_RETURN(
      std::unique_ptr<ResolvedExpr> key_columns_struct_expr,
      MakeStructExprFromColumnNames(grain_scan_info.GetRowIdentityColumnNames(),
                                    grain_scan_info, type_factory));

  // Step 4: Construct a `ResolvedMakeStruct` expression wrapping the STRUCT
  // expressions from steps 2 and 3.
  // TODO: b/350555383 - Consider refactoring into a helper function.
  const StructType* final_struct_type = nullptr;
  std::vector<StructField> final_struct_fields;
  final_struct_fields.push_back(StructField(
      kReferencedColumnsFieldName, referenced_columns_struct_expr->type()));
  final_struct_fields.push_back(
      StructField(kKeyColumnsFieldName, key_columns_struct_expr->type()));
  ZETASQL_RETURN_IF_ERROR(type_factory.MakeStructTypeFromVector(final_struct_fields,
                                                        &final_struct_type));
  std::vector<std::unique_ptr<const ResolvedExpr>> final_struct_field_exprs;
  final_struct_field_exprs.push_back(std::move(referenced_columns_struct_expr));
  final_struct_field_exprs.push_back(std::move(key_columns_struct_expr));
  std::unique_ptr<ResolvedMakeStruct> final_struct_expr =
      MakeResolvedMakeStruct(final_struct_type,
                             std::move(final_struct_field_exprs));

  // Step 5: Construct a `ResolvedComputedColumn` for the `ResolvedMakeStruct`
  // expression from step 4 and add it to `grain_scan_info`. Also update
  // `measure_expansion_info` with the `STRUCT` column.
  ResolvedColumn final_struct_column = ResolvedColumn(
      resolved_measure_column.column_id(),
      resolved_measure_column.table_name_id(),
      id_string_pool.Make(
          absl::StrCat("struct_for_measure_", resolved_measure_column.name())),
      final_struct_expr->type());
  grain_scan_info.AddStructComputedColumn(MakeResolvedComputedColumn(
      final_struct_column, std::move(final_struct_expr)));
  measure_expansion_info.struct_column = final_struct_column;
  return absl::OkStatus();
}

// Creates a `MeasureExpansionInfoMap` for the renamed measure columns
// associated with `measure_expansion_info`.
static absl::StatusOr<MeasureExpansionInfoMap>
GetRenamedMeasureExpansionInfoMap(MeasureExpansionInfo& measure_expansion_info,
                                  GrainScanInfo& grain_scan_info,
                                  TypeFactory& type_factory,
                                  IdStringPool& id_string_pool) {
  MeasureExpansionInfoMap renamed_measure_expansion_info_map;
  for (const ResolvedColumn& renamed_measure_column :
       measure_expansion_info.renamed_measure_columns) {
    ResolvedColumn renamed_struct_column = ResolvedColumn(
        renamed_measure_column.column_id(),
        renamed_measure_column.table_name_id(),
        id_string_pool.Make(
            absl::StrCat("struct_for_measure_", renamed_measure_column.name())),
        measure_expansion_info.struct_column.type());
    MeasureExpansionInfo renamed_measure_expansion_info{
        .measure_expr = measure_expansion_info.measure_expr,
        .struct_column = renamed_struct_column,
    };
    ZETASQL_RET_CHECK(renamed_measure_expansion_info_map
                  .insert({renamed_measure_column,
                           std::move(renamed_measure_expansion_info)})
                  .second);
  }
  return renamed_measure_expansion_info_map;
}

absl::Status PopulateStructColumnInfo(
    const ResolvedTableScan* grain_scan, GrainScanInfo& grain_scan_info,
    MeasureExpansionInfoMap& measure_expansion_info_map,
    TypeFactory& type_factory, IdStringPool& id_string_pool) {
  std::vector<MeasureExpansionInfoMap> renamed_measure_expansion_info_maps;
  for (auto& [resolved_measure_column, measure_expansion_info] :
       measure_expansion_info_map) {
    // Populate `measure_expansion_info` with a `STRUCT` typed column that will
    // be used to replace the measure column.
    ZETASQL_RETURN_IF_ERROR(MakeAndProjectStructColumn(
        resolved_measure_column, measure_expansion_info, grain_scan,
        grain_scan_info, type_factory, id_string_pool));
    // Create a `MeasureExpansionInfoMap` for the renamed measure columns
    // associated with `measure_expansion_info`.
    ZETASQL_ASSIGN_OR_RETURN(MeasureExpansionInfoMap renamed_measure_expansion_info_map,
                     GetRenamedMeasureExpansionInfoMap(
                         measure_expansion_info, grain_scan_info, type_factory,
                         id_string_pool));
    renamed_measure_expansion_info_maps.push_back(
        std::move(renamed_measure_expansion_info_map));
  }
  // Merge MeasureExpansionInfoMaps for renamed measure columns into the global
  // `measure_expansion_info_map`.
  for (MeasureExpansionInfoMap& renamed_measure_expansion_info_map :
       renamed_measure_expansion_info_maps) {
    for (auto& [renamed_measure_column, renamed_measure_expansion_info] :
         renamed_measure_expansion_info_map) {
      ZETASQL_RET_CHECK(measure_expansion_info_map
                    .insert({renamed_measure_column,
                             std::move(renamed_measure_expansion_info)})
                    .second);
    }
  }
  return absl::OkStatus();
}

////////////////////////////////////////////////////////////////////////
// Measure rewriting logic.
////////////////////////////////////////////////////////////////////////

// `GrainScanRewriter` does 2 things:
//  1) Rewrites grain scans to project any additional columns needed for measure
//     expansion.
//  2) Layers a ProjectScan over the grain scan to compute the `STRUCT` typed
//     columns needed for measure expansion.
class GrainScanRewriter : public ResolvedASTRewriteVisitor {
 public:
  static absl::StatusOr<std::unique_ptr<const ResolvedNode>> RewriteGrainScans(
      std::unique_ptr<const ResolvedNode> input,
      absl::flat_hash_map<const ResolvedTableScan*, GrainScanInfo>
          grain_scan_info_map) {
    return GrainScanRewriter(std::move(grain_scan_info_map))
        .VisitAll(std::move(input));
  }

 protected:
  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedTableScan(
      std::unique_ptr<const ResolvedTableScan> scan) override {
    auto it = grain_scan_info_map_.find(scan.get());
    if (it == grain_scan_info_map_.end()) {
      return scan;
    }
    GrainScanInfo& grain_scan_info = it->second;
    // `scan` is a grain scan for one or more measures. Rewrite `scan` to
    // project the additional columns needed for measure expansion.
    ZETASQL_ASSIGN_OR_RETURN(
        scan, ProjectAdditionalColumns(std::move(scan), grain_scan_info));

    // Layer a ProjectScan over `scan` to compute the STRUCT columns for measure
    // expansion.
    return LayerProjectScanWithStructColumns(std::move(scan), grain_scan_info);
  }

 private:
  explicit GrainScanRewriter(
      absl::flat_hash_map<const ResolvedTableScan*, GrainScanInfo>
          grain_scan_info_map)
      : grain_scan_info_map_(std::move(grain_scan_info_map)) {};
  GrainScanRewriter(const GrainScanRewriter&) = delete;
  GrainScanRewriter& operator=(const GrainScanRewriter&) = delete;

  absl::StatusOr<std::unique_ptr<const ResolvedTableScan>>
  ProjectAdditionalColumns(std::unique_ptr<const ResolvedTableScan> scan,
                           GrainScanInfo& grain_scan_info) {
    std::vector<GrainScanInfo::ColumnToProject> columns_to_project =
        grain_scan_info.GetAllColumnsToProject();
    std::sort(columns_to_project.begin(), columns_to_project.end(),
              [](const auto& a, const auto& b) {
                return a.catalog_column_index < b.catalog_column_index;
              });
    ResolvedColumnList column_list;
    column_list.reserve(columns_to_project.size());
    std::vector<int> column_index_list;
    column_index_list.reserve(column_list.size());
    for (const GrainScanInfo::ColumnToProject& column_to_project :
         columns_to_project) {
      column_list.push_back(column_to_project.resolved_column);
      column_index_list.push_back(column_to_project.catalog_column_index);
    }
    return ToBuilder(std::move(scan))
        .set_column_list(std::move(column_list))
        .set_column_index_list(std::move(column_index_list))
        .Build();
  }

  absl::StatusOr<std::unique_ptr<const ResolvedProjectScan>>
  LayerProjectScanWithStructColumns(
      std::unique_ptr<const ResolvedTableScan> scan,
      GrainScanInfo& grain_scan_info) {
    std::vector<ResolvedColumn> project_scan_column_list = scan->column_list();
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        struct_computed_columns =
            grain_scan_info.release_struct_computed_columns();
    for (const auto& struct_computed_column : struct_computed_columns) {
      project_scan_column_list.push_back(struct_computed_column->column());
    }
    return MakeResolvedProjectScan(project_scan_column_list,
                                   std::move(struct_computed_columns),
                                   std::move(scan));
  }

  absl::flat_hash_map<const ResolvedTableScan*, GrainScanInfo>
      grain_scan_info_map_;
};

// `MultiLevelAggregateRewriter` rewrites a measure expression to use
// multi-level aggregation to grain-lock and avoid overcounting. A measure
// expression is a scalar expression over one or more constituent aggregate
// functions (e.g. SUM(X) / SUM(Y) + (<scalar_subquery>)), and so the resulting
// rewritten expression has 2 components to it:
//
// 1. A list of constituent aggregate functions that are written to use
//    multi-level aggregation to grain-lock. These aggregate functions need to
//    be computed by the AggregateScan.
//
// 2. A scalar expression over the constituent aggregate functions. This
//    expression needs to be computed by a ProjectScan over the AggregateScan.
//
// The scalar expression is rewritten to use column references to the
// constituent aggregate functions. The constituent aggregate functions are
// themselves rewritten to use multi-level aggregation to grain-lock and avoid
// overcounting.
//
// TODO: b/350555383 - Modify this to avoid traversing subquery subtrees in the
// measure expression. Measure expressions with subqueries don't need their
// aggregate functions rewritten to grain-lock; for example, consider this expr:
//
//    'SUM(int64_col) + (SELECT AVG(arr.field) FROM UNNEST(array_col) AS arr)'
//
// The equivalent rewritten expr should be be:
//
//    'SUM(ANY_VALUE(int64_col) GROUP BY <grain_lock_expr>) +
//     (SELECT AVG(arr.field) FROM UNNEST(array_col) AS arr)'
//
// Aggregate functions within the subquery should not require grain-locking.
class MultiLevelAggregateRewriter : public ResolvedASTRewriteVisitor {
 public:
  MultiLevelAggregateRewriter(const Function* any_value_fn,
                              ColumnFactory& column_factory,
                              ResolvedColumn struct_column)
      : any_value_fn_(any_value_fn),
        column_factory_(column_factory),
        struct_column_(struct_column) {};
  MultiLevelAggregateRewriter(const MultiLevelAggregateRewriter&) = delete;
  MultiLevelAggregateRewriter& operator=(const MultiLevelAggregateRewriter&) =
      delete;

  absl::StatusOr<std::unique_ptr<const ResolvedExpr>>
  RewriteMultiLevelAggregate(std::unique_ptr<const ResolvedExpr> measure_expr) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedNode> rewritten_measure_expr,
                     VisitAll(std::move(measure_expr)));
    ZETASQL_RET_CHECK(rewritten_measure_expr->Is<ResolvedExpr>());
    return absl::WrapUnique(
        rewritten_measure_expr.release()->GetAs<ResolvedExpr>());
  }

  std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>
  release_constituent_aggregate_list() {
    return std::move(constituent_aggregate_list_);
  }

 protected:
  absl::Status PreVisitResolvedAggregateFunctionCall(
      const ResolvedAggregateFunctionCall& aggregate_function_call) override {
    // If we're already seen a top-level aggregate function call, then
    // `aggregate_function_call` is not a top-level aggregate function call.
    if (top_level_aggregate_function_call_ != nullptr) {
      return absl::OkStatus();
    }
    // `aggregate_function_call` is a top-level aggregate function call.
    top_level_aggregate_function_call_ = &aggregate_function_call;
    return absl::OkStatus();
  }

  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedAggregateFunctionCall(
      std::unique_ptr<const ResolvedAggregateFunctionCall> node) override {
    const ResolvedAggregateFunctionCall* aggregate_function_call_ptr =
        node.get();
    auto reset_top_level_aggregate_function_call =
        absl::MakeCleanup([this, aggregate_function_call_ptr] {
          if (IsTopLevelAggregateFunctionCall(aggregate_function_call_ptr)) {
            top_level_aggregate_function_call_ = nullptr;
          }
        });

    // We only rewrite aggregate functions that have an empty
    // `group_by_aggregate_list`.
    if (!node->group_by_aggregate_list().empty()) {
      return ProcessAggregateFunction(std::move(node),
                                      aggregate_function_call_ptr);
    }
    // TODO: b/350555383 - How do we handle `generic_argument_list` ?
    if (!node->generic_argument_list().empty()) {
      return absl::UnimplementedError(
          "Measure type rewrite does not currently support generic arguments");
    }
    if (!node->group_by_list().empty()) {
      // `group_by_list` is not empty, but `group_by_aggregate_list` is empty.
      // This means that the aggregate function is a leaf node aggregate
      // function that only references grouping consts or correlated columns
      // (e.g. SUM(1 GROUP BY e)). We don't need to grain-lock since the
      // aggregate function is guaranteed to see exactly 1 row per group.
      return ProcessAggregateFunction(std::move(node),
                                      aggregate_function_call_ptr);
    }

    // If here, both `group_by_list` and `group_by_aggregate_list` are empty.
    // This is a plain aggregate function that needs to be rewritten to
    // grain-lock.
    ResolvedAggregateFunctionCallBuilder aggregate_function_call_builder =
        ToBuilder(std::move(node));

    // Step 1: Release the argument list, and wrap each argument with an
    // ANY_VALUE aggregate function call. These aggregate function calls will be
    // placed in the `group_by_aggregate_list` of the rewritten aggregate
    // function call.
    std::vector<std::unique_ptr<const ResolvedExpr>> argument_list =
        aggregate_function_call_builder.release_argument_list();
    std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>
        group_by_aggregate_list;
    for (int i = 0; i < argument_list.size(); ++i) {
      std::unique_ptr<const ResolvedExpr>& argument = argument_list[i];
      const Type* argument_type = argument->type();
      std::vector<std::unique_ptr<const ResolvedExpr>> any_value_argument_list;
      any_value_argument_list.push_back(std::move(argument));
      FunctionSignature any_value_signature({argument_type, 1},
                                            {{argument_type, 1}}, FN_ANY_VALUE);
      auto resolved_any_value_aggregate_function_call =
          MakeResolvedAggregateFunctionCall(
              argument_type, any_value_fn_, any_value_signature,
              std::move(any_value_argument_list), /*generic_argument_list=*/{},
              aggregate_function_call_builder.error_mode(), /*distinct=*/false,
              ResolvedNonScalarFunctionCallBase::DEFAULT_NULL_HANDLING,
              /*where_expr=*/nullptr, /*having_modifier=*/nullptr,
              /*order_by_item_list=*/{}, /*limit=*/nullptr,
              /*function_call_info=*/nullptr, /*group_by_list=*/{},
              /*group_by_aggregate_list=*/{}, /*having_expr=*/nullptr);
      group_by_aggregate_list.push_back(MakeResolvedComputedColumn(
          column_factory_.MakeCol("$aggregate",
                                  absl::StrCat("$any_value_grain_lock_", i),
                                  argument_type),
          std::move(resolved_any_value_aggregate_function_call)));
    }

    // Step 2: Change the argument list to be column references to the
    // `group_by_aggregate_list` columns.
    std::vector<std::unique_ptr<const ResolvedExpr>> rewritten_argument_list;
    rewritten_argument_list.reserve(group_by_aggregate_list.size());
    for (const auto& any_value_aggregate_computed_column :
         group_by_aggregate_list) {
      const ResolvedColumn& any_value_column =
          any_value_aggregate_computed_column->column();
      rewritten_argument_list.push_back(MakeResolvedColumnRef(
          any_value_column.type(), any_value_column, /*is_correlated=*/false));
    }

    // Step 3: Compute the `group_by_list`. This should just be a
    // `GetStructField` accessing the `kKeyColumnsFieldIndex` index field of the
    // `struct_column_`.
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> group_by_list;
    ZETASQL_RET_CHECK(struct_column_.type()->IsStruct());
    ZETASQL_RET_CHECK(struct_column_.type()->AsStruct()->num_fields() == 2);
    std::unique_ptr<ResolvedColumnRef> struct_column_ref =
        MakeResolvedColumnRef(struct_column_.type(), struct_column_,
                              /*is_correlated=*/false);
    ZETASQL_RET_CHECK(struct_column_ref->type()->IsStruct());
    ZETASQL_RET_CHECK(struct_column_ref->type()->AsStruct()->num_fields() == 2);
    const StructField& key_columns_field =
        struct_column_ref->type()->AsStruct()->field(kKeyColumnsFieldIndex);
    std::unique_ptr<ResolvedGetStructField> get_struct_field_expr =
        MakeResolvedGetStructField(key_columns_field.type,
                                   std::move(struct_column_ref),
                                   kKeyColumnsFieldIndex);
    group_by_list.push_back(MakeResolvedComputedColumn(
        column_factory_.MakeCol("$groupbymod", "grain_lock_key",
                                key_columns_field.type),
        std::move(get_struct_field_expr)));

    // Step 4: Set the `group_by_aggregate_list`, `group_by_list` and
    // `argument_list` on the rewritten aggregate function call.
    aggregate_function_call_builder.set_argument_list(
        std::move(rewritten_argument_list));
    aggregate_function_call_builder.set_group_by_aggregate_list(
        std::move(group_by_aggregate_list));
    aggregate_function_call_builder.set_group_by_list(std::move(group_by_list));

    // Step 5: Push the rewritten aggregate function call into
    // `computed_aggregate_list_`, and return a column reference to it.
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedAggregateFunctionCall>
                         rewritten_aggregate_function,
                     std::move(aggregate_function_call_builder).Build());
    return ProcessAggregateFunction(std::move(rewritten_aggregate_function),
                                    aggregate_function_call_ptr);
  }

 private:
  bool IsTopLevelAggregateFunctionCall(
      const ResolvedAggregateFunctionCall* aggregate_function_call) const {
    return top_level_aggregate_function_call_ == aggregate_function_call;
  }

  std::unique_ptr<const ResolvedNode> ProcessAggregateFunction(
      std::unique_ptr<const ResolvedAggregateFunctionCall> aggregate_function,
      const ResolvedAggregateFunctionCall*
          original_aggregate_function_call_ptr) {
    if (!IsTopLevelAggregateFunctionCall(
            original_aggregate_function_call_ptr)) {
      return aggregate_function;
    }
    ResolvedColumn aggregate_function_column = column_factory_.MakeCol(
        "$aggregate",
        absl::StrCat("constituent_aggregate_", constituent_aggregate_count_++),
        aggregate_function->type());
    constituent_aggregate_list_.push_back(MakeResolvedComputedColumn(
        aggregate_function_column, std::move(aggregate_function)));
    return MakeResolvedColumnRef(
        constituent_aggregate_list_.back()->column().type(),
        constituent_aggregate_list_.back()->column(), /*is_correlated=*/false);
  }

  // A pointer to the `ANY_VALUE` function in the catalog used for the rewrite.
  const Function* any_value_fn_ = nullptr;
  // Used to create new columns.
  ColumnFactory& column_factory_;
  // The special STRUCT-typed column that contains the grouping keys needed for
  // grain-locking.
  ResolvedColumn struct_column_;
  // A list of (rewritten) constituent aggregates that compose a measure
  // expression.
  std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>
      constituent_aggregate_list_;
  // Used purely for naming constituent aggregate columns.
  uint64_t constituent_aggregate_count_ = 0;
  // Tracks the current top-level aggregate function call (e.g. track `SUM`
  // while traversing the expression `SUM(AVG(X) + MIN(Y) GROUP BY Z)`).
  // Only top-level aggregate function calls need to pushed into
  // `constituent_aggregate_list_`.
  const ResolvedAggregateFunctionCall* top_level_aggregate_function_call_ =
      nullptr;
};

// `StructColumnReferenceRewriter` rewrites a measure expression to reference
// columns from the STRUCT-typed column used to replace the measure column.
class StructColumnReferenceRewriter : public ResolvedASTRewriteVisitor {
 public:
  static absl::StatusOr<std::unique_ptr<const ResolvedExpr>>
  RewriteMeasureExpression(const ResolvedExpr* measure_expr,
                           ResolvedColumn struct_column) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedExpr> measure_expr_copy,
                     ResolvedASTDeepCopyVisitor::Copy(measure_expr));
    StructColumnReferenceRewriter rewriter(struct_column);

    // Rewrite the measure expression to reference columns from
    // `struct_column`.
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedNode> rewritten_measure_expr,
                     rewriter.VisitAll(std::move(measure_expr_copy)));
    ZETASQL_RET_CHECK(rewritten_measure_expr->Is<ResolvedExpr>());

    return absl::WrapUnique(
        rewritten_measure_expr.release()->GetAs<ResolvedExpr>());
  }

 protected:
  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedExpressionColumn(
      std::unique_ptr<const ResolvedExpressionColumn> node) override {
    // struct_column_ref = ColumnRef(
    //   type=STRUCT<STRUCT<referenced_columns>, STRUCT<key_columns>>
    // )
    std::unique_ptr<ResolvedColumnRef> struct_column_ref =
        MakeResolvedColumnRef(struct_column_.type(), struct_column_,
                              /*is_correlated=*/false);
    // +-GetStructField
    //  +-type=STRUCT<referenced_columns>
    //  +-expr=
    //  | +-<struct_column_ref>
    //  +-field_idx=0
    ZETASQL_RET_CHECK_EQ(struct_column_ref->type()->AsStruct()->num_fields(), 2);
    const StructField& referenced_columns_field =
        struct_column_ref->type()->AsStruct()->field(
            kReferencedColumnsFieldIndex);
    std::unique_ptr<ResolvedGetStructField> get_struct_field_expr =
        MakeResolvedGetStructField(referenced_columns_field.type,
                                   std::move(struct_column_ref),
                                   kReferencedColumnsFieldIndex);

    // +-GetStructField
    //  +-type=<output_type>
    //  +-expr=
    //  | +-GetStructField
    //  |  +-type=STRUCT<referenced_columns>
    //  |  +-expr=
    //  |  | +-<struct_column_ref>
    //  |  +-field_idx=0
    //  +-field_idx=<field_index>
    bool is_ambiguous = false;
    int field_index = -1;
    const StructField* field =
        get_struct_field_expr->type()->AsStruct()->FindField(
            node->name(), &is_ambiguous, &field_index);
    ZETASQL_RET_CHECK(field != nullptr);
    ZETASQL_RET_CHECK(!is_ambiguous);
    ZETASQL_RET_CHECK(field_index >= 0);
    return MakeResolvedGetStructField(
        field->type, std::move(get_struct_field_expr), field_index);
  }

 private:
  explicit StructColumnReferenceRewriter(ResolvedColumn struct_column)
      : struct_column_(struct_column) {}
  StructColumnReferenceRewriter(const StructColumnReferenceRewriter&) = delete;
  StructColumnReferenceRewriter& operator=(
      const StructColumnReferenceRewriter&) = delete;

  ResolvedColumn struct_column_;
};

// `MeasuresAggregateFunctionReplacer` replaces `AGGREGATE` function calls with
// rewritten measure expression information from `MeasureExpansionInfo`.
class MeasuresAggregateFunctionReplacer : public ResolvedASTRewriteVisitor {
 public:
  static absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  ReplaceMeasureAggregateFunctions(
      std::unique_ptr<const ResolvedNode> input, const Function* any_value_fn,
      ColumnFactory& column_factory,
      MeasureExpansionInfoMap& measure_expansion_info_map) {
    return MeasuresAggregateFunctionReplacer(any_value_fn, column_factory,
                                             measure_expansion_info_map)
        .VisitAll(std::move(input));
  }

 protected:
  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedAggregateScan(
      std::unique_ptr<const ResolvedAggregateScan> node) override {
    // First, rewrite the AggregateScan to compute constituent aggregates the
    // measure expands to.
    // TODO: b/350555383 - Verify the logic here; are the ResolvedColumn types
    // correct?
    std::vector<ResolvedColumn> columns_to_project = node->column_list();
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        computed_columns_to_project;
    ZETASQL_ASSIGN_OR_RETURN(node, RewriteAggregateScan(std::move(node),
                                                &computed_columns_to_project));
    // Then, layer a ProjectScan to project the computed columns (along with any
    // columns that were already projected by the original AggregateScan)
    return MakeResolvedProjectScan(std::move(columns_to_project),
                                   std::move(computed_columns_to_project),
                                   std::move(node));
  }

 private:
  explicit MeasuresAggregateFunctionReplacer(
      const Function* any_value_fn, ColumnFactory& column_factory,
      MeasureExpansionInfoMap& measure_expansion_info_map)
      : any_value_fn_(any_value_fn),
        column_factory_(column_factory),
        measure_expansion_info_map_(measure_expansion_info_map) {};
  MeasuresAggregateFunctionReplacer(const MeasuresAggregateFunctionReplacer&) =
      delete;
  MeasuresAggregateFunctionReplacer& operator=(
      const MeasuresAggregateFunctionReplacer&) = delete;

  absl::StatusOr<std::unique_ptr<const ResolvedAggregateScan>>
  RewriteAggregateScan(
      std::unique_ptr<const ResolvedAggregateScan> node,
      std::vector<std::unique_ptr<const ResolvedComputedColumn>>*
          computed_columns_to_project) {
    // We'll need to rewrite the `aggregate_list` and `column_list` of the
    // AggregateScan.
    std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>
        rewritten_aggregate_list;
    std::vector<ResolvedColumn> rewritten_column_list = node->column_list();
    auto aggregate_scan_builder = ToBuilder(std::move(node));
    for (std::unique_ptr<const ResolvedComputedColumnBase>& aggregate_column :
         aggregate_scan_builder.release_aggregate_list()) {
      if (IsMeasureAggFunction(aggregate_column->expr())) {
        ZETASQL_ASSIGN_OR_RETURN(ResolvedColumn invoked_measure_column,
                         GetInvokedMeasureColumn(
                             aggregate_column->expr()
                                 ->GetAs<ResolvedAggregateFunctionCall>()));
        ZETASQL_RET_CHECK(measure_expansion_info_map_.contains(invoked_measure_column));
        MeasureExpansionInfo& measure_expansion_info =
            measure_expansion_info_map_[invoked_measure_column];
        ZETASQL_RET_CHECK(measure_expansion_info.measure_expr != nullptr);

        // Remap column ids in the measure expression to use new column ids
        // allocated by `column_factory_`. Since the measure expression was
        // analyzed in a different context, it's column ids will be invalid in
        // the current query.
        ColumnReplacementMap column_replacement_map;
        ZETASQL_ASSIGN_OR_RETURN(
            std::unique_ptr<const ResolvedExpr> rewritten_measure_expr,
            CopyResolvedASTAndRemapColumns(*measure_expansion_info.measure_expr,
                                           column_factory_,
                                           column_replacement_map));

        // Rewrite the measure expression to reference columns from
        // `struct_column`.
        ZETASQL_ASSIGN_OR_RETURN(
            rewritten_measure_expr,
            StructColumnReferenceRewriter::RewriteMeasureExpression(
                rewritten_measure_expr.get(),
                measure_expansion_info.struct_column));

        // Rewrite the measure expression to use multi-level aggregation to
        // grain-lock and avoid overcounting.
        MultiLevelAggregateRewriter multi_level_aggregate_rewriter(
            any_value_fn_, column_factory_,
            measure_expansion_info.struct_column);
        ZETASQL_ASSIGN_OR_RETURN(
            rewritten_measure_expr,
            multi_level_aggregate_rewriter.RewriteMultiLevelAggregate(
                std::move(rewritten_measure_expr)));

        for (auto& computed_aggregates :
             multi_level_aggregate_rewriter
                 .release_constituent_aggregate_list()) {
          rewritten_column_list.push_back(computed_aggregates->column());
          rewritten_aggregate_list.push_back(std::move(computed_aggregates));
        }

        computed_columns_to_project->push_back(MakeResolvedComputedColumn(
            aggregate_column->column(), std::move(rewritten_measure_expr)));
        // Remove the aggregate column from the `rewritten_column_list`, since
        // that column id is now used in `computed_columns_to_project`.
        rewritten_column_list.erase(std::remove(rewritten_column_list.begin(),
                                                rewritten_column_list.end(),
                                                aggregate_column->column()),
                                    rewritten_column_list.end());
        continue;
      }
      rewritten_aggregate_list.push_back(std::move(aggregate_column));
    }
    aggregate_scan_builder
        .set_aggregate_list(std::move(rewritten_aggregate_list))
        .set_column_list(std::move(rewritten_column_list));
    return std::move(aggregate_scan_builder).Build();
  }

  const Function* any_value_fn_ = nullptr;
  ColumnFactory& column_factory_;
  MeasureExpansionInfoMap& measure_expansion_info_map_;
};

// `MeasureColumnReplacer` replaces measure columns with their corresponding
// `STRUCT` typed columns found in `MeasureExpansionInfo`.
class MeasureColumnReplacer : public ResolvedASTRewriteVisitor {
 public:
  static absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  ReplaceMeasureColumns(std::unique_ptr<const ResolvedNode> input,
                        MeasureExpansionInfoMap& measure_expansion_info_map) {
    return MeasureColumnReplacer(measure_expansion_info_map)
        .VisitAll(std::move(input));
  }

 protected:
  absl::StatusOr<ResolvedColumn> PostVisitResolvedColumn(
      const ResolvedColumn& column) override {
    if (!column.type()->IsMeasureType()) {
      return column;
    }
    if (measure_expansion_info_map_.contains(column)) {
      MeasureExpansionInfo& measure_expansion_info =
          measure_expansion_info_map_[column];
      ZETASQL_RET_CHECK(measure_expansion_info.struct_column.IsInitialized());
      ZETASQL_RET_CHECK(measure_expansion_info.struct_column.type()->IsStruct());
      return measure_expansion_info.struct_column;
    }
    return column;
  }

 private:
  explicit MeasureColumnReplacer(
      MeasureExpansionInfoMap& measure_expansion_info_map)
      : measure_expansion_info_map_(measure_expansion_info_map) {};

  MeasureColumnReplacer(const MeasureColumnReplacer&) = delete;
  MeasureColumnReplacer& operator=(const MeasureColumnReplacer&) = delete;

  MeasureExpansionInfoMap& measure_expansion_info_map_;
};

absl::StatusOr<std::unique_ptr<const ResolvedNode>> RewriteMeasures(
    std::unique_ptr<const ResolvedNode> input,
    absl::flat_hash_map<const ResolvedTableScan*, GrainScanInfo>
        grain_scan_info_map,
    const Function* any_value_fn, ColumnFactory& column_factory,
    MeasureExpansionInfoMap& measure_expansion_info_map) {
  // Rewrite grain scans to project any additional columns and layer a
  // ProjectScan over them to compute the `STRUCT` typed columns needed for
  // measure expansion.
  ZETASQL_ASSIGN_OR_RETURN(input,
                   GrainScanRewriter::RewriteGrainScans(
                       std::move(input), std::move(grain_scan_info_map)));

  // Replace measure columns with their corresponding `STRUCT` typed columns.
  ZETASQL_ASSIGN_OR_RETURN(input, MeasureColumnReplacer::ReplaceMeasureColumns(
                              std::move(input), measure_expansion_info_map));

  // Replace `AGGREGATE` function calls with rewritten measure expressions.
  return MeasuresAggregateFunctionReplacer::ReplaceMeasureAggregateFunctions(
      std::move(input), any_value_fn, column_factory,
      measure_expansion_info_map);
}

}  // namespace zetasql
