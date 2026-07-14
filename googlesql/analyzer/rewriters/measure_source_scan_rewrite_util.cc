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

#include "googlesql/analyzer/rewriters/measure_source_scan_rewrite_util.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/analyzer/rewriters/measure_collector.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/types/measure_type.h"
#include "googlesql/public/types/struct_type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/column_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_builder.h"
#include "googlesql/resolved_ast/resolved_ast_rewrite_visitor.h"
#include "googlesql/resolved_ast/resolved_ast_visitor.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/base/case.h"
#include "absl/algorithm/container.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "googlesql/base/check.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {

using NameToResolvedColumn =
    absl::flat_hash_map<std::string, ResolvedColumn,
                        googlesql_base::StringViewCaseHash,
                        googlesql_base::StringViewCaseEqual>;
using CaseInsensitiveStringSet =
    absl::flat_hash_set<std::string, googlesql_base::StringViewCaseHash,
                        googlesql_base::StringViewCaseEqual>;

static constexpr char kReferencedColumnsFieldName[] = "referenced_columns";
static constexpr char kKeyColumnsFieldName[] = "key_columns";

class ExpressionColumnNameCollector : public ResolvedASTVisitor {
 public:
  static absl::StatusOr<CaseInsensitiveStringSet> GetExpressionColumnNames(
      const ResolvedExpr* expr) {
    ExpressionColumnNameCollector collector;
    GOOGLESQL_RETURN_IF_ERROR(expr->Accept(&collector));
    return collector.column_names_;
  }

  absl::Status VisitResolvedExpressionColumn(
      const ResolvedExpressionColumn* node) override {
    column_names_.insert(node->name());
    return absl::OkStatus();
  }

 private:
  CaseInsensitiveStringSet column_names_;
};

absl::StatusOr<CaseInsensitiveStringSet> GetExpressionColumnNames(
    const ResolvedExpr* expr) {
  return ExpressionColumnNameCollector::GetExpressionColumnNames(expr);
}

absl::StatusOr<std::vector<int>> GetRowIdentityColumnIndices(
    const Column* column, const Table* table) {
  GOOGLESQL_RET_CHECK(column->GetExpression().has_value());
  if (std::optional<std::vector<int>> column_level_row_identity_columns =
          column->GetExpression()->RowIdentityColumns();
      column_level_row_identity_columns.has_value()) {
    return *column_level_row_identity_columns;
  }
  return table->RowIdentityColumns().value_or(std::vector<int>{});
}

absl::StatusOr<const StructType*> BuildClosureType(const Column* measure_column,
                                                   const Table* table,
                                                   TypeFactory& type_factory) {
  GOOGLESQL_RET_CHECK(measure_column->HasMeasureExpression() &&
            measure_column->GetExpression()->HasResolvedExpression());
  const ResolvedExpr* measure_expr =
      measure_column->GetExpression()->GetResolvedExpression();

  GOOGLESQL_ASSIGN_OR_RETURN(CaseInsensitiveStringSet referenced_column_names,
                   GetExpressionColumnNames(measure_expr));

  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<int> row_identity_column_indices,
                   GetRowIdentityColumnIndices(measure_column, table));
  GOOGLESQL_RET_CHECK(!row_identity_column_indices.empty());
  absl::c_sort(row_identity_column_indices);

  // Build referenced_columns struct type
  std::vector<StructType::StructField> ref_fields;
  for (int table_col_idx = 0; table_col_idx < table->NumColumns();
       ++table_col_idx) {
    const Column* column = table->GetColumn(table_col_idx);
    if (referenced_column_names.contains(column->Name())) {
      ref_fields.push_back(
          StructType::StructField(column->Name(), column->GetType()));
    }
  }
  const StructType* ref_struct_type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(type_factory.MakeStructType(ref_fields, &ref_struct_type));

  // Build key_columns struct type
  std::vector<StructType::StructField> key_fields;
  key_fields.reserve(row_identity_column_indices.size());
  for (int row_id_col_idx : row_identity_column_indices) {
    const Column* column = table->GetColumn(row_id_col_idx);
    key_fields.push_back(
        StructType::StructField(column->Name(), column->GetType()));
  }
  const StructType* key_struct_type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(type_factory.MakeStructType(key_fields, &key_struct_type));

  // Build wrapping struct type
  std::vector<StructType::StructField> wrapping_fields = {
      {kReferencedColumnsFieldName, ref_struct_type},
      {kKeyColumnsFieldName, key_struct_type}};
  const StructType* wrapping_struct_type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(
      type_factory.MakeStructType(wrapping_fields, &wrapping_struct_type));

  return wrapping_struct_type;
}

// Provides scan-type-specific information for measure source scans.
template <typename ScanType>
struct MeasureSourceTraits {};

template <>
struct MeasureSourceTraits<ResolvedTableScan> {
  static const Table* GetTable(const ResolvedTableScan* scan) {
    return scan->table();
  }
};

template <>
struct MeasureSourceTraits<ResolvedTVFScan> {
  static const Table* GetTable(const ResolvedTVFScan* scan) {
    return scan->signature()->result_table_schema();
  }
};



// Holds information about a measure source column.
struct MeasureSourceInfo {
  // The measure column from table scan's column_list.
  ResolvedColumn measure_col;

  // The measure expression of `measure_col` from catalog.
  const ResolvedExpr* measure_expr;

  // The set of column names referenced in `measure_expr`.
  CaseInsensitiveStringSet referenced_column_names;

  // The row identity column indices for this measure source column.
  std::vector<int> row_identity_column_indices;
};

static absl::StatusOr<int> FindColumnIndex(const Table* table,
                                           absl::string_view name) {
  const Column* target_col = table->FindColumnByName(std::string(name));
  GOOGLESQL_RET_CHECK(target_col != nullptr) << "Cannot find column: " << name;
  for (int i = 0; i < table->NumColumns(); ++i) {
    if (table->GetColumn(i) == target_col) {
      return i;
    }
  }
  GOOGLESQL_RET_CHECK_FAIL() << "Column " << name << " not found in table columns";
}

// Builds the closure struct type for all measure columns on the scan.
// The closure struct type has two fields:
// 1. `referenced_columns`: A STRUCT containing all columns referenced by
//    measure expressions on the scan.
// 2. `key_columns`: A STRUCT containing row identity columns of the table.
static absl::StatusOr<const StructType*> BuildSharedClosureType(
    absl::Span<const MeasureSourceInfo> measure_infos, const Table* table,
    TypeFactory& type_factory) {
  absl::btree_set<std::string, googlesql_base::CaseLess>
      all_referenced_column_names;
  absl::btree_set<int> all_row_identity_column_indices;

  for (const auto& info : measure_infos) {
    all_referenced_column_names.insert(info.referenced_column_names.begin(),
                                       info.referenced_column_names.end());
    all_row_identity_column_indices.insert(
        info.row_identity_column_indices.begin(),
        info.row_identity_column_indices.end());
  }

  // Build referenced_columns struct type
  std::vector<StructType::StructField> ref_fields;
  for (int table_col_idx = 0; table_col_idx < table->NumColumns();
       ++table_col_idx) {
    const Column* column = table->GetColumn(table_col_idx);
    if (all_referenced_column_names.contains(column->Name())) {
      ref_fields.push_back(
          StructType::StructField(column->Name(), column->GetType()));
    }
  }
  const StructType* ref_struct_type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(type_factory.MakeStructType(ref_fields, &ref_struct_type));

  // Build key_columns struct type
  std::vector<StructType::StructField> key_fields;
  key_fields.reserve(all_row_identity_column_indices.size());
  for (int row_id_col_idx : all_row_identity_column_indices) {
    const Column* column = table->GetColumn(row_id_col_idx);
    key_fields.push_back(
        StructType::StructField(column->Name(), column->GetType()));
  }
  const StructType* key_struct_type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(type_factory.MakeStructType(key_fields, &key_struct_type));

  // Build wrapping struct type
  std::vector<StructType::StructField> wrapping_fields = {
      {kReferencedColumnsFieldName, ref_struct_type},
      {kKeyColumnsFieldName, key_struct_type}};
  const StructType* wrapping_struct_type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(
      type_factory.MakeStructType(wrapping_fields, &wrapping_struct_type));

  return wrapping_struct_type;
}

// Rewrites a ResolvedTableScan or ResolvedTVFScan if it contains AGG'ed
// measure source columns.
//
// If measure columns are present on the scan, this class:
// 1. Builds a closure column, which is a STRUCT containing:
//    - referenced_columns: a STRUCT of columns referenced by any measure
//      expression on the scan.
//    - key_columns: a STRUCT of row identity columns of the table.
// 2. Creates a ProjectScan on top of the input scan to project this closure
//    column.
// 3. Removes measure columns that have measure expressions from scan's column
//    list, and adds any columns referenced by measure expressions but not
//    present in scan's column list to the scan.
// 4. Registers measure definitions with `measure_collector_` for later rewrite
//    of ResolvedMeasureReference.
template <typename ScanType>
class MeasureSourceColumnReplacer {
 public:
  MeasureSourceColumnReplacer(std::unique_ptr<const ScanType> scan,
                              MeasureCollector& measure_collector,
                              TypeFactory& type_factory,
                              ColumnFactory& column_factory)
      : scan_(std::move(scan)),
        measure_collector_(measure_collector),
        type_factory_(type_factory),
        column_factory_(column_factory) {}

  absl::StatusOr<std::unique_ptr<const ResolvedNode>> Replace() {
    // Step 1: Collect measure column information.
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<MeasureSourceInfo> measure_infos,
                     CollectMeasureInfos());
    if (measure_infos.empty()) {
      // No measure definitions are found, nothing to rewrite.
      return std::move(scan_);
    }
    // Step 2: Build the closure struct.
    NameToResolvedColumn missing_columns_from_scan;
    absl::flat_hash_set<ResolvedColumn> measure_cols_with_expr_set;
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<ResolvedComputedColumn> closure,
        BuildClosureColumn(measure_infos, missing_columns_from_scan,
                           measure_cols_with_expr_set));
    // Step 3: Store the relevant information for each measure definition.
    const Table* table = GetTable();
    for (const auto& info : measure_infos) {
      GOOGLESQL_RET_CHECK(info.measure_col.type()->IsMeasureType());
      absl::btree_set<std::string, googlesql_base::CaseLess>
          row_identity_column_names;
      for (const int index : info.row_identity_column_indices) {
        const std::string column_name = table->GetColumn(index)->Name();
        GOOGLESQL_RET_CHECK(row_identity_column_names.insert(column_name).second)
            << "Duplicate row identity column name: " << column_name;
      }
      GOOGLESQL_RETURN_IF_ERROR(measure_collector_.AddMeasureInfo(
          info.measure_col.type()->AsMeasure(),
          {.measure_expr = info.measure_expr,
           .row_identity_column_names = std::move(row_identity_column_names),
           .closure_struct_type = closure->column().type(),
           .closure_column = MeasureInfo::ClosureColumn{
               .closure_struct = closure->column(),
               .measure_source_column = info.measure_col}}));
    }

    // Step 4: Add a ProjectScan to project the closure column.
    return RebuildScanAndCreateProjectScan(measure_cols_with_expr_set,
                                           missing_columns_from_scan,
                                           std::move(closure));
  }

 private:
  const Table* GetTable() const {
    const Table* table = MeasureSourceTraits<ScanType>::GetTable(scan_.get());
    ABSL_DCHECK(table != nullptr);
    return table;
  }

  absl::StatusOr<std::vector<MeasureSourceInfo>> CollectMeasureInfos() {
    std::vector<MeasureSourceInfo> measure_infos;
    for (int i = 0; i < scan_->column_list_size(); ++i) {
      const ResolvedColumn& col = scan_->column_list(i);
      if (!col.type()->IsMeasureType()) {
        continue;
      }
      if (!measure_collector_.IsAgged(col.type()->AsMeasure())) {
        continue;
      }
      const int col_idx_in_table = scan_->column_index_list(i);
      const Table* table = GetTable();
      const Column* catalog_column = table->GetColumn(col_idx_in_table);
      GOOGLESQL_RET_CHECK(catalog_column->HasMeasureExpression() &&
                catalog_column->GetExpression()->HasResolvedExpression());

      const ResolvedExpr* measure_expr =
          catalog_column->GetExpression()->GetResolvedExpression();

      GOOGLESQL_ASSIGN_OR_RETURN(CaseInsensitiveStringSet referenced_column_names,
                       GetExpressionColumnNames(measure_expr));

      GOOGLESQL_ASSIGN_OR_RETURN(std::vector<int> row_identity_column_indices,
                       GetRowIdentityColumnIndices(catalog_column, table));
      GOOGLESQL_RET_CHECK(!row_identity_column_indices.empty());
      absl::c_sort(row_identity_column_indices);

      measure_infos.push_back({
          .measure_col = col,
          .measure_expr = measure_expr,
          .referenced_column_names = referenced_column_names,
          .row_identity_column_indices = row_identity_column_indices,
      });
    }
    return measure_infos;
  }

  // Builds and returns a ResolvedComputedColumn representing the closure
  // for all measure columns on the scan. The expression is a struct that
  // contains all columns referenced by measures defined in `measure_infos`, as
  // well as row identity columns, i.e.,
  //
  // STRUCT(
  //   referenced_columns: STRUCT(
  //     <column_name>: ResolvedColumn,
  //     ...
  //   ),
  //   key_columns: STRUCT(
  //     <column index>: ResolvedColumn,
  //     ...
  //   )
  // )
  //
  // Populates `missing_columns_from_scan` with columns that are needed for
  // building the closure but are not present in `scan_`. Populates
  // `measure_cols_with_expr_set` with measure columns that have measure
  // expressions.
  absl::StatusOr<std::unique_ptr<ResolvedComputedColumn>> BuildClosureColumn(
      absl::Span<const MeasureSourceInfo> measure_infos,
      NameToResolvedColumn& missing_columns_from_scan,
      absl::flat_hash_set<ResolvedColumn>& measure_cols_with_expr_set) {
    const Table* table = GetTable();

    for (const auto& info : measure_infos) {
      measure_cols_with_expr_set.insert(info.measure_col);
    }

    GOOGLESQL_ASSIGN_OR_RETURN(
        const StructType* closure_struct_type,
        BuildSharedClosureType(measure_infos, table, type_factory_));

    // Validation on the computed closure struct type.
    GOOGLESQL_RET_CHECK(closure_struct_type->num_fields() == 2);
    GOOGLESQL_RET_CHECK(closure_struct_type->field(0).name ==
              kReferencedColumnsFieldName);
    GOOGLESQL_RET_CHECK(closure_struct_type->field(0).type->IsStruct());
    GOOGLESQL_RET_CHECK(closure_struct_type->field(1).name == kKeyColumnsFieldName);
    GOOGLESQL_RET_CHECK(closure_struct_type->field(1).type->IsStruct());

    // 1. Build referenced_columns struct expression
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedMakeStruct> ref_struct_expr,
                     BuildStructExprFromFields(
                         closure_struct_type->field(0).type->AsStruct(),
                         missing_columns_from_scan));

    // 2. Build key_columns struct expression
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedMakeStruct> key_struct_expr,
                     BuildStructExprFromFields(
                         closure_struct_type->field(1).type->AsStruct(),
                         missing_columns_from_scan));

    // 3. Build wrapping struct expression
    std::vector<std::unique_ptr<const ResolvedExpr>> wrapping_exprs;
    wrapping_exprs.reserve(2);
    wrapping_exprs.push_back(std::move(ref_struct_expr));
    wrapping_exprs.push_back(std::move(key_struct_expr));
    auto closure_expr =
        MakeResolvedMakeStruct(closure_struct_type, std::move(wrapping_exprs));

    const std::string closure_column_name =
        absl::StrCat("struct_for_measures_from_table_", table->Name());
    ResolvedColumn closure_column = column_factory_.MakeCol(
        table->Name(), closure_column_name, closure_expr->type());
    return MakeResolvedComputedColumn(closure_column, std::move(closure_expr));
  }

  // Builds a struct expression that projects the columns specified by the
  // fields of `target_struct_type`.
  absl::StatusOr<std::unique_ptr<ResolvedMakeStruct>> BuildStructExprFromFields(
      const StructType* target_struct_type,
      NameToResolvedColumn& missing_columns_from_scan) {
    const Table* table = GetTable();
    std::vector<std::unique_ptr<const ResolvedExpr>> exprs;
    exprs.reserve(target_struct_type->num_fields());
    for (int i = 0; i < target_struct_type->num_fields(); ++i) {
      const StructType::StructField& field = target_struct_type->field(i);
      GOOGLESQL_ASSIGN_OR_RETURN(const int table_col_idx,
                       FindColumnIndex(table, field.name));
      ResolvedColumn col =
          GetOrProjectColumn(table_col_idx, missing_columns_from_scan);
      exprs.push_back(MakeResolvedColumnRef(field.type, col,
                                            /*is_correlated=*/false));
    }
    return MakeResolvedMakeStruct(target_struct_type, std::move(exprs));
  }

  // Rebuilds `scan_` to remove measure columns and add columns in
  // `missing_columns_from_scan`. Then, creates a ProjectScan on top of
  // `scan_` which adds `closure` to the column list.
  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  RebuildScanAndCreateProjectScan(
      const absl::flat_hash_set<ResolvedColumn>& measure_cols_with_expr_set,
      const NameToResolvedColumn& missing_columns_from_scan,
      std::unique_ptr<ResolvedComputedColumn> closure) {
    // Step 1: Remove the AGG'ed measure columns from the source_scan - they
    // are replaced by the struct closure column on this scan.
    std::vector<std::pair<int, ResolvedColumn>> indexed_columns;
    ResolvedColumnList project_column_list;

    for (int i = 0; i < scan_->column_list_size(); ++i) {
      if (!measure_cols_with_expr_set.contains(scan_->column_list(i))) {
        indexed_columns.push_back(
            {scan_->column_index_list(i), scan_->column_list(i)});
        project_column_list.push_back(scan_->column_list(i));
      }
    }

    // Step 2: Add columns in `missing_columns_from_scan` to the
    // source_scan. These columns are referenced in measure expressions but
    // not in scan_'s column list.
    const Table* table = GetTable();
    for (int i = 0; i < table->NumColumns(); ++i) {
      const Column* column = table->GetColumn(i);
      if (missing_columns_from_scan.contains(column->Name())) {
        indexed_columns.push_back(
            {i, missing_columns_from_scan.at(column->Name())});
      }
    }

    GOOGLESQL_RETURN_IF_ERROR(RebuildScanColumns(indexed_columns));

    // Step 3: Build ProjectScan to add `closure`.
    project_column_list.push_back(closure->column());
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> project_exprs;
    project_exprs.push_back(std::move(closure));

    return MakeResolvedProjectScan(project_column_list,
                                   std::move(project_exprs), std::move(scan_));
  }

  // Gets the ResolvedColumn corresponding to `table_col_idx`. If it is not
  // present in `scan_->column_list()`, creates a new ResolvedColumn for it
  // and adds it to scan_->column_list() first.
  ResolvedColumn GetOrProjectColumn(
      int table_col_idx, NameToResolvedColumn& missing_columns_from_scan) {
    for (int i = 0; i < scan_->column_index_list_size(); ++i) {
      if (scan_->column_index_list(i) == table_col_idx) {
        return scan_->column_list(i);
      }
    }
    const Table* table = GetTable();
    const Column* column = table->GetColumn(table_col_idx);
    if (missing_columns_from_scan.contains(column->Name())) {
      return missing_columns_from_scan.at(column->Name());
    }
    ResolvedColumn new_col = column_factory_.MakeCol(
        table->Name(), column->Name(), column->GetType());
    missing_columns_from_scan[column->Name()] = new_col;
    return new_col;
  }

  // Rebuilds `column_list` and `column_index_list` of `scan_` with columns and
  // indices in `indexed_columns`.
  absl::Status RebuildScanColumns(
      std::vector<std::pair<int, ResolvedColumn>>& indexed_columns) {
    std::sort(indexed_columns.begin(), indexed_columns.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    ResolvedColumnList new_column_list;
    std::vector<int> new_column_index_list;
    new_column_list.reserve(indexed_columns.size());
    new_column_index_list.reserve(indexed_columns.size());
    for (const auto& [index, column] : indexed_columns) {
      new_column_index_list.push_back(index);
      new_column_list.push_back(column);
    }

    GOOGLESQL_ASSIGN_OR_RETURN(scan_, ToBuilder(std::move(scan_))
                                .set_column_list(new_column_list)
                                .set_column_index_list(new_column_index_list)
                                .Build());
    return absl::OkStatus();
  }

  std::unique_ptr<const ScanType> scan_;
  MeasureCollector& measure_collector_;
  TypeFactory& type_factory_;
  ColumnFactory& column_factory_;
};

// Collects measure sources and computes the corresponding closure struct
// types.
//
// If the measure source is a TableScan or TVFScan, it also creates a closure
// column and adds it to the source scan by adding a ProjectScan on top of it.
class MeasureSourceCollector : public ResolvedASTRewriteVisitor {
 public:
  MeasureSourceCollector(MeasureCollector& measure_collector,
                         TypeFactory& type_factory,
                         ColumnFactory& column_factory)
      : measure_collector_(measure_collector),
        type_factory_(type_factory),
        column_factory_(column_factory) {}

 protected:
  // Row field access of a measure-typed column is a source of a measure.
  //
  // Here we only collect the measure info and defer replacing the measure
  // source with the closure struct to `MeasureColumnRewriter` to avoid having
  // type inconsistencies in the Resolved AST, e.g., between a
  // `ResolvedGetRowField` and the `ResolvedComputedColumn` that contains
  // it.
  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedGetRowField(
      std::unique_ptr<const ResolvedGetRowField> node) override {
    if (!node->type()->IsMeasureType()) {
      return node;
    }

    const MeasureType* measure_type = node->type()->AsMeasure();
    if (!measure_collector_.IsAgged(measure_type)) {
      return node;
    }

    {
      absl::Status status =
          measure_collector_.GetMeasureInfo(measure_type).status();
      if (status.ok()) {
        // Already registered, skip the collection. This can happen because
        // the type of a `ResolvedGetRowField` comes from catalog
        // `Column::type()`.
        return node;
      }
      GOOGLESQL_RET_CHECK(absl::IsNotFound(status))
          << "Unexpected error getting measure info for measure type: "
          << measure_type->DebugString() << " error: " << status;
    }

    const Column* measure_column = node->column();
    const Table* table = node->expr()->type()->AsRowType()->table();
    // We currently only support measure columns on tables with DEFAULT column
    // list mode, i.e., tables that have a column list.
    GOOGLESQL_RET_CHECK(table->HasColumnList());
    GOOGLESQL_ASSIGN_OR_RETURN(const StructType* closure_struct_type,
                     BuildClosureType(measure_column, table, type_factory_));

    const ResolvedExpr* measure_expr =
        measure_column->GetExpression()->GetResolvedExpression();

    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<int> row_identity_column_indices,
                     GetRowIdentityColumnIndices(measure_column, table));
    absl::btree_set<std::string, googlesql_base::CaseLess>
        row_identity_column_names;
    for (const int index : row_identity_column_indices) {
      row_identity_column_names.insert(table->GetColumn(index)->Name());
    }

    GOOGLESQL_RETURN_IF_ERROR(measure_collector_.AddMeasureInfo(
        measure_type,
        {
            .measure_expr = measure_expr,
            .row_identity_column_names = std::move(row_identity_column_names),
            .closure_struct_type = closure_struct_type,
        }));
    return node;
  }

  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedTableScan(
      std::unique_ptr<const ResolvedTableScan> scan) override {
    return MeasureSourceColumnReplacer<ResolvedTableScan>(
               std::move(scan), measure_collector_, type_factory_,
               column_factory_)
        .Replace();
  }

  absl::StatusOr<std::unique_ptr<const ResolvedNode>> PostVisitResolvedTVFScan(
      std::unique_ptr<const ResolvedTVFScan> scan) override {
    return MeasureSourceColumnReplacer<ResolvedTVFScan>(
               std::move(scan), measure_collector_, type_factory_,
               column_factory_)
        .Replace();
  }

 private:
  MeasureCollector& measure_collector_;
  TypeFactory& type_factory_;
  ColumnFactory& column_factory_;
};

absl::StatusOr<std::unique_ptr<const ResolvedNode>> AddClosures(
    MeasureCollector& measure_collector,
    std::unique_ptr<const ResolvedNode> resolved_ast, TypeFactory& type_factory,
    ColumnFactory& column_factory) {
  MeasureSourceCollector visitor(measure_collector, type_factory,
                                 column_factory);
  return visitor.VisitAll(std::move(resolved_ast));
}

}  // namespace googlesql
