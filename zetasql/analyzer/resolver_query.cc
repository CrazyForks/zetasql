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

// This file contains the implementation of query-related (i.e. SELECT)
// resolver methods from resolver.h.
#include <ctype.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <stack>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetasql/base/logging.h"
#include "zetasql/analyzer/analytic_function_resolver.h"
#include "zetasql/analyzer/expr_matching_helpers.h"
#include "zetasql/analyzer/expr_resolver_helper.h"
#include "zetasql/analyzer/function_resolver.h"
#include "zetasql/analyzer/function_signature_matcher.h"
#include "zetasql/analyzer/input_argument_type_resolver_helper.h"
#include "zetasql/analyzer/name_scope.h"
#include "zetasql/analyzer/named_argument_info.h"
#include "zetasql/analyzer/path_expression_span.h"
#include "zetasql/analyzer/query_resolver_helper.h"
#include "zetasql/analyzer/recursive_queries.h"
#include "zetasql/analyzer/resolver.h"
#include "zetasql/analyzer/resolver_common_inl.h"
#include "zetasql/common/errors.h"
#include "zetasql/public/function_signature.h"
#include "zetasql/public/functions/array_zip_mode.pb.h"
#include "zetasql/public/input_argument_type.h"
#include "zetasql/public/sql_function.h"
#include "zetasql/public/templated_sql_function.h"
#include "zetasql/resolved_ast/node_sources.h"
// This includes common macro definitions to define in the resolver cc files.
#include "zetasql/common/string_util.h"
#include "zetasql/parser/ast_node.h"
#include "zetasql/parser/ast_node_kind.h"
#include "zetasql/parser/parse_tree.h"
#include "zetasql/parser/parse_tree_errors.h"
#include "zetasql/public/analyzer_options.h"
#include "zetasql/public/analyzer_output_properties.h"
#include "zetasql/public/annotation/collation.h"
#include "zetasql/public/cast.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/coercer.h"
#include "zetasql/public/cycle_detector.h"
#include "zetasql/public/function.h"
#include "zetasql/public/functions/date_time_util.h"
#include "zetasql/public/id_string.h"
#include "zetasql/public/language_options.h"
#include "zetasql/public/options.pb.h"
#include "zetasql/public/parse_location.h"
#include "zetasql/public/proto_util.h"
#include "zetasql/public/select_with_mode.h"
#include "zetasql/public/signature_match_result.h"
#include "zetasql/public/sql_tvf.h"
#include "zetasql/public/sql_view.h"
#include "zetasql/public/strings.h"
#include "zetasql/public/table_valued_function.h"
#include "zetasql/public/templated_sql_tvf.h"
#include "zetasql/public/type.h"
#include "zetasql/public/type.pb.h"
#include "zetasql/public/types/array_type.h"
#include "zetasql/public/types/collation.h"
#include "zetasql/public/types/enum_type.h"
#include "zetasql/public/types/proto_type.h"
#include "zetasql/public/types/struct_type.h"
#include "zetasql/public/types/type_modifiers.h"
#include "zetasql/public/types/type_parameters.h"
#include "zetasql/public/value.h"
#include "zetasql/resolved_ast/make_node_vector.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "zetasql/resolved_ast/resolved_ast_builder.h"
#include "zetasql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "zetasql/resolved_ast/resolved_ast_enums.pb.h"
#include "zetasql/resolved_ast/resolved_ast_helper.h"
#include "zetasql/resolved_ast/resolved_ast_visitor.h"
#include "zetasql/resolved_ast/resolved_collation.h"
#include "zetasql/resolved_ast/resolved_column.h"
#include "zetasql/resolved_ast/resolved_node.h"
#include "zetasql/resolved_ast/resolved_node_kind.h"
#include "zetasql/resolved_ast/resolved_node_kind.pb.h"
#include "zetasql/resolved_ast/target_syntax.h"
#include "zetasql/base/case.h"
#include "absl/algorithm/container.h"
#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "zetasql/base/check.h"
#include "absl/memory/memory.h"
#include "absl/meta/type_traits.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "zetasql/base/map_util.h"
#include "zetasql/base/stl_util.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

namespace {

// Extracts an expression from the given scan at the provided column,
// returning nullptr if the column doesn't exist in the scan or does not
// contain an expression.
absl::StatusOr<const ResolvedExpr*> GetColumnExpr(
    const ResolvedProjectScan* scan, const ResolvedColumn& column) {
  for (const std::unique_ptr<const ResolvedComputedColumn>& computed_column :
       scan->expr_list()) {
    const ResolvedExpr* expr = computed_column->expr();
    ZETASQL_RET_CHECK_NE(expr, nullptr);
    if (computed_column->column().column_id() == column.column_id()) {
      return expr;
    }
  }
  return nullptr;
}

// Validates nested struct constructor in rollup, cube and grouping sets. Only
// single-level struct constructors are allowed in the grouping set.
// See (broken link):syntax.
absl::Status ValidateNestedStructConstructor(const ASTExpression* expr,
                                             const LanguageOptions& language,
                                             absl::string_view clause_name) {
  // The expr will be null if it's an empty grouping set, a.k.a. ().
  if (expr == nullptr) {
    return absl::OkStatus();
  }
  if (!expr->Is<ASTStructConstructorWithParens>()) {
    return absl::OkStatus();
  }

  const ASTStructConstructorWithParens* struct_constructor =
      expr->GetAs<ASTStructConstructorWithParens>();
  for (const ASTExpression* field_expr :
       struct_constructor->field_expressions()) {
    if (field_expr->Is<ASTStructConstructorWithParens>()) {
      return MakeSqlErrorAt(field_expr)
             << "Nested column list is not allowed in " << clause_name;
    }
  }
  return absl::OkStatus();
}

// Validates Rollup to check the feature option is set properly, and all its
// expressions are not nested struct constructors.
absl::Status ValidateRollup(const ASTRollup* rollup,
                            const LanguageOptions& language,
                            size_t grouping_item_count) {
  ZETASQL_RET_CHECK(rollup != nullptr);
  if (!language.LanguageFeatureEnabled(FEATURE_GROUP_BY_ROLLUP)) {
    return MakeSqlErrorAt(rollup) << "GROUP BY ROLLUP is unsupported";
  }
  if (grouping_item_count > 1) {
    return MakeSqlErrorAt(rollup)
           << "The GROUP BY clause only supports ROLLUP when there are no "
           << "other grouping elements";
  }
  // Currently Rollup allows nested struct construtor, and it will be disabled
  // under the proposal (broken link).
  if (language.LanguageFeatureEnabled(FEATURE_V_1_4_GROUPING_SETS)) {
    for (const ASTExpression* expr : rollup->expressions()) {
      ZETASQL_RETURN_IF_ERROR(
          ValidateNestedStructConstructor(expr, language, "ROLLUP"));
    }
  }
  return absl::OkStatus();
}

// Validates Cube to check the feature option is set properly, and all its
// expressions are not nested struct constructors.
absl::Status ValidateCube(const ASTCube* cube, const LanguageOptions& language,
                          size_t grouping_item_count) {
  ZETASQL_RET_CHECK(cube != nullptr);
  if (!language.LanguageFeatureEnabled(FEATURE_V_1_4_GROUPING_SETS)) {
    return MakeSqlErrorAt(cube) << "GROUP BY CUBE is unsupported";
  }
  if (grouping_item_count > 1) {
    return MakeSqlErrorAt(cube)
           << "The GROUP BY clause only supports CUBE when there are no other "
           << "grouping elements";
  }
  for (const ASTExpression* expr : cube->expressions()) {
    ZETASQL_RETURN_IF_ERROR(ValidateNestedStructConstructor(expr, language, "CUBE"));
  }
  return absl::OkStatus();
}

// Validates GroupingSetList and all its sub grouping set.
absl::Status ValidateGroupingSetList(
    const ASTGroupingSetList* grouping_set_list,
    const LanguageOptions& language, size_t grouping_item_count) {
  ZETASQL_RET_CHECK(grouping_set_list != nullptr);
  if (!language.LanguageFeatureEnabled(FEATURE_V_1_4_GROUPING_SETS)) {
    return MakeSqlErrorAt(grouping_set_list)
           << "GROUP BY GROUPING SETS is unsupported";
  }
  if (grouping_item_count > 1) {
    return MakeSqlErrorAt(grouping_set_list)
           << "The GROUP BY clause only supports GROUPING SETS when there are "
              "no other grouping elements";
  }
  for (const ASTGroupingSet* grouping_set :
       grouping_set_list->grouping_sets()) {
    if (grouping_set->rollup() != nullptr) {
      ZETASQL_RETURN_IF_ERROR(ValidateRollup(grouping_set->rollup(), language,
                                     grouping_item_count));
    } else if (grouping_set->cube() != nullptr) {
      ZETASQL_RETURN_IF_ERROR(
          ValidateCube(grouping_set->cube(), language, grouping_item_count));
    } else {
      ZETASQL_RETURN_IF_ERROR(ValidateNestedStructConstructor(
          grouping_set->expression(), language, "GROUPING SETS"));
    }
  }
  return absl::OkStatus();
}

absl::Status CreateUnsupportedGroupingSetsError(
    const QueryResolutionInfo* query_resolution_info, const ASTSelect* select,
    absl::string_view clause_name) {
  if (!query_resolution_info->HasGroupByGroupingSets()) {
    return absl::OkStatus();
  }

  ZETASQL_RET_CHECK(select != nullptr);
  ZETASQL_RET_CHECK(select->group_by() != nullptr);
  ZETASQL_RET_CHECK(!select->group_by()->grouping_items().empty());
  const ASTGroupingItem* grouping_item =
      select->group_by()->grouping_items()[0];

  std::string grouping_clause_name;
  if (grouping_item->rollup() != nullptr) {
    grouping_clause_name = "ROLLUP";
  } else if (grouping_item->cube() != nullptr) {
    grouping_clause_name = "CUBE";
  } else if (grouping_item->grouping_set_list() != nullptr) {
    grouping_clause_name = "GROUPING SETS";
  } else {
    return absl::InvalidArgumentError(
        absl::StrFormat("Expect a node with grouping set, node: %s",
                        grouping_item->DebugString()));
  }

  return MakeSqlErrorAt(grouping_item)
         << absl::StrFormat("GROUP BY %s is not supported in %s",
                            grouping_clause_name, clause_name);
}

// `PartitionedComputedColumns` represents a partitioning of computed columns
// such that:
//    - Each 'dependee_column' is depended upon by at least 1 other column in
//      `dependee_computed_columns` or `non_dependee_computed_columns`.
//    - Each 'non_dependee_column' is not depended on by any other column
//      in `dependee_computed_columns` or `non_dependee_computed_columns`.
struct PartitionedComputedColumns {
  std::vector<std::unique_ptr<const ResolvedComputedColumn>>
      dependee_computed_columns;
  std::vector<std::unique_ptr<const ResolvedComputedColumn>>
      non_dependee_computed_columns;
};

// Partition `computed_columns` into 'dependee' and 'non_dependee' computed
// column sets.
absl::StatusOr<PartitionedComputedColumns> PartitionComputedColumns(
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        computed_columns) {
  // Gather column references.
  absl::flat_hash_set<const ResolvedColumnRef*> column_refs;
  for (const std::unique_ptr<const ResolvedComputedColumn>& computed_column :
       computed_columns) {
    ZETASQL_ASSIGN_OR_RETURN(absl::flat_hash_set<const ResolvedColumnRef*>
                         computed_column_column_refs,
                     CollectFreeColumnRefs(*computed_column));
    column_refs.merge(computed_column_column_refs);
  }

  auto is_dependee_column =
      [&column_refs](const std::unique_ptr<const ResolvedComputedColumn>&
                         computed_column) {
        for (const ResolvedColumnRef* column_ref : column_refs) {
          if (column_ref->column() == computed_column->column()) {
            return true;
          }
        }
        return false;
      };

  // Partition `computed_columns` into 'dependee' and 'non_dependee' sets.
  // 'dependee' computed columns will be held in `computed_columns`;
  // 'non_dependee' computed columns will held in
  // `non_dependee_computed_columns`.
  std::vector<std::unique_ptr<const ResolvedComputedColumn>>
      non_dependee_computed_columns;
  auto it_non_dependee_computed_columns = std::stable_partition(
      computed_columns.begin(), computed_columns.end(), is_dependee_column);
  non_dependee_computed_columns.insert(
      non_dependee_computed_columns.end(),
      std::make_move_iterator(it_non_dependee_computed_columns),
      std::make_move_iterator(computed_columns.end()));
  computed_columns.erase(it_non_dependee_computed_columns,
                         computed_columns.end());
  return PartitionedComputedColumns{
      .dependee_computed_columns = std::move(computed_columns),
      .non_dependee_computed_columns =
          std::move(non_dependee_computed_columns)};
}

// Returns the original select item expression for a given select column state.
const ResolvedExpr* GetPreGroupByResolvedExpr(
    const SelectColumnState* select_column_state) {
  if (select_column_state->original_resolved_expr != nullptr) {
    return select_column_state->original_resolved_expr;
  }
  return select_column_state->resolved_expr == nullptr
             ? select_column_state->resolved_computed_column->expr()
             : select_column_state->resolved_expr.get();
}

}  // namespace

// These are constant identifiers used mostly for generated column or table
// names.  We use a single IdString for each so we never have to allocate
// or copy these strings again.
const IdString& Resolver::kArrayId =
    *new IdString(IdString::MakeGlobal("$array"));
const IdString& Resolver::kOffsetAlias =
    *new IdString(IdString::MakeGlobal("offset"));
const IdString& Resolver::kWeightAlias =
    *new IdString(IdString::MakeGlobal("weight"));
const IdString& Resolver::kArrayOffsetId =
    *new IdString(IdString::MakeGlobal("$array_offset"));
const IdString& Resolver::kLambdaArgId =
    *new IdString(IdString::MakeGlobal("$lambda_arg"));
const IdString& Resolver::kWithActionId =
    *new IdString(IdString::MakeGlobal("$with_action"));
const IdString& Resolver::kRecursionDepthAlias =
    *new IdString(IdString::MakeGlobal("depth"));
const IdString& Resolver::kRecursionDepthId =
    *new IdString(IdString::MakeGlobal("$recursion_depth"));

STATIC_IDSTRING(kDistinctId, "$distinct");
STATIC_IDSTRING(kFullJoinId, "$full_join");
STATIC_IDSTRING(kGroupById, "$groupby");
STATIC_IDSTRING(kMakeProtoId, "$make_proto");
STATIC_IDSTRING(kMakeStructId, "$make_struct");
STATIC_IDSTRING(kOrderById, "$orderby");
STATIC_IDSTRING(kPreGroupById, "$pre_groupby");
STATIC_IDSTRING(kPreProjectId, "$preproject");
STATIC_IDSTRING(kProtoId, "$proto");
STATIC_IDSTRING(kStructId, "$struct");
STATIC_IDSTRING(kValueColumnId, "$value_column");
STATIC_IDSTRING(kCastedColumnId, "$casted_column");
STATIC_IDSTRING(kWeightId, "$sample_weight");
STATIC_IDSTRING(kDummyTableId, "$dummy_table");
STATIC_IDSTRING(kPivotId, "$pivot");
STATIC_IDSTRING(kUnpivotColumnId, "$unpivot");

absl::Status Resolver::CheckForUnwantedSelectClauseChildNodes(
    const ASTSelect* select,
    absl::flat_hash_set<const ASTNode*> allowed_children,
    const char* node_context) {
  for (int i = 0; i < select->num_children(); ++i) {
    const ASTNode* child = select->child(i);
    ZETASQL_RET_CHECK(child != nullptr);
    if (!allowed_children.contains(child)) {
      ZETASQL_RET_CHECK_FAIL() << node_context << " has unexpected child node of type "
                       << child->GetNodeKindString();
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveQueryAfterWith(
    const ASTQuery* query, const NameScope* scope, IdString query_alias,
    const Type* inferred_type_for_query,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  // Note: If <query> is the input to a PIVOT clause, force each projection
  // to create a new column. This is necessary because it is possible for a
  // pivot clause to reference some, but not all, projections of the column, and
  // the rules for PIVOT require only unreferenced columns to be used for
  // grouping; collapsing projections to their underlying column would lose this
  // distinction and cause the list of referenced columns to be calculated
  // incorrectly. As an example, consider the following query:
  //   SELECT * FROM (SELECT x AS y, x AS z FROM t)
  //                 PIVOT (SUM(y) FOR y IN (0,1))
  //
  // If y, and z were collapsed, z would be excluded in the groupby list
  // because we would see y and z as the same column, so the reference to y
  // would incorrectly count as a reference to z.
  bool force_new_columns_for_projected_outputs = query->is_pivot_input();

  if (query->query_expr()->node_kind() == AST_SELECT) {
    // If we just have a single SELECT, then we treat that specially so
    // we can resolve the ORDER BY and LIMIT directly inside that SELECT.
    return ResolveSelect(query->query_expr()->GetAsOrDie<ASTSelect>(),
                         query->order_by(), query->limit_offset(), scope,
                         query_alias, force_new_columns_for_projected_outputs,
                         inferred_type_for_query, output, output_name_list);
  }

  ZETASQL_RETURN_IF_ERROR(
      ResolveQueryExpression(query->query_expr(), scope, query_alias,
                             force_new_columns_for_projected_outputs, output,
                             output_name_list, inferred_type_for_query));

  if (query->order_by() != nullptr) {
    const std::unique_ptr<const NameScope> query_expression_name_scope(
        new NameScope(scope, *output_name_list));
    ZETASQL_RETURN_IF_ERROR(ResolveOrderBySimple(query->order_by(),
                                         query_expression_name_scope.get(),
                                         "ORDER BY clause after set operation",
                                         OrderBySimpleMode::kNormal, output));
  }

  if (query->limit_offset() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveLimitOffsetScan(query->limit_offset(), scope, output));
  }

  return absl::OkStatus();
}

void Resolver::AddNamedSubquery(const std::vector<IdString>& alias,
                                std::unique_ptr<NamedSubquery> named_subquery) {
  auto it = named_subquery_map_.find(alias);
  if (it == named_subquery_map_.end()) {
    named_subquery_map_[std::vector<IdString>{alias}] =
        std::vector<std::unique_ptr<NamedSubquery>>{};
    it = named_subquery_map_.find(alias);
  }
  it->second.push_back(std::move(named_subquery));
}

absl::StatusOr<std::vector<std::unique_ptr<const ResolvedWithEntry>>>
Resolver::ResolveWithClauseIfPresent(const ASTQuery* query,
                                     bool is_outer_query) {
  std::vector<std::unique_ptr<const ResolvedWithEntry>> with_entries;
  if (query->with_clause() != nullptr) {
    if (!is_outer_query &&
        !language().LanguageFeatureEnabled(FEATURE_V_1_1_WITH_ON_SUBQUERY)) {
      return MakeSqlErrorAt(query->with_clause())
             << "WITH is not supported on subqueries in this language version";
    }

    // Check for duplicate WITH aliases
    IdStringHashSetCase alias_names;
    for (const ASTAliasedQuery* with_entry : query->with_clause()->with()) {
      if (!zetasql_base::InsertIfNotPresent(&alias_names,
                                   with_entry->alias()->GetAsIdString())) {
        return MakeSqlErrorAt(with_entry->alias())
               << "Duplicate alias " << with_entry->alias()->GetAsString()
               << " for WITH subquery";
      }
    }

    if (query->with_clause()->recursive()) {
      if (!language().LanguageFeatureEnabled(FEATURE_V_1_3_WITH_RECURSIVE)) {
        return MakeSqlErrorAt(query->with_clause())
               << "RECURSIVE is not supported in the WITH clause";
      }

      ZETASQL_ASSIGN_OR_RETURN(WithEntrySortResult sort_result,
                       SortWithEntries(query->with_clause()));

      for (const ASTAliasedQuery* with_entry : sort_result.sorted_entries) {
        ZETASQL_ASSIGN_OR_RETURN(
            std::unique_ptr<const ResolvedWithEntry> resolved_with_entry,
            ResolveAliasedQuery(
                with_entry,
                sort_result.self_recursive_entries.contains(with_entry)));
        with_entries.push_back(std::move(resolved_with_entry));
      }
    } else {
      // Non-recursive WITH
      for (const ASTAliasedQuery* with_entry : query->with_clause()->with()) {
        ZETASQL_ASSIGN_OR_RETURN(
            std::unique_ptr<const ResolvedWithEntry> resolved_with_entry,
            ResolveAliasedQuery(with_entry, /*recursive=*/false));
        with_entries.push_back(std::move(resolved_with_entry));
      }
    }
  }
  return with_entries;
}

absl::Status Resolver::FinishResolveWithClauseIfPresent(
    const ASTQuery* query,
    std::vector<std::unique_ptr<const ResolvedWithEntry>> with_entries,
    std::unique_ptr<const ResolvedScan>* output) {
  if (query->with_clause() == nullptr) {
    return absl::OkStatus();
  }
  // Now remove any WITH entry mappings we added, restoring what was visible
  // outside this WITH clause.
  for (const ASTAliasedQuery* with_entry : query->with_clause()->with()) {
    const IdString with_alias = with_entry->alias()->GetAsIdString();
    auto it = named_subquery_map_.find({with_alias});
    ZETASQL_RET_CHECK(it != named_subquery_map_.end());
    it->second.pop_back();
    if (it->second.empty()) {
      named_subquery_map_.erase(it);
    }
  }

  // Wrap a ResolvedWithScan around the output query.
  const auto& tmp_column_list = (*output)->column_list();
  *output = MakeResolvedWithScan(tmp_column_list, std::move(with_entries),
                                 std::move(*output),
                                 query->with_clause()->recursive());
  return absl::OkStatus();
}

absl::Status Resolver::ResolveQuery(
    const ASTQuery* query, const NameScope* scope, IdString query_alias,
    bool is_outer_query, std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list,
    const Type* inferred_type_for_query) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  const Type* inferred_type_for_base_query = inferred_type_for_query;

  ZETASQL_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<const ResolvedWithEntry>> with_entries,
      ResolveWithClauseIfPresent(query, is_outer_query));
  ZETASQL_RETURN_IF_ERROR(ResolveQueryAfterWith(query, scope, query_alias,
                                        inferred_type_for_base_query, output,
                                        output_name_list));

  // Add coercions to the final column output types if needed.
  if (is_outer_query && !analyzer_options().get_target_column_types().empty()) {
    ZETASQL_RETURN_IF_ERROR(CoerceQueryStatementResultToTypes(
        query, analyzer_options().get_target_column_types(), output,
        output_name_list));
  }

  ZETASQL_RETURN_IF_ERROR(
      FinishResolveWithClauseIfPresent(query, std::move(with_entries), output));

  // Add parse location to the outermost ResolvedScan only. This is intended
  // because the outermost ResolvedScan represents a query_expr (query or
  // subquery) in the syntax
  // in (broken link)#sql-syntax.
  // This is safe for existing engines because parse locations in resolved AST
  // are mainly for debugging and indexing purposes. Existing engines don't
  // need parse locations during query execution.
  MaybeRecordParseLocation(query, const_cast<ResolvedScan*>(output->get()));

  return absl::OkStatus();
}

// Convert a NameList representing a value table query result into a
// NameList for a FROM clause scanning that value table with <alias>.
// input_name_list and output_name_list may be the same NameList.
static absl::Status ConvertValueTableNameListToNameListWithValueTable(
    const ASTNode* ast_location, IdString alias,
    const std::shared_ptr<const NameList>& input_name_list,
    std::shared_ptr<NameList>* output_name_list) {
  ZETASQL_RET_CHECK(input_name_list->is_value_table());
  ZETASQL_RET_CHECK_EQ(input_name_list->num_columns(), 1);

  std::shared_ptr<NameList> new_name_list(new NameList);
  ZETASQL_RETURN_IF_ERROR(new_name_list->AddValueTableColumn(
      alias, input_name_list->column(0).column(), ast_location));
  *output_name_list = new_name_list;
  return absl::OkStatus();
}

// Create a new NameList that gives the result of converting `input_name_list`
// into a flattened row with a new alias for the row.
// The new row will have the same column list, with `alias` as a range
// variable for the row.  Existing range variables are dropped.
// If `input_name_list` is a value table, `output_name_list` will still be
// a value table.
static absl::Status UpdateNameListForTableAlias(
    const ASTNode* ast_location, IdString alias,
    const std::shared_ptr<const NameList>& input_name_list,
    std::shared_ptr<const NameList>* output_name_list) {
  if (input_name_list->is_value_table()) {
    ZETASQL_RET_CHECK_EQ(input_name_list->num_columns(), 1);
    auto new_name_list = std::make_shared<NameList>();
    // For a value table subquery with an alias, the NameList gets converted
    // to a regular row containing a value table.
    if (input_name_list->HasRangeVariables()) {
      // If the value table has an existing range variable, then it has to be
      // renamed to the new name.
      ZETASQL_RETURN_IF_ERROR(
          new_name_list->MergeFrom(*input_name_list, ast_location,
                                   {.rename_value_table_to_name = &alias}));
    } else {
      // If the value table was anonymous and didn't have an existing range
      // variable, this function will create one, using `alias` as the name.
      ZETASQL_RETURN_IF_ERROR(ConvertValueTableNameListToNameListWithValueTable(
          ast_location, alias, input_name_list, &new_name_list));
    }
    ZETASQL_RETURN_IF_ERROR(new_name_list->SetIsValueTable());
    *output_name_list = new_name_list;
    return absl::OkStatus();
  }

  // For a regular table subquery with an alias, use `flatten_to_table`
  // to drop existing range variables and convert value tables to columns,
  // and then make the new range variables pointing at the remaining columns.
  auto new_name_list = std::make_shared<NameList>();
  ZETASQL_RETURN_IF_ERROR(new_name_list->MergeFrom(*input_name_list, ast_location,
                                           {.flatten_to_table = true}));

  ZETASQL_ASSIGN_OR_RETURN(*output_name_list,
                   NameList::AddRangeVariableInWrappingNameList(
                       alias, ast_location, new_name_list));

  return absl::OkStatus();
}

static absl::Status VerifyNoLimitOrOrderByInRecursiveQuery(
    const ASTQuery* query) {
  if (query->order_by() != nullptr) {
    return MakeSqlErrorAt(query->order_by())
           << "A recursive query may not use ORDER BY";
  }
  if (query->limit_offset() != nullptr) {
    return MakeSqlErrorAt(query->limit_offset())
           << "A recursive query may not use LIMIT";
  }
  return absl::OkStatus();
}

absl::StatusOr<const ASTSetOperation*> Resolver::GetRecursiveUnion(
    const ASTQuery* query) {
  // Skip redundant parentheses around the UNION
  while (query->query_expr()->node_kind() == AST_QUERY) {
    ZETASQL_RETURN_IF_ERROR(VerifyNoLimitOrOrderByInRecursiveQuery(query));
    query = query->query_expr()->GetAsOrDie<ASTQuery>();
  }
  ZETASQL_RETURN_IF_ERROR(VerifyNoLimitOrOrderByInRecursiveQuery(query));

  const ASTSetOperation* query_set_op =
      query->query_expr()->GetAsOrNull<ASTSetOperation>();
  if (query_set_op == nullptr ||
      query_set_op->op_type() != ASTSetOperation::UNION) {
    return MakeSqlErrorAt(query)
           << "Recursive query does not have the form <non-recursive-term> "
           << "UNION [ALL|DISTINCT] <recursive-term>";
  }
  return query_set_op;
}

absl::Status Resolver::ValidateAliasedQueryModifiers(
    IdString query_alias, const ASTAliasedQueryModifiers* modifiers,
    bool is_recursive) {
  if (modifiers->recursion_depth_modifier() != nullptr) {
    if (!language().LanguageFeatureEnabled(
            FEATURE_V_1_4_WITH_RECURSIVE_DEPTH_MODIFIER)) {
      return MakeSqlErrorAt(modifiers->recursion_depth_modifier())
             << "Recursion depth modifier is not supported";
    }
    if (!is_recursive) {
      return MakeSqlErrorAt(modifiers->recursion_depth_modifier())
             << "Recursion depth modifier is not allowed for non-recursive "
                "CTE named "
             << query_alias;
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<const ResolvedRecursionDepthModifier>>
Resolver::ResolveRecursionDepthModifier(
    const ASTRecursionDepthModifier* recursion_depth_modifier) {
  constexpr char kClauseName[] = "WITH DEPTH";
  NameScope empty_scope;
  ExprResolutionInfo expr_resolution_info(&empty_scope, kClauseName);
  std::unique_ptr<const ResolvedExpr> lower, upper;
  if (recursion_depth_modifier->lower_bound()->bound() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveExpr(recursion_depth_modifier->lower_bound()->bound(),
                    &expr_resolution_info, &lower));
    ZETASQL_RETURN_IF_ERROR(ValidateParameterOrLiteralAndCoerceToInt64IfNeeded(
        kClauseName, recursion_depth_modifier->lower_bound(), &lower));
  }
  if (recursion_depth_modifier->upper_bound()->bound() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveExpr(recursion_depth_modifier->upper_bound()->bound(),
                    &expr_resolution_info, &upper));
    ZETASQL_RETURN_IF_ERROR(ValidateParameterOrLiteralAndCoerceToInt64IfNeeded(
        kClauseName, recursion_depth_modifier->upper_bound(), &upper));
  }
  if (lower != nullptr && upper != nullptr && lower->Is<ResolvedLiteral>() &&
      upper->Is<ResolvedLiteral>()) {
    // At this point, we already know lower_value and upper_value are
    // non-negative and not null. We still need to ensure lower bound is no
    // larger than upper bound.
    const Value& lower_value = lower->GetAs<ResolvedLiteral>()->value();
    const Value& upper_value = upper->GetAs<ResolvedLiteral>()->value();
    if (lower_value.int64_value() > upper_value.int64_value()) {
      return MakeSqlErrorAt(recursion_depth_modifier)
             << kClauseName << " expects lower bound (" << lower_value.Format()
             << ") no larger than upper bound (" << upper_value.Format() << ")";
    }
  }

  const IdString depth_column_alias =
      recursion_depth_modifier->alias() != nullptr
          ? recursion_depth_modifier->alias()->GetAsIdString()
          : kRecursionDepthAlias;
  ZETASQL_ASSIGN_OR_RETURN(auto recursion_depth_column,
                   ResolvedColumnHolderBuilder()
                       .set_column(ResolvedColumn(
                           AllocateColumnId(), kRecursionDepthId,
                           depth_column_alias, type_factory_->get_int64()))
                       .Build());
  return ResolvedRecursionDepthModifierBuilder()
      .set_lower_bound(std::move(lower))
      .set_upper_bound(std::move(upper))
      .set_recursion_depth_column(std::move(recursion_depth_column))
      .Build();
}

absl::StatusOr<std::unique_ptr<const ResolvedWithEntry>>
Resolver::ResolveAliasedQuery(const ASTAliasedQuery* with_entry,
                              bool recursive) {
  const IdString with_alias = with_entry->alias()->GetAsIdString();
  if (with_entry->modifiers() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ValidateAliasedQueryModifiers(
        with_alias, with_entry->modifiers(), recursive));
  }

  // Generate a unique alias for this WITH subquery, if necessary.
  IdString unique_alias = with_alias;
  while (!zetasql_base::InsertIfNotPresent(&unique_with_alias_names_, unique_alias)) {
    unique_alias = MakeIdString(absl::StrCat(unique_alias.ToStringView(), "_",
                                             unique_with_alias_names_.size()));
  }

  std::unique_ptr<const ResolvedScan> resolved_subquery;
  std::shared_ptr<const NameList> subquery_name_list;
  if (recursive) {
    // WITH entry is actually recursive (not just defined using the RECURSIVE
    // keyword).
    ZETASQL_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<const ResolvedWithEntry>>
                         inner_with_entries,
                     ResolveWithClauseIfPresent(with_entry->query(),
                                                /*is_outer_query=*/false));
    ZETASQL_ASSIGN_OR_RETURN(const ASTSetOperation* recursive_union,
                     GetRecursiveUnion(with_entry->query()));
    SetOperationResolver setop_resolver(recursive_union, this);
    ZETASQL_RETURN_IF_ERROR(setop_resolver.ResolveRecursive(
        empty_name_scope_.get(), {with_alias}, unique_alias, &resolved_subquery,
        &subquery_name_list));

    if (with_entry->modifiers() != nullptr &&
        with_entry->modifiers()->recursion_depth_modifier() != nullptr) {
      ZETASQL_ASSIGN_OR_RETURN(
          std::unique_ptr<const ResolvedRecursionDepthModifier>
              recursion_depth_modifier,
          ResolveRecursionDepthModifier(
              with_entry->modifiers()->recursion_depth_modifier()));
      ZETASQL_RETURN_IF_ERROR(setop_resolver.FinishResolveRecursionWithModifier(
          with_entry->modifiers()->recursion_depth_modifier(), {with_alias},
          std::move(recursion_depth_modifier), &resolved_subquery,
          &subquery_name_list));
    }
    ZETASQL_RETURN_IF_ERROR(FinishResolveWithClauseIfPresent(
        with_entry->query(), std::move(inner_with_entries),
        &resolved_subquery));
  } else {
    // We always pass empty_name_scope_ when resolving the subquery inside
    // WITH.  Those queries must stand alone and cannot reference any
    // correlated columns or other names defined outside.
    ZETASQL_RETURN_IF_ERROR(ResolveQuery(with_entry->query(), empty_name_scope_.get(),
                                 with_alias, /*is_outer_query=*/false,
                                 &resolved_subquery, &subquery_name_list));

    // We don't want pseudo-columns from the subquery to make it out
    // of the WITH, so prune them away.
    ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForColumnPruning(*subquery_name_list,
                                                    &resolved_subquery));

    AddNamedSubquery({with_alias},
                     std::make_unique<NamedSubquery>(
                         unique_alias, /*recursive_in=*/false,
                         resolved_subquery->column_list(), subquery_name_list));
  }

  // The output columns for the WITH entry cannot be pruned because we require
  // the ResolvedWithRefScan column_list to match them exactly.
  RecordColumnAccess(resolved_subquery->column_list());

  std::unique_ptr<const ResolvedWithEntry> resolved_with_entry =
      MakeResolvedWithEntry(unique_alias.ToString(),
                            std::move(resolved_subquery));
  return resolved_with_entry;
}

absl::Status Resolver::MaybeAddProjectForComputedColumns(
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> computed_columns,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  auto add_project_scan_for_computed_columns =
      [current_scan](std::vector<std::unique_ptr<const ResolvedComputedColumn>>
                         computed_columns) {
        if (computed_columns.empty()) {
          return;
        }

        // Any side effect columns are meant to be used immediately by the
        // consumer $with_side_effect() call and not leak further as an
        // additional column beyond the next scan.
        absl::flat_hash_set<ResolvedColumn> side_effect_columns;
        if ((*current_scan)->Is<ResolvedAggregateScan>()) {
          for (const auto& agg : (*current_scan)
                                     ->GetAs<ResolvedAggregateScan>()
                                     ->aggregate_list()) {
            if (agg->Is<ResolvedDeferredComputedColumn>()) {
              side_effect_columns.insert(
                  agg->GetAs<ResolvedDeferredComputedColumn>()
                      ->side_effect_column());
            }
          }
        }

        ResolvedColumnList wrapper_column_list;
        for (const ResolvedColumn& column : (*current_scan)->column_list()) {
          if (side_effect_columns.find(column) == side_effect_columns.end()) {
            wrapper_column_list.push_back(column);
          }
        }
        for (const auto& computed_column : computed_columns) {
          wrapper_column_list.push_back(computed_column->column());
        }
        *current_scan = MakeResolvedProjectScan(wrapper_column_list,
                                                std::move(computed_columns),
                                                std::move(*current_scan));
      };
  // 1 column or less means only 1 ProjectScan is needed.
  if (computed_columns.size() <= 1) {
    add_project_scan_for_computed_columns(std::move(computed_columns));
    return absl::OkStatus();
  }

  // Partition the computed columns into 2 sets; 'dependee' computed_columns
  // and 'non_dependee' computed_columns.
  ZETASQL_ASSIGN_OR_RETURN(PartitionedComputedColumns partitioned_computed_columns,
                   PartitionComputedColumns(std::move(computed_columns)));

  // Ensure that two projects scans are sufficient. Re-partition the 'dependee'
  // computed columns to ensure that they are now all 'non_dependee' columns.
  // We don't expect 'dependee' columns to remain 'dependees' after the first
  // partitioning, so explicitly check that here.
  ZETASQL_ASSIGN_OR_RETURN(
      PartitionedComputedColumns repartitioned_computed_columns,
      PartitionComputedColumns(
          std::move(partitioned_computed_columns.dependee_computed_columns)));
  ZETASQL_RET_CHECK(repartitioned_computed_columns.dependee_computed_columns.empty())
      << "Computed columns have a dependency tree of depth > 2.";

  // Add project scans that respect the dependency ordering.
  add_project_scan_for_computed_columns(
      std::move(repartitioned_computed_columns.non_dependee_computed_columns));
  add_project_scan_for_computed_columns(
      std::move(partitioned_computed_columns.non_dependee_computed_columns));
  return absl::OkStatus();
}

absl::Status Resolver::MaybeAddProjectForColumnPruning(
    const NameList& name_list,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  const absl::flat_hash_set<ResolvedColumn> old_visible_columns(
      (*current_scan)->column_list().begin(),
      (*current_scan)->column_list().end());

  absl::flat_hash_set<ResolvedColumn> keep_columns;
  for (const ResolvedColumn& column : name_list.GetResolvedColumns()) {
    keep_columns.insert(column);
    ZETASQL_RET_CHECK(old_visible_columns.contains(column)) << column.DebugString();
  }

  // To minimize the changes to existing ProjectScans, rather than using the
  // new list directly, we preserve the original list in its original order,
  // just skipping unwanted columns.
  bool did_pruning = false;
  ResolvedColumnList pruned_column_list;
  for (const ResolvedColumn& column : (*current_scan)->column_list()) {
    if (keep_columns.contains(column)) {
      pruned_column_list.push_back(column);
    } else {
      did_pruning = true;
    }
  }

  if (did_pruning) {
    *current_scan = MakeResolvedProjectScan(
        pruned_column_list, /*expr_list=*/{}, std::move(*current_scan));
  }

  return absl::OkStatus();
}

absl::Status Resolver::AddAggregateScan(
    const ASTSelect* select, bool is_for_select_distinct,
    QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  ResolvedColumnList column_list;
  for (const GroupByColumnState& group_by_column_state :
       query_resolution_info->group_by_column_state_list()) {
    column_list.push_back(group_by_column_state.computed_column->column());
  }
  for (const auto& aggregate_column :
       query_resolution_info->aggregate_columns_to_compute()) {
    column_list.push_back(aggregate_column->column());
    if (aggregate_column->Is<ResolvedDeferredComputedColumn>()) {
      column_list.push_back(
          aggregate_column->GetAs<ResolvedDeferredComputedColumn>()
              ->side_effect_column());
    }
  }
  for (const std::unique_ptr<const ResolvedGroupingCall>& grouping_call :
       query_resolution_info->grouping_columns_list()) {
    column_list.push_back(grouping_call->output_column());
  }

  std::vector<std::unique_ptr<const ResolvedColumnRef>> rollup_column_list;
  std::vector<std::unique_ptr<const ResolvedGroupingSetBase>> grouping_set_list;

  // Retrieve the grouping sets and rollup list for the aggregate scan, if any.
  ZETASQL_RETURN_IF_ERROR(query_resolution_info->ReleaseGroupingSetsAndRollupList(
      &grouping_set_list, &rollup_column_list, language()));

  std::unique_ptr<ResolvedAggregateScan> aggregate_scan =
      MakeResolvedAggregateScan(
          column_list, std::move(*current_scan),
          query_resolution_info->release_group_by_columns_to_compute(),
          query_resolution_info->release_aggregate_columns_to_compute(),
          std::move(grouping_set_list), std::move(rollup_column_list),
          query_resolution_info->release_grouping_call_list());

  for (const auto& aggregate_comp_col : aggregate_scan->aggregate_list()) {
    const auto& aggregate_expr =
        aggregate_comp_col->GetAs<ResolvedComputedColumnImpl>()->expr();
    ZETASQL_RET_CHECK(aggregate_expr->Is<ResolvedAggregateFunctionCall>());
    const auto& aggregate_func_call =
        aggregate_expr->GetAs<ResolvedAggregateFunctionCall>();
    if (aggregate_func_call->function()->Is<SQLFunctionInterface>() ||
        aggregate_func_call->function()->Is<TemplatedSQLFunction>()) {
      analyzer_output_properties_.MarkRelevant(REWRITE_INLINE_SQL_UDAS);
    }
  }

  // If the feature is not enabled, any collation annotation that might exist on
  // the grouping expressions is ignored.
  if (language().LanguageFeatureEnabled(FEATURE_V_1_3_COLLATION_SUPPORT)) {
    std::vector<ResolvedCollation> collation_list;
    bool empty = true;
    for (const auto& group_by_expr : aggregate_scan->group_by_list()) {
      ResolvedCollation resolved_collation;
      if (group_by_expr->expr()->type_annotation_map() != nullptr) {
        ZETASQL_ASSIGN_OR_RETURN(resolved_collation,
                         ResolvedCollation::MakeResolvedCollation(
                             *group_by_expr->expr()->type_annotation_map()));
        empty &= resolved_collation.Empty();
      }
      collation_list.push_back(std::move(resolved_collation));
    }
    if (!empty) {
      aggregate_scan->set_collation_list(collation_list);
    }
  }
  // We might have aggregation without GROUP BY.
  if (!is_for_select_distinct && select->group_by() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveHintsForNode(select->group_by()->hint(), aggregate_scan.get()));
    if (query_resolution_info->is_group_by_all()) {
      analyzer_output_properties_.MarkTargetSyntax(
          aggregate_scan.get(), SQLBuildTargetSyntax::kGroupByAll);
    }
  }

  *current_scan = std::move(aggregate_scan);
  return absl::OkStatus();
}

absl::Status Resolver::AddAnonymizedAggregateScan(
    const ASTSelect* select, QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  if (query_resolution_info->HasGroupByGroupingSets()) {
    ZETASQL_RETURN_IF_ERROR(CreateUnsupportedGroupingSetsError(
        query_resolution_info, select, "anonymization queries"));
  }
  ZETASQL_RET_CHECK(query_resolution_info->select_with_mode() ==
                SelectWithMode::ANONYMIZATION ||
            query_resolution_info->select_with_mode() ==
                SelectWithMode::DIFFERENTIAL_PRIVACY);
  ResolvedColumnList column_list;
  for (const GroupByColumnState& group_by_column_state :
       query_resolution_info->group_by_column_state_list()) {
    column_list.push_back(group_by_column_state.computed_column->column());
  }
  for (const std::unique_ptr<const ResolvedComputedColumnBase>&
           aggregate_column :
       query_resolution_info->aggregate_columns_to_compute()) {
    column_list.push_back(aggregate_column->column());
    if (aggregate_column->Is<ResolvedDeferredComputedColumn>()) {
      column_list.push_back(
          aggregate_column->GetAs<ResolvedDeferredComputedColumn>()
              ->side_effect_column());
    }
  }
  ZETASQL_RET_CHECK(!column_list.empty());
  std::vector<std::unique_ptr<const ResolvedOption>>
      resolved_anonymization_options;
  if (select->select_with() != nullptr &&
      select->select_with()->options() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveAnonymizationOptionsList(
        select->select_with()->options(), *query_resolution_info,
        &resolved_anonymization_options));
  }

  switch (query_resolution_info->select_with_mode()) {
    case SelectWithMode::ANONYMIZATION: {
      auto anonymized_scan = MakeResolvedAnonymizedAggregateScan(
          column_list, std::move(*current_scan),
          query_resolution_info->release_group_by_columns_to_compute(),
          query_resolution_info->release_aggregate_columns_to_compute(),
          /*grouping_set_list=*/{}, /*rollup_column_list=*/{},
          /*grouping_call_list=*/{},
          /*k_threshold_expr=*/nullptr,
          std::move(resolved_anonymization_options));
      // We might have aggregation without GROUP BY.
      if (select->group_by() != nullptr) {
        ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(select->group_by()->hint(),
                                            anonymized_scan.get()));
      }
      *current_scan = std::move(anonymized_scan);
    } break;
    case SelectWithMode::DIFFERENTIAL_PRIVACY: {
      auto anonymized_scan = MakeResolvedDifferentialPrivacyAggregateScan(
          column_list, std::move(*current_scan),
          query_resolution_info->release_group_by_columns_to_compute(),
          query_resolution_info->release_aggregate_columns_to_compute(),
          /*grouping_set_list=*/{}, /*rollup_column_list=*/{},
          /*grouping_call_list=*/{},
          /*group_selection_threshold_expr=*/nullptr,
          std::move(resolved_anonymization_options));
      // We might have aggregation without GROUP BY.
      if (select->group_by() != nullptr) {
        ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(select->group_by()->hint(),
                                            anonymized_scan.get()));
      }
      *current_scan = std::move(anonymized_scan);
    } break;

    default:
      ZETASQL_RET_CHECK_FAIL();
      break;
  }
  analyzer_output_properties_.MarkRelevant(REWRITE_ANONYMIZATION);
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<const ResolvedScan>>
Resolver::AddAggregationThresholdAggregateScan(
    const ASTSelect* select, QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const ResolvedScan> input_scan) {
  ZETASQL_RET_CHECK(query_resolution_info->select_with_mode() ==
            SelectWithMode::AGGREGATION_THRESHOLD);
  ResolvedColumnList column_list;
  for (const GroupByColumnState& group_by_column_state :
       query_resolution_info->group_by_column_state_list()) {
    column_list.push_back(group_by_column_state.computed_column->column());
  }
  for (const std::unique_ptr<const ResolvedComputedColumnBase>&
           aggregate_column :
       query_resolution_info->aggregate_columns_to_compute()) {
    column_list.push_back(aggregate_column->column());
    if (aggregate_column->Is<ResolvedDeferredComputedColumn>()) {
      column_list.push_back(
          aggregate_column->GetAs<ResolvedDeferredComputedColumn>()
              ->side_effect_column());
    }
  }

  std::vector<std::unique_ptr<const ResolvedColumnRef>> rollup_column_list;
  std::vector<std::unique_ptr<const ResolvedGroupingSetBase>> grouping_set_list;

  // Retrieve the grouping sets and rollup list for the aggregate scan, if any.
  ZETASQL_RETURN_IF_ERROR(query_resolution_info->ReleaseGroupingSetsAndRollupList(
      &grouping_set_list, &rollup_column_list, language()));

  ZETASQL_RET_CHECK(!column_list.empty());
  std::vector<std::unique_ptr<const ResolvedOption>> resolved_options;
  if (select->select_with() != nullptr &&
      select->select_with()->options() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveAggregationThresholdOptionsList(
        select->select_with()->options(), *query_resolution_info,
        &resolved_options));
  }
  auto aggregation_threshold_scan =
      MakeResolvedAggregationThresholdAggregateScan(
          column_list, std::move(input_scan),
          query_resolution_info->release_group_by_columns_to_compute(),
          query_resolution_info->release_aggregate_columns_to_compute(),
          std::move(grouping_set_list), std::move(rollup_column_list),
          /*grouping_call_list=*/{}, std::move(resolved_options));
  // When GROUP BY shows up in aggregation, aggregation threshold aggregate scan
  // respects the specified hints.
  if (select->group_by() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(select->group_by()->hint(),
                                        aggregation_threshold_scan.get()));
  }
  // Add the UDA rewriter if present in the aggregates to match the rewrites in
  // the checker which is tested in resolved AST rewriter.
  for (const auto& aggregate_comp_col :
       aggregation_threshold_scan->aggregate_list()) {
    const auto& aggregate_expr = aggregate_comp_col->expr();
    ZETASQL_RET_CHECK(aggregate_expr->Is<ResolvedAggregateFunctionCall>());
    const auto& aggregate_func_call =
        aggregate_expr->GetAs<ResolvedAggregateFunctionCall>();
    if (aggregate_func_call->function()->Is<SQLFunctionInterface>() ||
        aggregate_func_call->function()->Is<TemplatedSQLFunction>()) {
      analyzer_output_properties_.MarkRelevant(REWRITE_INLINE_SQL_UDAS);
    }
  }
  // Add the aggregation threshold rewriter.
  analyzer_output_properties_.MarkRelevant(REWRITE_AGGREGATION_THRESHOLD);
  return aggregation_threshold_scan;
}

absl::Status Resolver::AddAnalyticScan(
    QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  // An analytic function in the ORDER BY can reference select list aliases,
  // so if there are any such SELECT list columns that need precomputing,
  // project them now.  We have to be careful though.  We can only project
  // SELECT list precomputed columns if they do not themselves include
  // analytic functions.  If they do, they must be computed after the
  // AnalyticScan.
  std::vector<std::unique_ptr<const ResolvedComputedColumn>>
      select_columns_without_analytic;
  ZETASQL_RETURN_IF_ERROR(
      query_resolution_info->GetAndRemoveSelectListColumnsWithoutAnalytic(
          &select_columns_without_analytic));

  // TODO: Consider using MaybeAddProject...() here, if we don't
  // care about sorting the column list.
  if (!select_columns_without_analytic.empty()) {
    const std::vector<ResolvedColumn>& column_list =
        (*current_scan)->column_list();
    ResolvedColumnList concat_columns =
        ConcatColumnListWithComputedColumnsAndSort(
            column_list, select_columns_without_analytic);

    *current_scan = MakeResolvedProjectScan(
        concat_columns, std::move(select_columns_without_analytic),
        std::move(*current_scan));
    // Avoid deletion after transfer.
    select_columns_without_analytic.clear();
  }
  return query_resolution_info->analytic_resolver()->CreateAnalyticScan(
      query_resolution_info, current_scan);
}

// Given an input pre-GROUP BY NameScope and a ValidFieldInfoMap
// returns a post-GROUP BY NameScope.  The ValidFieldInfoMap
// represents the mapping between pre-GROUP BY to post-GROUP BY columns
// and fields.  The returned NameScope includes the same previous_scope_
// as the pre-GROUP BY NameScope, while the returned NameScope's local
// NameList is created by merging the pre-GROUP BY local NameList with
// the ValidFieldInfoMap.  The resulting NameScope's local NameList
// includes all the same names as the old one, but columns/fields that are
// not available to access after GROUP BY are marked as invalid to access.
// The names/fields that are valid to access map to post-GROUP BY versions
// of those columns.
//
// WARNING: When we call this function we *MUST* only use a NameScope whose
// previous_scope_ is for an outer/correlation NameScope.  Outer/correlation
// NameScope names should and will remain accessible after GROUP BY or
// DISTINCT, but local names that are not grouped by become invalid.  If
// the 'pre_group_by_scope' used to create the post-GROUP BY Namescope
// has a previous_scope_ that is not an outer/correlation NameScope (for
// example a layered NameScope that adds new names that resolve over the
// FROM clause names) then any names in that previous_scope_ will
// incorrectly remain valid to access after creating the new post-GROUP BY
// NameScope.  This is because CreateNameScopeGivenValidNamePaths() creates
// a new NameScope from 'pre_group_by_scope' by updating its local names()
// and value_table_columns(), but names from previous scopes remain
// unchanged and valid to access in the returned NameScope.
static absl::Status CreatePostGroupByNameScope(
    const NameScope* pre_group_by_scope,
    QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const NameScope>* post_group_by_scope_out) {
  const ValidFieldInfoMap* valid_field_info_map =
      &query_resolution_info->group_by_valid_field_info_map();

  std::unique_ptr<NameScope> post_group_by_scope;
  ZETASQL_RETURN_IF_ERROR(pre_group_by_scope->CreateNameScopeGivenValidNamePaths(
      *valid_field_info_map, &post_group_by_scope));

  *post_group_by_scope_out = std::move(post_group_by_scope);
  return absl::OkStatus();
}

absl::Status Resolver::AddRemainingScansForSelect(
    const ASTSelect* select, const ASTOrderBy* order_by,
    const ASTLimitOffset* limit_offset,
    const NameScope* having_and_order_by_scope,
    std::unique_ptr<const ResolvedExpr>* resolved_having_expr,
    std::unique_ptr<const ResolvedExpr>* resolved_qualify_expr,
    QueryResolutionInfo* query_resolution_info,
    std::shared_ptr<const NameList>* output_name_list,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  SelectColumnStateList* select_column_state_list =
      query_resolution_info->select_column_state_list();

  // Precompute any other columns necessary before aggregation.
  ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
      query_resolution_info
          ->release_select_list_columns_to_compute_before_aggregation(),
      current_scan));

  if (query_resolution_info->HasGroupByOrAggregation()) {
    switch (query_resolution_info->select_with_mode()) {
      case SelectWithMode::ANONYMIZATION:
      case SelectWithMode::DIFFERENTIAL_PRIVACY:
        ZETASQL_RETURN_IF_ERROR(AddAnonymizedAggregateScan(
            select, query_resolution_info, current_scan));
        break;
      case SelectWithMode::AGGREGATION_THRESHOLD: {
        ZETASQL_ASSIGN_OR_RETURN(*current_scan, AddAggregationThresholdAggregateScan(
                                            select, query_resolution_info,
                                            std::move(*current_scan)));
      } break;
      case SelectWithMode::NONE:
        // We know all the GROUP BY and aggregate columns, so can now create an
        // AggregateScan.
        ZETASQL_RETURN_IF_ERROR(AddAggregateScan(select,
                                         /*is_for_select_distinct=*/false,
                                         query_resolution_info, current_scan));
        break;
    }
  }

  // Precompute any other columns necessary after aggregation.
  ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
      query_resolution_info->release_columns_to_compute_after_aggregation(),
      current_scan));

  if (*resolved_having_expr != nullptr) {
    // The HAVING might reference select list aliases, so if there are
    // any such SELECT list columns that need precomputing, precompute
    // those without an analytic function.
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        select_columns_without_analytic;
    ZETASQL_RETURN_IF_ERROR(
        query_resolution_info->GetAndRemoveSelectListColumnsWithoutAnalytic(
            &select_columns_without_analytic));

    if (!select_columns_without_analytic.empty()) {
      const std::vector<ResolvedColumn>& column_list =
          (*current_scan)->column_list();
      ResolvedColumnList concat_columns =
          ConcatColumnListWithComputedColumnsAndSort(
              column_list, select_columns_without_analytic);

      *current_scan = MakeResolvedProjectScan(
          concat_columns, std::move(select_columns_without_analytic),
          std::move(*current_scan));
    }

    const auto& tmp_column_list = (*current_scan)->column_list();
    *current_scan =
        MakeResolvedFilterScan(tmp_column_list, std::move(*current_scan),
                               std::move(*resolved_having_expr));
  }

  if (query_resolution_info->HasAnalytic()) {
    ZETASQL_RETURN_IF_ERROR(AddAnalyticScan(query_resolution_info, current_scan));
  }

  // Precompute any other columns necessary after analytic functions.
  ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
      query_resolution_info->release_columns_to_compute_after_analytic(),
      current_scan));

  // Make additional filter scan if QUALIFY is present.
  if (*resolved_qualify_expr != nullptr) {
    // The QUALIFY clause might reference select list aliases, so if there are
    // any such SELECT list columns that need precomputing, precompute them.
    ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
        query_resolution_info->release_select_list_columns_to_compute(),
        current_scan));

    const auto& tmp_column_list = (*current_scan)->column_list();
    *current_scan =
        MakeResolvedFilterScan(tmp_column_list, std::move(*current_scan),
                               std::move(*resolved_qualify_expr));
  }

  if (select->distinct()) {
    // If there are (aliased or non-aliased) select list columns to compute
    // then add a project first.
    ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
        query_resolution_info->release_select_list_columns_to_compute(),
        current_scan));

    // Note: The DISTINCT processing is very similar to the GROUP BY
    // processing.  The output of GROUP BY is used for resolving subsequent
    // clauses and expressions (e.g., the SELECT list), and the output of
    // DISTINCT is used for resolving the subsequent ORDER BY expressions.
    //
    // These steps include:
    // 1) Creating a new DISTINCT NameScope that includes names from the
    //    SELECT list that are available after DISTINCT.  Other names
    //    become invalid targets in the new DISTINCT NameScope.
    // 2) Then below, the ORDER BY expressions will be resolved against
    //    the new DISTINCT NameScope, returning an error if an invalid
    //    column/name is accessed.
    //
    // For handling DISTINCT, we use all the same machinery as GROUP BY,
    // but mark the QueryResolutionInfo so we know subsequent resolution
    // is for post-DISTINCT processing.
    ZETASQL_RETURN_IF_ERROR(ResolveSelectDistinct(
        select, select_column_state_list, output_name_list->get(), current_scan,
        query_resolution_info, output_name_list));
  }

  if (order_by != nullptr) {
    if (select->distinct()) {
      // Check expected state.  If DISTINCT is present, then we already
      // computed any necessary SELECT list columns before processing the
      // DISTINCT.
      ZETASQL_RET_CHECK(
          query_resolution_info->select_list_columns_to_compute()->empty());

      // If DISTINCT is present, then the ORDER BY expressions have *not*
      // been resolved yet.  Resolve the ORDER BY expressions to reference
      // the post-DISTINCT versions of columns.  Note that the DISTINCT
      // processing already updated <query_resolution_info> with the
      // mapping from pre-DISTINCT to post-DISTINCT versions of columns
      // and expressions, so we simply need to resolve the ORDER BY
      // expressions with the updated <query_resolution_info> and
      // post-distinct NameScope.  Resolution of ORDER BY expressions
      // against the output of DISTINCT has the same characteristics
      // as post-GROUP BY expression resolution.  ORDER BY expressions
      // resolve successfully to columns and path expressions that were
      // output from DISTINCT.  As such, any column reference that is
      // not in the SELECT list is an error.

      // Create a new NameScope for what comes out of the DISTINCT
      // AggregateScan.  It is derived from the <having_and_order_by_scope>,
      // and allows column references to resolve to the post-DISTINCT versions
      // of the columns.
      std::unique_ptr<const NameScope> distinct_scope;
      ZETASQL_RETURN_IF_ERROR(CreatePostGroupByNameScope(
          having_and_order_by_scope, query_resolution_info, &distinct_scope));

      // The second 'distinct_scope' NameScope argument is only
      // used for resolving the arguments to aggregate functions, but when
      // DISTINCT is present then aggregate functions are not allowed in
      // ORDER BY so we will always get an error regardless of whether or
      // not the name is visible post-DISTINCT.
      ZETASQL_RETURN_IF_ERROR(ResolveOrderByExprs(
          order_by, distinct_scope.get(), distinct_scope.get(),
          /*is_post_distinct=*/true, query_resolution_info));
    } else {
      // DISTINCT is *not* present so we have already resolved the ORDER BY
      // expressions in ResolveSelect() and do not resolve them here.
      // Also, the ORDER BY might have computed columns to compute so
      // add a wrapper project to compute them if necessary.
      // TODO: In many cases we can combine the two projects into
      // one, so fix this where possible.  Two projects are required when
      // an ORDER BY expression references a SELECT list alias, such as:
      //   SELECT a+1 as foo FROM T ORDER BY foo + 1;
      // In this case, we need a ProjectScan to compute foo, then another
      // ProjectScan to compute foo + 1.  Simply combining the SELECT
      // list columns to compute with the
      // query_resolution_info->order_by_columns_to_compute() does not work.
      ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
          query_resolution_info->release_select_list_columns_to_compute(),
          current_scan));
    }

    // <query_resolution_info>->order_by_columns_to_compute() already
    // contains the ORDER BY expressions that need computing, but we
    // also need to compute SELECT list computed columns before ordering,
    // so add them to the list.
    for (std::unique_ptr<const ResolvedComputedColumn>& select_computed_column :
         query_resolution_info->release_select_list_columns_to_compute()) {
      query_resolution_info->order_by_columns_to_compute()->push_back(
          std::move(select_computed_column));
    }

    ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
        query_resolution_info->release_order_by_columns_to_compute(),
        current_scan));

    ZETASQL_RETURN_IF_ERROR(MakeResolvedOrderByScan(
        order_by->hint(), query_resolution_info->GetResolvedColumnList(),
        {query_resolution_info->order_by_item_info()},
        current_scan));
  }

  if (order_by == nullptr && !select->distinct()) {
    // TODO: For now, we always add the project if no ORDER BY and
    // no DISTINCT, including both aliased and non-aliased select list
    // columns to precompute.  This is primarily to minimize diffs
    // with the original plans.  We can probably do better about avoiding
    // unnecessary PROJECT nodes and delay projecting the non-aliased
    // expressions once this refactoring is submitted.
      *current_scan = MakeResolvedProjectScan(
          query_resolution_info->GetResolvedColumnList(),
          query_resolution_info->release_select_list_columns_to_compute(),
          std::move(*current_scan));
  }

  if (limit_offset != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveLimitOffsetScan(
        limit_offset, having_and_order_by_scope, current_scan));
  }

  if (select->select_as() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveSelectAs(select->select_as(), *select_column_state_list,
                        std::move(*current_scan), output_name_list->get(),
                        current_scan, output_name_list));
  }

  // Resolve the hint last since we want to attach it as the outermost Scan.
  if (select->hint() != nullptr) {
    // TODO Currently we always add a new ProjectScan to store the
    // hint.  We construct it mutable so we can build the hint_list.

    // To avoid undefined behavior, don't release 'current_scan' and use it in
    // the same function call.
    const std::vector<ResolvedColumn>& column_list =
        (*current_scan)->column_list();
    auto hinted_scan = MakeResolvedProjectScan(column_list, /*expr_list=*/{},
                                               std::move(*current_scan));

    ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(select->hint(), hinted_scan.get()));

    *current_scan = std::move(hinted_scan);
  }
  return absl::OkStatus();
}

absl::Status Resolver::AddColumnsForOrderByExprs(
    IdString query_alias, std::vector<OrderByItemInfo>* order_by_info,
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>*
        computed_columns) {
  for (int order_by_item_idx = 0; order_by_item_idx < order_by_info->size();
       ++order_by_item_idx) {
    OrderByItemInfo& item_info = (*order_by_info)[order_by_item_idx];
    if (!item_info.is_select_list_index()) {
      ZETASQL_RET_CHECK(item_info.order_expression != nullptr);
      if (item_info.order_expression->node_kind() == RESOLVED_COLUMN_REF &&
          !item_info.order_expression->GetAs<ResolvedColumnRef>()
               ->is_correlated()) {
        item_info.order_column =
            item_info.order_expression->GetAs<ResolvedColumnRef>()->column();
      } else {
        bool already_computed = false;
        for (const std::unique_ptr<const ResolvedComputedColumn>&
                 computed_column : *computed_columns) {
          if (IsSameFieldPath(item_info.order_expression.get(),
                              computed_column->expr(),
                              FieldPathMatchingOption::kExpression)) {
            item_info.order_column = computed_column->column();
            item_info.order_expression.reset();  // not needed any more
            already_computed = true;
            break;
          }
        }
        if (already_computed) {
          continue;
        }
        const IdString order_column_alias =
            MakeIdString(absl::StrCat("$orderbycol", order_by_item_idx + 1));
        ResolvedColumn resolved_column(
            AllocateColumnId(), query_alias, order_column_alias,
            item_info.order_expression->annotated_type());
        item_info.order_column = resolved_column;
        computed_columns->emplace_back(MakeResolvedComputedColumn(
            item_info.order_column, std::move(item_info.order_expression)));
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveOrderBySimple(
    const ASTOrderBy* order_by, const NameScope* scope, const char* clause_name,
    OrderBySimpleMode mode, std::unique_ptr<const ResolvedScan>* scan) {
  ZETASQL_RET_CHECK(mode == OrderBySimpleMode::kNormal    //
  );

  // We use a new QueryResolutionInfo because resolving the ORDER BY
  // outside of set operations is independent from its input subquery
  // resolution.
  std::unique_ptr<QueryResolutionInfo> query_resolution_info(
      new QueryResolutionInfo(this));

  query_resolution_info->analytic_resolver()->DisableNamedWindowRefs(
      clause_name);

  ExprResolutionInfo expr_resolution_info(
      scope, scope, scope, /*allows_aggregation_in=*/false,
      /*allows_analytic_in=*/true,
      /*use_post_grouping_columns_in=*/false, clause_name,
      query_resolution_info.get());
  ZETASQL_RETURN_IF_ERROR(ResolveOrderingExprs(
      order_by->ordering_expressions(), &expr_resolution_info,
      query_resolution_info->mutable_order_by_item_info()));

  ZETASQL_RET_CHECK(!query_resolution_info->HasAggregation());

  // If the ORDER BY clause after set operations includes analytic functions,
  // then we need to create an analytic scan for them before we do ordering.
  // For example:
  //
  // SELECT a, b, c FROM t1
  // UNION ALL
  // SELECT a, b, c FROM t2
  // ORDER BY sum(a) OVER (PARTITION BY b ORDER BY c);
  //
  // The ORDER BY binds outside the UNION ALL, so the UNION ALL feeds
  // an AnalyticScan, which in turn feeds the OrderByScan.
  if (query_resolution_info->HasAnalytic()) {
    ZETASQL_RETURN_IF_ERROR(
        query_resolution_info->analytic_resolver()->CreateAnalyticScan(
            query_resolution_info.get(), scan));
  }

  std::vector<std::unique_ptr<const ResolvedComputedColumn>> computed_columns;
  ZETASQL_RETURN_IF_ERROR(AddColumnsForOrderByExprs(
      /*query_alias=*/kOrderById,
      query_resolution_info->mutable_order_by_item_info(), &computed_columns));

  // The output columns of the ORDER BY are the same as the output of the
  // original input.
  const ResolvedColumnList output_columns = (*scan)->column_list();

  // If the ORDER BY requires computed columns, add a wrapper project to
  // compute them.
  ZETASQL_RETURN_IF_ERROR(
      MaybeAddProjectForComputedColumns(std::move(computed_columns), scan));

  return MakeResolvedOrderByScan(order_by->hint(), output_columns,
                                 {query_resolution_info->order_by_item_info()},
                                 scan);
}

absl::Status Resolver::ResolveOrderByItems(
    const std::vector<ResolvedColumn>& output_column_list,
    const OrderByItemInfoVectorList& order_by_info_lists,
    std::vector<std::unique_ptr<const ResolvedOrderByItem>>*
        resolved_order_by_items) {
  resolved_order_by_items->clear();

  ZETASQL_RET_CHECK(!order_by_info_lists.empty());
  for (const auto& order_by_info_vector : order_by_info_lists) {
    for (const OrderByItemInfo& item_info : order_by_info_vector.get()) {
      std::unique_ptr<const ResolvedColumnRef> resolved_column_ref;
      if (item_info.is_select_list_index()) {
        if (item_info.select_list_index < 0 ||
            item_info.select_list_index >= output_column_list.size()) {
          return MakeSqlErrorAt(item_info.ast_location)
                 << "ORDER BY is out of SELECT column number range: "
                 << item_info.select_list_index + 1;
        }
        // NOTE: Accessing scan column list works now as we don't deduplicate
        // anything from the column list.  Thus it matches 1:1 with the select
        // list.  If that changes, we should use name list instead.
        // Convert the select list ordinal reference to a column reference.
        resolved_column_ref =
            MakeColumnRef(output_column_list[item_info.select_list_index]);
      } else {
        ZETASQL_RET_CHECK(item_info.order_column.IsInitialized());
        resolved_column_ref = MakeColumnRef(item_info.order_column);
      }

      std::unique_ptr<const ResolvedExpr> resolved_collation_name;
      if (item_info.ast_collate != nullptr) {
        ZETASQL_RETURN_IF_ERROR(ValidateAndResolveOrderByCollate(
            item_info.ast_collate, item_info.ast_location,
            resolved_column_ref->column().type(), &resolved_collation_name));
      }

      auto resolved_order_by_item = MakeResolvedOrderByItem(
          std::move(resolved_column_ref), std::move(resolved_collation_name),
          item_info.is_descending, item_info.null_order);
      if (language().LanguageFeatureEnabled(FEATURE_V_1_3_COLLATION_SUPPORT)) {
        ZETASQL_RETURN_IF_ERROR(
            CollationAnnotation::ResolveCollationForResolvedOrderByItem(
                resolved_order_by_item.get()));
      }
      resolved_order_by_items->push_back(std::move(resolved_order_by_item));

      if (!resolved_order_by_items->back()
               ->column_ref()
               ->type()
               ->SupportsOrdering(language(), /*type_description=*/nullptr)) {
        return MakeSqlErrorAt(item_info.ast_location)
               << "ORDER BY does not support expressions of type "
               << resolved_order_by_items->back()
                      ->column_ref()
                      ->type()
                      ->ShortTypeName(product_mode());
      }
    }
  }

  return absl::OkStatus();
}

absl::Status Resolver::MakeResolvedOrderByScan(
    const ASTHint* order_by_hint,
    const std::vector<ResolvedColumn>& output_column_list,
    const OrderByItemInfoVectorList& order_by_info_lists,
    std::unique_ptr<const ResolvedScan>* scan) {
  std::vector<std::unique_ptr<const ResolvedOrderByItem>>
      resolved_order_by_items;

  ZETASQL_RETURN_IF_ERROR(
      ResolveOrderByItems(output_column_list, order_by_info_lists,
                          &resolved_order_by_items));

  std::unique_ptr<ResolvedOrderByScan> order_by_scan =
      zetasql::MakeResolvedOrderByScan(output_column_list, std::move(*scan),
                                         std::move(resolved_order_by_items));
  order_by_scan->set_is_ordered(true);

  ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(order_by_hint, order_by_scan.get()));
  *scan = std::move(order_by_scan);

  return absl::OkStatus();
}

absl::Status Resolver::ResolveQueryExpression(
    const ASTQueryExpression* query_expr, const NameScope* scope,
    IdString query_alias, bool force_new_columns_for_projected_outputs,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list,
    const Type* inferred_type_for_query) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  switch (query_expr->node_kind()) {
    case AST_SELECT:
      return ResolveSelect(query_expr->GetAsOrDie<ASTSelect>(),
                           /*order_by=*/nullptr,
                           /*limit_offset=*/nullptr, scope, query_alias,
                           force_new_columns_for_projected_outputs,
                           inferred_type_for_query, output, output_name_list);

    case AST_SET_OPERATION:
      return ResolveSetOperation(query_expr->GetAsOrDie<ASTSetOperation>(),
                                 scope, inferred_type_for_query, output,
                                 output_name_list);

    case AST_QUERY:
      return ResolveQuery(query_expr->GetAsOrDie<ASTQuery>(), scope,
                          query_alias, /*is_outer_query=*/false, output,
                          output_name_list, inferred_type_for_query);

    default:
      break;
  }

  return MakeSqlErrorAt(query_expr) << "Unhandled query_expr:\n"
                                    << query_expr->DebugString();
}

absl::Status Resolver::ResolveAdditionalExprsSecondPass(
    const NameScope* from_clause_or_group_by_scope,
    QueryResolutionInfo* query_resolution_info) {
  for (const auto& entry :
       *query_resolution_info
            ->dot_star_columns_with_aggregation_for_second_pass_resolution()) {
    // Re-resolve the source expression for the dot-star expressions that
    // contain aggregation.
    ExprResolutionInfo expr_resolution_info(
        from_clause_or_group_by_scope, from_clause_or_group_by_scope,
        from_clause_or_group_by_scope,
        /*allows_aggregation_in=*/true, /*allows_analytic_in=*/true,
        query_resolution_info->HasGroupByOrAggregation(), "SELECT list",
        query_resolution_info);
    std::unique_ptr<const ResolvedExpr> resolved_expr;
    ZETASQL_RETURN_IF_ERROR(
        ResolveExpr(entry.second, &expr_resolution_info, &resolved_expr));
    query_resolution_info->columns_to_compute_after_aggregation()->emplace_back(
        MakeResolvedComputedColumn(entry.first, std::move(resolved_expr)));
  }
  for (const auto& entry :
       *query_resolution_info
            ->dot_star_columns_with_analytic_for_second_pass_resolution()) {
    // Re-resolve the source expression for the dot-star expressions.
    ExprResolutionInfo expr_resolution_info(
        from_clause_or_group_by_scope, from_clause_or_group_by_scope,
        from_clause_or_group_by_scope,
        /*allows_aggregation_in=*/true, /*allows_analytic_in=*/true,
        query_resolution_info->HasGroupByOrAggregation(), "SELECT list",
        query_resolution_info);
    std::unique_ptr<const ResolvedExpr> resolved_expr;
    ZETASQL_RETURN_IF_ERROR(
        ResolveExpr(entry.second, &expr_resolution_info, &resolved_expr));
    query_resolution_info->columns_to_compute_after_analytic()->emplace_back(
        MakeResolvedComputedColumn(entry.first, std::move(resolved_expr)));
  }
  return absl::OkStatus();
}

static constexpr absl::string_view SelectWithModeToString(
    SelectWithMode select_with_mode) {
  switch (select_with_mode) {
    case SelectWithMode::NONE:
      return "";
    case SelectWithMode::ANONYMIZATION:
      return "ANONYMIZATION";
    case SelectWithMode::DIFFERENTIAL_PRIVACY:
      return "DIFFERENTIAL_PRIVACY";
    case SelectWithMode::AGGREGATION_THRESHOLD:
      return "AGGREGATION_THRESHOLD";
  }
}

static absl::StatusOr<SelectWithMode> ExtractSelectWithMode(
    const LanguageOptions& language, const ASTSelect* select) {
  if (select->select_with() == nullptr) {
    return SelectWithMode::NONE;
  }
  ZETASQL_RET_CHECK_NE(select->select_with()->identifier(), nullptr);
  std::vector<absl::string_view> allowed_with_identifiers;
  if (language.LanguageFeatureEnabled(FEATURE_ANONYMIZATION)) {
    allowed_with_identifiers.push_back("ANONYMIZATION");
  }
  if (language.LanguageFeatureEnabled(FEATURE_DIFFERENTIAL_PRIVACY)) {
    allowed_with_identifiers.push_back("DIFFERENTIAL_PRIVACY");
  }
  if (language.LanguageFeatureEnabled(FEATURE_AGGREGATION_THRESHOLD)) {
    allowed_with_identifiers.push_back("AGGREGATION_THRESHOLD");
  }
  if (allowed_with_identifiers.empty()) {
    return MakeSqlErrorAt(select->select_with()) << "Unexpected keyword WITH";
  }
  const std::string_view select_with_identifier =
      select->select_with()->identifier()->GetAsStringView();
  ZETASQL_ASSIGN_OR_RETURN(
      SelectWithMode result, [&]() -> absl::StatusOr<SelectWithMode> {
        if (zetasql_base::CaseEqual(select_with_identifier, "ANONYMIZATION") &&
            language.LanguageFeatureEnabled(FEATURE_ANONYMIZATION)) {
          return SelectWithMode::ANONYMIZATION;
        }
        if (zetasql_base::CaseEqual(select_with_identifier,
                                   "DIFFERENTIAL_PRIVACY") &&
            language.LanguageFeatureEnabled(FEATURE_DIFFERENTIAL_PRIVACY)) {
          return SelectWithMode::DIFFERENTIAL_PRIVACY;
        }
        if (zetasql_base::CaseEqual(select_with_identifier,
                                   "AGGREGATION_THRESHOLD") &&
            language.LanguageFeatureEnabled(FEATURE_AGGREGATION_THRESHOLD)) {
          return SelectWithMode::AGGREGATION_THRESHOLD;
        }
        return MakeSqlErrorAt(select->select_with()->identifier())
               << "Invalid identifier after SELECT WITH; expected "
               << absl::StrJoin(allowed_with_identifiers, ", ")
               << " but got: " << select_with_identifier;
      }());

  if (select->distinct()) {
    return MakeSqlErrorAt(select)
           << "SELECT WITH " << absl::AsciiStrToUpper(select_with_identifier)
           << " does not support DISTINCT";
  }
  if (select->from_clause() == nullptr) {
    return MakeSqlErrorAt(select)
           << "SELECT without FROM clause cannot "
              "specify WITH "
           << absl::AsciiStrToUpper(select_with_identifier);
  }
  return result;
}

// Returns true if mode is a SELECT WITH that is always an aggregate scan.
static constexpr bool IsAlwaysAggregateSelectWithMode(SelectWithMode mode) {
  return mode == SelectWithMode::AGGREGATION_THRESHOLD ||
         mode == SelectWithMode::ANONYMIZATION ||
         mode == SelectWithMode::DIFFERENTIAL_PRIVACY;
}

// Returns true if GROUP BY ALL should apply implicitly in this context.
static bool ShouldApplyImplicitGroupByAll(
    const LanguageOptions& language,
    const QueryResolutionInfo* query_resolution_info) {
  return false;
}

absl::Status Resolver::ResolveSelect(
    const ASTSelect* select, const ASTOrderBy* order_by,
    const ASTLimitOffset* limit_offset, const NameScope* external_scope,
    IdString query_alias, bool force_new_columns_for_projected_outputs,
    const Type* inferred_type_for_query,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  // Do this first to generate errors early on unsupported features.
  ZETASQL_ASSIGN_OR_RETURN(SelectWithMode select_with_mode,
                   ExtractSelectWithMode(language(), select));

  std::unique_ptr<const ResolvedScan> scan;
  std::shared_ptr<const NameList> from_clause_name_list;

  ZETASQL_RETURN_IF_ERROR(ResolveFromClauseAndCreateScan(
      select, order_by, external_scope, &scan, &from_clause_name_list));

  ZETASQL_RETURN_IF_ERROR(ResolveSelectAfterFrom(
      select, order_by, limit_offset, external_scope, query_alias,
      select->from_clause() == nullptr ? SelectForm::kNoFrom
                                       : SelectForm::kClassic,
      select_with_mode, force_new_columns_for_projected_outputs,
      inferred_type_for_query, &scan, from_clause_name_list, output_name_list));

  *output = std::move(scan);
  return absl::OkStatus();
}

// Resolves a SELECT query/subquery, resolving all the expressions and
// generating scans necessary to produce its output (as defined by the
// SELECT list).  The logic is generally as follows:
//
// 1) Assume the caller has resolved the FROM clause, generated a scan for it,
//    and built a NameList for what comes out of it.
//
// 2) If present, resolve the WHERE clause against the FROM NameScope,
//    and generate a scan for it on top of the FROM scan.
//
// 3) Do resolution of the remaining clauses (the SELECT list,
//    GROUP BY, HAVING, QUALIFY and ORDER BY).  This step resolves all the
//    expressions (without creating scans), and collects information about the
//    expressions in QueryResolutionInfo.  The collected information includes
//    SELECT list columns and aliases, GROUP BY columns, aggregate
//    columns, analytic function columns, and ORDER BY columns.
//    At a high level, this is done by:
//
//    1. Resolve the SELECT list first pass.  This resolves expressions
//       against the FROM clause NameScope, including star and dot-star
//       expansion.  This first pass is necessary to allow the GROUP BY to
//       resolve against SELECT list aliases.
//    2. Resolve the QUALIFY clause first pass to detect any aggregations.
//    3. Resolve the GROUP BY expressions against SELECT list aliases and
//       the FROM clause NameScope.
//    4. Resolve the SELECT list second pass.  If GROUP BY is present, this
//       re-resolves expressions against a new NameScope that includes
//       grouped versions of the columns (expressions computed after
//       grouping/aggregation must reference the grouped versions of the
//       columns since the original columns are not visible after
//       aggregation).  In this new NameScope, non-grouped columns can still
//       be looked up by name but they provide errors if accessed.  There
//       are optimizations in place to avoid re-resolution of expressions
//       whenever possible.
//    5. Resolve the HAVING clause against another new post-grouped column
//       NameScope that includes SELECT list aliases.
//    6. Resolve the QUALIFY clause against same NameScope as HAVING.
//    7. Resolve ORDER BY expressions against this post-grouped column
//       NameScope (or if DISTINCT is present, then a post-DISTINCT
//       NameScope) that includes SELECT list aliases.
//
// 4) Generate all necessary remaining scans based on the information in
//    QueryResolutionInfo, in the following order:
//    1. PROJECT scan for dot-star columns
//    2. AGGREGATE scan
//    3. PROJECT scan for columns needed by HAVING or QUALIFY
//    4. FILTER scan for HAVING
//    5. PROJECT scan for columns needed by analytic functions
//    6. ANALYTIC scan
//    7. FILTER scan for QUALIFY
//    8. PROJECT scan if needed for DISTINCT
//    9. AGGREGATE scan for DISTINCT
//   10. PROJECT scan for columns needed by ORDER BY
//   11. ORDER BY scan
//   12. LIMIT OFFSET scan
//   13. PROJECT scan for AS STRUCT/PROTO
//   14. PROJECT scan for handling HINTs
//
// For a more detailed discussion, see (broken link).
//
absl::Status Resolver::ResolveSelectAfterFrom(
    const ASTSelect* select, const ASTOrderBy* order_by,
    const ASTLimitOffset* limit_offset, const NameScope* external_scope,
    IdString query_alias, SelectForm select_form,
    SelectWithMode select_with_mode,
    bool force_new_columns_for_projected_outputs,
    const Type* inferred_type_for_query,
    std::unique_ptr<const ResolvedScan>* scan,
    const std::shared_ptr<const NameList>& from_clause_name_list,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  std::unique_ptr<const NameScope> from_scan_scope(
      new NameScope(external_scope, from_clause_name_list));

  // The WHERE clause depends only on the FROM clause, so we resolve it before
  // looking at the SELECT-list or GROUP BY.
  if (select->where_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveWhereClauseAndCreateScan(
        select->where_clause(), from_scan_scope.get(), scan));
  }

  ZETASQL_RET_CHECK(select->select_list() != nullptr);
  ZETASQL_RET_CHECK_EQ(select->from_clause() != nullptr,
               select_form == SelectForm::kClassic);

  std::unique_ptr<QueryResolutionInfo> query_resolution_info(
      new QueryResolutionInfo(this));
  query_resolution_info->set_select_with_mode(select_with_mode);
  SelectColumnStateList* select_column_state_list =
      query_resolution_info->select_column_state_list();

  if (select->window_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(query_resolution_info->analytic_resolver()->SetWindowClause(
        *select->window_clause()));
  }

  query_resolution_info->set_select_form(select_form);
  query_resolution_info->set_has_group_by(select->group_by() != nullptr);
  query_resolution_info->set_has_having(select->having() != nullptr);
  query_resolution_info->set_has_qualify(select->qualify() != nullptr);
  query_resolution_info->set_has_order_by(order_by != nullptr);

  // Remember the columns of current scan (after the FROM clause and WHERE, but
  // before SELECT).
  query_resolution_info->set_from_clause_name_list(from_clause_name_list);

  // We avoid passing down the inferred type to the SELECT list if we have a
  // SELECT AS query.
  const Type* inferred_type_for_select_list = nullptr;
  if (select->select_as() == nullptr) {
    inferred_type_for_select_list = inferred_type_for_query;
  }
  ZETASQL_RETURN_IF_ERROR(ResolveSelectListExprsFirstPass(
      select->select_list(), from_scan_scope.get(), from_clause_name_list,
      query_resolution_info.get(), inferred_type_for_select_list));

  // At this point, query_resolution_info->HasGroupByOrAggregation()` reflects
  // either the presence of a `GROUP BY` clause, or the presence of aggregate
  // functions in the `SELECT` list. There could still be aggregate functions
  // in the `QUALIFY` clause that have not been detected yet. There may also be
  // aggregate functions in the `ORDER BY` clause, though that will be an error.
  // We need to detect the aggregation in `QUALIFY` before we continue because
  // subsequent tasks are handled differently for queries with aggregation or no
  // aggregation.
  if (!query_resolution_info->HasGroupByOrAggregation() &&
      select->qualify() != nullptr) {
    // QUALIFY may reference names in the from clause scope or explicit aliases
    // introduced in the SELECT list.
    std::shared_ptr<NameList> post_analytic_name_list(new NameList);
    for (const std::unique_ptr<SelectColumnState>& select_column_state :
         select_column_state_list->select_column_state_list()) {
      if (!select_column_state->alias.empty() &&
          !IsInternalAlias(select_column_state->alias)) {
        if (select_column_state->resolved_expr->Is<ResolvedColumnRef>()) {
          // The expression already resolved to a column (either correlated
          // or uncorrelated is ok), so just use it.
          ZETASQL_RETURN_IF_ERROR(post_analytic_name_list->AddColumn(
              select_column_state->alias,
              select_column_state->resolved_expr->GetAs<ResolvedColumnRef>()
                  ->column(),
              /*is_explicit=*/true));
        } else {
          ZETASQL_RET_CHECK(select_column_state->GetType() != nullptr);
          ResolvedColumn select_column(AllocateColumnId(), query_alias,
                                       select_column_state->alias,
                                       select_column_state->GetType());
          ZETASQL_RETURN_IF_ERROR(post_analytic_name_list->AddColumn(
              select_column_state->alias, select_column, /*is_explicit=*/true));
        }
      }
    }
    std::unique_ptr<NameScope> qualify_name_scope;
    ZETASQL_RETURN_IF_ERROR(from_scan_scope->CopyNameScopeWithOverridingNames(
        post_analytic_name_list, &qualify_name_scope));

    auto qualify_expr_resolution_info = std::make_unique<ExprResolutionInfo>(
        qualify_name_scope.get(), query_resolution_info.get(),
        select->qualify()->expression());
    // We don't use this ResolvedExpr. For this first pass, we are only
    // resolving this for the side effect in query_resolution_info so that
    // subsequent steps get the correct value for `HasGroupByOrAggregation()`.
    std::unique_ptr<const ResolvedExpr> resolved_qualify_expr;
    ZETASQL_RETURN_IF_ERROR(ResolveExpr(
        select->qualify()->expression(), qualify_expr_resolution_info.get(),
        &resolved_qualify_expr, /*inferred_type=*/nullptr));
    bool has_aggregation = query_resolution_info->HasAggregation();
    // Reset GroupByInfo. We will re-resolve the QUALIFY expression later with
    // the real input columns. We need to clear it here so that we don't end up
    // with duplicate entries in the group_by_expr_map after we re-resolve
    // QUALIFY.
    query_resolution_info->ClearGroupByInfo();
    query_resolution_info->SetHasAggregation(has_aggregation);
  }

  // Return an appropriate error for anonymization queries that don't perform
  // aggregation.
  // This check is required because we have to wait until after resolving the
  // SELECT list (first pass) to know if there are aggregate functions
  // present.
  if (IsAlwaysAggregateSelectWithMode(
          query_resolution_info->select_with_mode()) &&
      !query_resolution_info->HasGroupByOrAggregation()) {
    ZETASQL_RET_CHECK_GT(select->select_list()->columns().size(), 0);
    return MakeSqlErrorAt(select->select_list()->columns(0))
           << "You must use GROUP BY or aggregation function in a SELECT WITH "
           << SelectWithModeToString(select_with_mode)
           << " query, but it was absent";
  }

  if (query_resolution_info->HasGroupByOrAggregation() &&
      query_resolution_info->HasHavingOrQualifyOrOrderBy()) {
    // We have GROUP BY or aggregation in the SELECT list (we performed
    // first pass SELECT list expression resolution above), and we have
    // either HAVING or QUALIFY or ORDER BY.  This implies that the expressions
    // in HAVING or QUALIFY or ORDER BY could reference SELECT list aliases,
    // which might need to resolve against either the pre- or post- grouping
    // version of the column.  Consider:
    //
    //   SELECT key as foo
    //   FROM table
    //   GROUP BY key
    //   HAVING foo > 5
    //
    //   SELECT key as foo
    //   FROM table
    //   GROUP BY key
    //   HAVING sum(foo) > 5
    //
    // In the first query, HAVING 'foo' must resolve to the post-grouped version
    // of table.key since the HAVING gets applied after aggregation.  In the
    // second query, HAVING 'foo' must resolve to the pre-grouped version of
    // table.key since the aggregation function is applied on top of it.  To
    // address this, we need to assign and remember pre-grouped versions of
    // all the SELECT list columns that have non-internal aliases (since those
    // could get referenced in HAVING or QUALIFY or ORDER BY).
    ZETASQL_RETURN_IF_ERROR(AnalyzeSelectColumnsToPrecomputeBeforeAggregation(
        query_resolution_info.get()));
  }

  if (select->group_by() != nullptr) {
    if (select->group_by()->all() != nullptr) {
      ZETASQL_RETURN_IF_ERROR(ResolveGroupByAll(select->group_by(),
                                        from_scan_scope.get(),
                                        query_resolution_info.get()));
    } else {
      ZETASQL_RETURN_IF_ERROR(ResolveGroupByExprs(select->group_by(),
                                          from_scan_scope.get(),
                                          query_resolution_info.get()));
    }
  }

  if (select->group_by() == nullptr &&
      ShouldApplyImplicitGroupByAll(language(), query_resolution_info.get())) {
    ZETASQL_RETURN_IF_ERROR(ResolveGroupByAll(select->group_by(), from_scan_scope.get(),
                                      query_resolution_info.get()));
  }

  bool check_from_clause = select->from_clause() == nullptr;
  if (check_from_clause) {
    // Note that we do not allow HAVING or ORDER BY if there is no FROM
    // clause, so even though aggregation or analytic can appear there
    // we do not have to wait until we analyze those clauses to check for
    // this error condition.
    if (query_resolution_info->HasGroupByOrAggregation()) {
      return MakeSqlErrorAt(select)
             << "SELECT without FROM clause cannot use aggregation";
    }
    if (query_resolution_info->HasAnalytic()) {
      return MakeSqlErrorAt(select)
             << "SELECT without FROM clause cannot use analytic functions";
    }
  }

  if (!query_resolution_info->HasGroupByOrAggregation() &&
      !query_resolution_info->HasAnalytic()) {
    // There is no GROUP BY, and no aggregation or analytic functions in the
    // SELECT list, so the initial resolution pass on the SELECT list is
    // final.  This will create ResolvedColumns for the SELECT columns, and
    // identify any columns necessary to precompute.  Once these SELECT list
    // ResolvedColumns are assigned, we avoid calling ResolveExpr() again on
    // the SELECT list expressions in ResolveAdditionalExprsSecondPass().
    // TODO: We should be able to avoid the second pass on SELECT
    // list expressions even if there is analytic present, but we currently
    // reset the analytic resolver state before re-resolving the SELECT and
    // resolving the ORDER BY (which can contain analytic functions).
    // Fix this.
    FinalizeSelectColumnStateList(select->select_list(), query_alias,
                                  force_new_columns_for_projected_outputs,
                                  query_resolution_info.get(),
                                  select_column_state_list);
  }

  // Resolve the SELECT list against what comes out of the GROUP BY.
  // GROUP BY columns that are select list ordinals or aliases already
  // have resolved and updated column references.  Aggregate subexpressions
  // have also already been resolved.  Other expressions will resolve against
  // a GROUP BY name scope, with aggregate subexpressions mapped to their
  // already-resolved columns.

  // Create a new NameScope for what comes out of the AggregateScan.
  // It is derived from the FROM clause scope, allows column references to
  // resolve to the grouped versions of the columns, and provides errors
  // for column references that are not grouped by or aggregated.
  std::unique_ptr<const NameScope> group_by_scope;
  const NameScope* from_clause_or_group_by_scope = from_scan_scope.get();
  if (query_resolution_info->HasGroupByOrAggregation()) {
    // Create a new NameScope that reflects what names are and are not
    // available post-GROUP BY.
    ZETASQL_RETURN_IF_ERROR(CreatePostGroupByNameScope(
        from_scan_scope.get(), query_resolution_info.get(), &group_by_scope));
    from_clause_or_group_by_scope = group_by_scope.get();
  }

  // The analytic function resolver contains information collected during
  // the initial analysis of the SELECT list columns.  Reset and
  // re-initialize the analytic function resolver for second-pass SELECT
  // list resolution and ORDER BY resolution.
  //
  // Note that when we reset the analytic resolver here, we lose all of
  // the information about the currently resolved analytic expressions.
  // This implies that we *must* re-resolve all analytic expressions in
  // order to be able to generate appropriate AnalyticScans.
  if (query_resolution_info->HasAnalytic()) {
    query_resolution_info->ResetAnalyticResolver(this);
  }

  std::shared_ptr<NameList> final_project_name_list(new NameList);

  ZETASQL_RETURN_IF_ERROR(ResolveSelectListExprsSecondPass(
      query_alias, from_clause_or_group_by_scope, &final_project_name_list,
      query_resolution_info.get()));

  ZETASQL_RETURN_IF_ERROR(ResolveAdditionalExprsSecondPass(
      from_clause_or_group_by_scope, query_resolution_info.get()));

  *output_name_list = final_project_name_list;

  // Create new NameLists for SELECT list columns/aliases, since they can be
  // referenced elsewhere in the query (in the GROUP BY, HAVING, and ORDER BY).
  // This NameList reflects post-grouped versions of SELECT list columns
  // (if grouping is present).
  std::shared_ptr<NameList> post_group_by_alias_name_list(new NameList);
  // This NameList reflects pre-grouped versions of SELECT list columns
  // (if grouping is present).
  std::shared_ptr<NameList> pre_group_by_alias_name_list(new NameList);
  // The 'error_name_targets' identify SELECT list aliases whose related
  // expressions contain aggregation or analytic functions.  These
  // NameTargets will be used in a NameScope for resolving HAVING clause
  // aggregate function arguments, where references to these SELECT
  // list aliases are invalid.
  IdStringHashMapCase<NameTarget> error_name_targets;
  std::set<IdString, IdStringCaseLess> select_column_aliases;
  for (const std::unique_ptr<SelectColumnState>& select_column_state :
       select_column_state_list->select_column_state_list()) {
    ZETASQL_RETURN_IF_ERROR(CreateSelectNamelists(
        select_column_state.get(), post_group_by_alias_name_list.get(),
        pre_group_by_alias_name_list.get(), &error_name_targets,
        &select_column_aliases));
  }

  // The NameScope to use when resolving the HAVING and ORDER BY clauses.
  // Includes the GROUP BY scope, along with additional SELECT list aliases
  // for post-grouping versions of columns.
  // SELECT list aliases override any names in group_by_scope.
  std::unique_ptr<NameScope> having_and_order_by_scope;
  ZETASQL_RETURN_IF_ERROR(
      from_clause_or_group_by_scope->CopyNameScopeWithOverridingNames(
          post_group_by_alias_name_list, &having_and_order_by_scope));

  // The NameScope to use when resolving aggregate functions in the HAVING
  // and ORDER BY clauses.  It is the <from_scan_scope>, extended with
  // SELECT list aliases for pre-grouping columns.  The SELECT list aliases
  // override the names in <from_scan_scope>.
  std::unique_ptr<const NameScope> original_select_list_and_from_scan_scope(
      new NameScope(from_scan_scope.get(), pre_group_by_alias_name_list));

  // SELECT list aliases related to aggregate expressions are not valid
  // to access as aggregate function arguments, so update the
  // <original_select_list_and_from_scan_scope> to mark those aliases as
  // invalid to access.
  std::unique_ptr<NameScope> select_list_and_from_scan_scope;

  ZETASQL_RETURN_IF_ERROR(
      original_select_list_and_from_scan_scope
          ->CopyNameScopeWithOverridingNameTargets(
              error_name_targets, &select_list_and_from_scan_scope));

  // Analyze the HAVING.
  // TODO: Should probably move <resolved_having_expr> to
  // <query_resolution_info>.
  std::unique_ptr<const ResolvedExpr> resolved_having_expr;
  if (select->having() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveHavingExpr(select->having(), having_and_order_by_scope.get(),
                          select_list_and_from_scan_scope.get(),
                          query_resolution_info.get(), &resolved_having_expr));
  }

  std::unique_ptr<const ResolvedExpr> resolved_qualify_expr;
  if (select->qualify() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveQualifyExpr(
        select->qualify(), having_and_order_by_scope.get(),
        select_list_and_from_scan_scope.get(), query_resolution_info.get(),
        &resolved_qualify_expr));
  }

  // Analyze the ORDER BY.  If we have SELECT DISTINCT, we will resolve
  // the ORDER BY expressions after resolving the DISTINCT since we must
  // resolve the ORDER BY against post-DISTINCT versions of columns.
  if (order_by != nullptr && !select->distinct()) {
    ZETASQL_RETURN_IF_ERROR(ResolveOrderByExprs(
        order_by, having_and_order_by_scope.get(),
        select_list_and_from_scan_scope.get(), /*is_post_distinct=*/false,
        query_resolution_info.get()));
  }

  // We are done with analysis and can now build the remaining scans.
  // The current <scan> covers the FROM and WHERE clauses.  The remaining
  // scans are built on top of the current <scan>.
  ZETASQL_RETURN_IF_ERROR(AddRemainingScansForSelect(
      select, order_by, limit_offset, having_and_order_by_scope.get(),
      &resolved_having_expr, &resolved_qualify_expr,
      query_resolution_info.get(), output_name_list, scan));

    // Any columns produced in a SELECT list (for the final query or any
    // subquery) count as referenced and cannot be pruned.
    RecordColumnAccess((*scan)->column_list());

  // Some sanity checks.
  // Note that we cannot check that the number of columns in the
  // output_name_list is the same as the number in select_column_state_list,
  // because this is not true for SELECT AS STRUCT or SELECT AS PROTO.

  // All columns to compute have been consumed.
  ZETASQL_RETURN_IF_ERROR(query_resolution_info->CheckComputedColumnListsAreEmpty());

  return absl::OkStatus();
}

absl::Status Resolver::ResolveModelTransformSelectList(
    const NameScope* input_scope, const ASTSelectList* select_list,
    const std::shared_ptr<const NameList>& input_cols_name_list,
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>* transform_list,
    std::vector<std::unique_ptr<const ResolvedOutputColumn>>*
        transform_output_column_list,
    std::vector<std::unique_ptr<const ResolvedAnalyticFunctionGroup>>*
        transform_analytic_function_group_list) {
  QueryResolutionInfo query_info(this);
  for (int i = 0; i < select_list->columns().size(); ++i) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveSelectColumnFirstPass(select_list->columns(i), input_scope,
                                     input_cols_name_list, i, &query_info));
  }
  FinalizeSelectColumnStateList(
      select_list, kDummyTableId,
      /*force_new_columns_for_projected_outputs=*/false, &query_info,
      query_info.select_column_state_list());

  // Creates ResolvedComputedColumn for each select column.
  absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
      col_names;
  for (int i = 0;
       i <
       query_info.select_column_state_list()->select_column_state_list().size();
       ++i) {
    const SelectColumnState* select_column_state =
        query_info.select_column_state_list()->GetSelectColumnState(i);
    if (IsInternalAlias(select_column_state->alias)) {
      return MakeSqlErrorAt(select_column_state->ast_expr)
             << "Anonymous columns are disallowed in TRANSFORM clause. Please "
                "provide a column name";
    }
    if (select_column_state->has_aggregation) {
      return MakeSqlErrorAt(select_column_state->ast_expr)
             << "Aggregation functions are not supported in TRANSFORM clause";
    }
    if (!zetasql_base::InsertIfNotPresent(&col_names, select_column_state->alias)) {
      return MakeSqlErrorAt(select_column_state->ast_expr)
             << "Duplicate column aliases are disallowed in TRANSFORM clause";
    }
    if (select_column_state->has_analytic) {
      const std::vector<
          std::unique_ptr<AnalyticFunctionResolver::AnalyticFunctionGroupInfo>>&
          analytic_function_groups =
              query_info.analytic_resolver()->analytic_function_groups();
      for (const auto& analytic_function_group : analytic_function_groups) {
        if (analytic_function_group->ast_partition_by != nullptr ||
            analytic_function_group->ast_order_by != nullptr) {
          return MakeSqlErrorAt(select_column_state->ast_expr)
                 << "Analytic functions with a non-empty OVER() clause are "
                    "disallowed in the TRANSFORM clause";
        }
        // resolved_computed_columns could be empty after merging. We only add
        // non-empty ones for clarity.
        if (!analytic_function_group->resolved_computed_columns.empty()) {
          transform_analytic_function_group_list->push_back(
              MakeResolvedAnalyticFunctionGroup(
                  /*partition_by=*/nullptr, /*order_by=*/nullptr,
                  std::move(
                      analytic_function_group->resolved_computed_columns)));
        }
      }
    }

    if (select_column_state->resolved_expr != nullptr) {
      // This is a column reference without any computation.
      ZETASQL_RET_CHECK(
          select_column_state->resolved_expr->GetAs<ResolvedColumnRef>() !=
          nullptr)
          << "resolved_expr should be of type ResolvedColumnRef in "
             "ResolveModelTransformSelectList";
      const ResolvedColumnRef* resolved_col_ref =
          select_column_state->resolved_expr->GetAs<ResolvedColumnRef>();
      const ResolvedColumn resolved_col_cp(
          AllocateColumnId(),
          analyzer_options_.id_string_pool()->Make(
              resolved_col_ref->column().table_name()),
          analyzer_options_.id_string_pool()->Make(
              select_column_state->alias.ToString()),
          resolved_col_ref->column().type());
      transform_list->push_back(MakeResolvedComputedColumn(
          resolved_col_cp,
          MakeResolvedColumnRef(resolved_col_ref->column().type(),
                                resolved_col_ref->column(),
                                /*is_correlated=*/false)));
    } else {
      // This is a computed column.
      ZETASQL_RET_CHECK(select_column_state->resolved_computed_column != nullptr)
          << "resolved_computed_column cannot be nullptr in "
             "ResolveModelTransformSelectList when resolved_expr is nullptr";
      zetasql::ResolvedASTDeepCopyVisitor deep_copy_visitor;
      ZETASQL_RETURN_IF_ERROR(
          select_column_state->resolved_computed_column->expr()->Accept(
              &deep_copy_visitor));
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedExpr> resolved_expr_copy,
                       deep_copy_visitor.ConsumeRootNode<ResolvedExpr>());
      transform_list->push_back(MakeResolvedComputedColumn(
          select_column_state->resolved_computed_column->column(),
          std::move(resolved_expr_copy)));
    }
    transform_output_column_list->push_back(MakeResolvedOutputColumn(
        select_column_state->alias.ToString(),
        transform_list->at(transform_list->size() - 1)->column()));
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveCreateModelAliasedQueryList(
    const ASTAliasedQueryList* aliased_query_list,
    std::vector<std::unique_ptr<const ResolvedCreateModelAliasedQuery>>*
        resolved_aliased_query_list) {
  IdStringHashSetCase alias_names;
  for (const ASTAliasedQuery* aliased_query :
       aliased_query_list->aliased_query_list()) {
    const IdString alias_id_string = aliased_query->alias()->GetAsIdString();
    const std::string alias_string = aliased_query->alias()->GetAsString();

    // Checks for duplicate aliases.
    if (!zetasql_base::InsertIfNotPresent(&alias_names, alias_id_string)) {
      return MakeSqlErrorAt(aliased_query->alias())
             << "Duplicate alias " << alias_string << " for aliased query list";
    }

    std::unique_ptr<const ResolvedScan> query_scan;
    std::vector<std::unique_ptr<const ResolvedOutputColumn>> output_column_list;
    // Create model does not allow value table as input.
    bool is_value_table = false;
    // Each subquery is resolved independently.
    // We are using ResolveQueryAndOutputColumns to resolve the query which is
    // the same way as how create model is resolving its input query right now.
    // For create model statement, aliased queries are independent of each
    // other, and are essentially the same as create model's input query.
    // Therefore we can keep the behavior consistent across the places.
    ZETASQL_RETURN_IF_ERROR(ResolveQueryAndOutputColumns(
        aliased_query->query(),
        /*object_type=*/"MODEL",
        /*is_recursive_view=*/false, /*table_name_id_string=*/{},
        alias_id_string,
        /*view_explicit_column_list=*/nullptr, &query_scan, &is_value_table,
        &output_column_list, /*column_definition_list=*/nullptr));

    std::unique_ptr<const ResolvedCreateModelAliasedQuery>
        resolved_aliased_query = MakeResolvedCreateModelAliasedQuery(
            alias_string, std::move(query_scan), std::move(output_column_list));
    resolved_aliased_query_list->push_back(std::move(resolved_aliased_query));
  }
  return absl::OkStatus();
}

absl::Status Resolver::CreateSelectNamelists(
    const SelectColumnState* select_column_state,
    NameList* post_group_by_alias_name_list,
    NameList* pre_group_by_alias_name_list,
    IdStringHashMapCase<NameTarget>* error_name_targets,
    std::set<IdString, IdStringCaseLess>* select_column_aliases) {
  // Test expected invariant.
  ZETASQL_RET_CHECK(select_column_state->resolved_select_column.IsInitialized());

  // The alias names should be marked explicit, since they were either
  // explicitly in the query (SELECT col AS alias), were derived
  // from a path expression that was explicitly in the query
  // (SELECT table.col), or were internal (SELECT a+1) where it does
  // not matter.
  //
  // TODO: Consider only doing this for non-internal aliases
  // (i.e., if !IsInternalAlias(select_column_state->alias)), since only
  // those should be able to be referenced elsewhere in the query.  There
  // might be reasons why this will not work (for instance positional
  // references), so investigate this in a post-refactoring changelist.
  ZETASQL_RETURN_IF_ERROR(post_group_by_alias_name_list->AddColumn(
      select_column_state->alias, select_column_state->resolved_select_column,
      /*is_explicit=*/true));

  const ResolvedColumn target_column =
      select_column_state->HasPreGroupByResolvedColumn()
          ? select_column_state->resolved_pre_group_by_select_column
          : select_column_state->resolved_select_column;
  ZETASQL_RETURN_IF_ERROR(pre_group_by_alias_name_list->AddColumn(
      select_column_state->alias, target_column, /*is_explicit=*/true));
  if (select_column_state->has_aggregation ||
      select_column_state->has_analytic) {
    if (zetasql_base::ContainsKey(*select_column_aliases, select_column_state->alias)) {
      // There's already a SELECT list alias for this, so make the
      // NameTarget ambiguous as well.  Note that the NameTarget may or
      // may not exist yet in <error_name_targets>.
      zetasql_base::InsertOrUpdate(error_name_targets, select_column_state->alias,
                          NameTarget());
      return absl::OkStatus();
    }
    NameTarget name_target(target_column, /*is_explicit=*/true);
    name_target.SetAccessError(
        NameTarget::EXPLICIT_COLUMN,
        select_column_state->has_aggregation
            ? "Aggregations of aggregations are not allowed"
            : "Analytic functions cannot be arguments to aggregate functions");
    // This insert should succeed, since if there was already an entry for
    // this alias we would have handled the alias as ambiguous above.
    ZETASQL_RET_CHECK(zetasql_base::InsertIfNotPresent(error_name_targets,
                                      select_column_state->alias, name_target));
  } else {
    if (zetasql_base::ContainsKey(*error_name_targets, select_column_state->alias)) {
      // We saw a non-aggregate SELECT column with the same alias as
      // an aggregate/analytic SELECT column.  Ensure that the related
      // NameTarget is ambiguous.
      zetasql_base::InsertOrUpdate(error_name_targets, select_column_state->alias,
                          NameTarget());
      return absl::OkStatus();
    }
  }
  select_column_aliases->insert(select_column_state->alias);
  return absl::OkStatus();
}

absl::Status Resolver::AnalyzeSelectColumnsToPrecomputeBeforeAggregation(
    QueryResolutionInfo* query_resolution_info) {
  SelectColumnStateList* select_column_state_list =
      query_resolution_info->select_column_state_list();
  for (int idx = 0; idx < select_column_state_list->Size(); ++idx) {
    SelectColumnState* select_column_state =
        select_column_state_list->GetSelectColumnState(idx);
    // If the column has analytic or aggregation, then we do not compute
    // this before the AggregateScan.
    if (select_column_state->has_aggregation ||
        select_column_state->has_analytic) {
      continue;
    }

    // Only if the select list item has an explicit or inferrable alias, can it
    // be used by other parts of the queries like GROUP BY, HAVING, etc.
    if (!IsInternalAlias(select_column_state->alias)) {
      ZETASQL_RET_CHECK(select_column_state->resolved_expr != nullptr);
      ResolvedColumn pre_group_by_column;
      if (select_column_state->resolved_expr->node_kind() ==
          RESOLVED_COLUMN_REF) {
        // The expression already resolved to a column (either correlated
        // or uncorrelated is ok), so just use it.
        pre_group_by_column =
            select_column_state->resolved_expr->GetAs<ResolvedColumnRef>()
                ->column();
      } else {
        // The expression is not a simple column reference, it is a more
        // complicated expression that need to be computed before aggregation
        // if we GROUP BY that computed column.
        pre_group_by_column = ResolvedColumn(
            AllocateColumnId(), kPreGroupById, select_column_state->alias,
            select_column_state->resolved_expr->annotated_type());
        // If the expression is a path expression then collect that
        // information in the QueryResolutionInfo so that we know that
        // accessing that path is valid post-GROUP BY, even if accessing
        // the source of the path is not.
        ResolvedColumn source_column;
        bool unused_is_correlated;
        ValidNamePath valid_name_path;
        if (GetSourceColumnAndNamePath(select_column_state->resolved_expr.get(),
                                       pre_group_by_column, &source_column,
                                       &unused_is_correlated, &valid_name_path,
                                       id_string_pool_)) {
          // We found a field access path, register it in QueryResolutionInfo.
          query_resolution_info->mutable_select_list_valid_field_info_map()
              ->InsertNamePath(source_column, valid_name_path);
        }
        query_resolution_info
            ->select_list_columns_to_compute_before_aggregation()
            ->push_back(MakeResolvedComputedColumn(
                pre_group_by_column,
                std::move(select_column_state->resolved_expr)));
        // This column reference will be used when resolving the GROUP BY
        // expressions.
        select_column_state->resolved_expr = MakeColumnRef(pre_group_by_column);
        // Keep track of the resolved expr before it gets replaced with a
        // reference to a pre-aggregate compute column.
        select_column_state->original_resolved_expr =
            query_resolution_info
                ->select_list_columns_to_compute_before_aggregation()
                ->back()
                ->expr();
      }
      select_column_state->resolved_pre_group_by_select_column =
          pre_group_by_column;
    }
  }
  return absl::OkStatus();
}

class FindReferencedColumnsVisitor : public ResolvedASTVisitor {
 public:
  explicit FindReferencedColumnsVisitor(
      absl::flat_hash_set<ResolvedColumn>* columns)
      : columns_(columns) {}
  absl::Status DefaultVisit(const ResolvedNode* node) override {
    return node->ChildrenAccept(this);
  }

  absl::Status VisitResolvedColumnRef(const ResolvedColumnRef* node) override {
    columns_->insert(node->column());
    return absl::OkStatus();
  }

 private:
  absl::flat_hash_set<ResolvedColumn>* columns_;
};

absl::Status Resolver::ResolveQualifyExpr(
    const ASTQualify* qualify, const NameScope* having_and_order_by_scope,
    const NameScope* select_list_and_from_scan_scope,
    QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const ResolvedExpr>* resolved_qualify_expr) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  if (!language().LanguageFeatureEnabled(FEATURE_V_1_3_QUALIFY)) {
    return MakeSqlErrorAt(qualify) << "QUALIFY is not supported";
  }

  ExprResolutionInfo expr_resolution_info(
      having_and_order_by_scope, select_list_and_from_scan_scope,
      having_and_order_by_scope,
      /*allows_aggregation_in=*/true, /*allows_analytic_in=*/true,
      query_resolution_info->HasAnalytic(), "QUALIFY clause",
      query_resolution_info);
  ZETASQL_RETURN_IF_ERROR(ResolveExpr(qualify->expression(), &expr_resolution_info,
                              resolved_qualify_expr));

  ZETASQL_RET_CHECK(*resolved_qualify_expr != nullptr);
  ZETASQL_RETURN_IF_ERROR(CoerceExprToBool(qualify->expression(), "QUALIFY clause",
                                   resolved_qualify_expr));

  if (!query_resolution_info->HasAnalytic()) {
    return MakeSqlErrorAt(qualify->expression())
           << "The QUALIFY clause requires analytic function to be present";
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveHavingExpr(
    const ASTHaving* having, const NameScope* having_and_order_by_scope,
    const NameScope* select_list_and_from_scan_scope,
    QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const ResolvedExpr>* resolved_having_expr) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  // Aggregation is only allowed if we already saw GROUP BY or aggregation.
  // In theory, the following is valid, though it's not very interesting:
  //   SELECT 1 from T HAVING sum(T.a) > 5;
  // Return an error for that case.  The only way I can think of how to
  // allow this is to do an initial resolution pass on HAVING to detect
  // aggregations, so that when we resolve the SELECT list above it
  // can detect errors when referencing non-grouped and
  // non-aggregated columns, such as:
  //   SELECT value FROM KeyValue HAVING sum(key) > 5;
  const bool already_saw_group_by_or_aggregation =
      query_resolution_info->HasGroupByOrAggregation();
  ExprResolutionInfo expr_resolution_info(
      having_and_order_by_scope, select_list_and_from_scan_scope,
      having_and_order_by_scope,
      /*allows_aggregation_in=*/true, /*allows_analytic_in=*/false,
      query_resolution_info->HasGroupByOrAggregation(), "HAVING clause",
      query_resolution_info);
  ZETASQL_RETURN_IF_ERROR(ResolveExpr(having->expression(), &expr_resolution_info,
                              resolved_having_expr));

  ZETASQL_RET_CHECK(*resolved_having_expr != nullptr);
  ZETASQL_RETURN_IF_ERROR(CoerceExprToBool(having->expression(), "HAVING clause",
                                   resolved_having_expr));

  if (!already_saw_group_by_or_aggregation &&
      query_resolution_info->HasGroupByOrAggregation()) {
    return MakeSqlErrorAt(having->expression())
           << "The HAVING clause only allows aggregation if GROUP BY or "
              "SELECT list aggregation is present";
  }
  // TODO: Should we move this up above, and simply bail
  // if HAVING is present without GROUP BY or aggregation in the
  // SELECT list?  We end up bailing anyway, but it is sort of nice
  // to detect the corner case and give a more specific error message
  // above.
  if (!query_resolution_info->HasGroupByOrAggregation()) {
    return MakeSqlErrorAt(having->expression())
           << "The HAVING clause requires GROUP BY or aggregation to "
              "be present";
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveOrderByExprs(
    const ASTOrderBy* order_by, const NameScope* having_and_order_by_scope,
    const NameScope* select_list_and_from_scan_scope, bool is_post_distinct,
    QueryResolutionInfo* query_resolution_info) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  // Aggregation is only allowed if we already saw GROUP BY or aggregation.
  // In theory, we could support:
  // SELECT 1 from T ORDER BY sum(T.a) > 5;
  const bool already_saw_group_by_or_aggregation =
      query_resolution_info->HasGroupByOrAggregation();

  // TODO: Clean this up.  This is setting global state, in the
  // context of analyzing the ORDER BY clause.  This is ok because once we
  // get to resolving the ORDER BY expressions, we won't re-resolve an
  // analytic function where named windows are allowed.  Make sure we
  // have test coverage for this:
  // select agg() over (named_window)
  // from table
  // order by agg2() over (partition by a)
  // window named_window (partition by b)
  static const char clause_name[] = "ORDER BY clause";
  query_resolution_info->analytic_resolver()->DisableNamedWindowRefs(
      clause_name);

  // Aggregation is not allowed in ORDER BY if the query is SELECT DISTINCT.
  // Analytic functions are also currently disallowed after SELECT DISTINCT,
  // but could be allowed (it would require a bunch of additional analysis
  // logic though).  TODO: It is low priority but make this work.
  // Maybe wait until we get a feature request.
  ExprResolutionInfo expr_resolution_info(
      having_and_order_by_scope, select_list_and_from_scan_scope,
      having_and_order_by_scope,
      /*allows_aggregation_in=*/!is_post_distinct,
      /*allows_analytic_in=*/!is_post_distinct,
      query_resolution_info->HasGroupByOrAggregation(), clause_name,
      query_resolution_info);

  ZETASQL_RETURN_IF_ERROR(ResolveOrderingExprs(
      order_by->ordering_expressions(), &expr_resolution_info,
      expr_resolution_info.query_resolution_info
          ->mutable_order_by_item_info()));

  ZETASQL_RETURN_IF_ERROR(AddColumnsForOrderByExprs(
      /*query_alias=*/kOrderById,
      query_resolution_info->mutable_order_by_item_info(),
      query_resolution_info->order_by_columns_to_compute()));

  if (!already_saw_group_by_or_aggregation &&
      query_resolution_info->HasGroupByOrAggregation()) {
    // Return an error for now.  The only way I can think of how to
    // allow this is to do an earlier pass over the ORDER BY to detect
    // aggregations, so that the select list can resolve against the
    // post grouped columns only so it can detect an error for a query
    // that looks like:
    //  SELECT value FROM KeyValue ORDER BY sum(key);
    return MakeSqlErrorAt(order_by)
           << "The ORDER BY clause only allows aggregation if GROUP BY or "
              "SELECT list aggregation is present";
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveWhereClauseAndCreateScan(
    const ASTWhereClause* where_clause, const NameScope* from_scan_scope,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  std::unique_ptr<const ResolvedExpr> resolved_where;
  static constexpr char kWhereClause[] = "WHERE clause";
  ZETASQL_RETURN_IF_ERROR(ResolveScalarExpr(where_clause->expression(), from_scan_scope,
                                    kWhereClause, &resolved_where));
  ZETASQL_RETURN_IF_ERROR(CoerceExprToBool(where_clause->expression(), kWhereClause,
                                   &resolved_where));

  std::vector<ResolvedColumn> tmp_column_list = (*current_scan)->column_list();
  *current_scan = MakeResolvedFilterScan(
      tmp_column_list, std::move(*current_scan), std::move(resolved_where));
  return absl::OkStatus();
}

void Resolver::FinalizeSelectColumnStateList(
    const ASTSelectList* ast_select_list, IdString query_alias,
    bool force_new_columns_for_projected_outputs,
    QueryResolutionInfo* query_resolution_info,
    SelectColumnStateList* select_column_state_list) {
  // TODO: Consider renaming SelectColumnStateList to
  // SelectListColumnStateInfo or similar.  It's just weird that we have
  // SelectColumnStateList that has a method select_column_state_list()
  // that returns a vector<SelectColumnsState*>.
  for (const std::unique_ptr<SelectColumnState>& select_column_state :
       select_column_state_list->select_column_state_list()) {
    if (!force_new_columns_for_projected_outputs &&
        select_column_state->resolved_expr->node_kind() ==
            RESOLVED_COLUMN_REF &&
        !select_column_state->resolved_expr->GetAs<ResolvedColumnRef>()
             ->is_correlated() &&
        !analyzer_options_.create_new_column_for_each_projected_output()) {
      // The expression was already resolved to a column.  If it was not
      // correlated, just use the column.
      const ResolvedColumn& select_column =
          select_column_state->resolved_expr->GetAs<ResolvedColumnRef>()
              ->column();
      select_column_state->resolved_select_column = select_column;
    } else {
      ResolvedColumn select_column(
          AllocateColumnId(), query_alias, select_column_state->alias,
          select_column_state->resolved_expr->annotated_type());
      std::unique_ptr<ResolvedComputedColumn> resolved_computed_column =
          MakeResolvedComputedColumn(
              select_column, std::move(select_column_state->resolved_expr));
      select_column_state->resolved_computed_column =
          resolved_computed_column.get();
      // TODO: Also do not include internal aliases, i.e.,
      // !IsInternalAlias(select_column_state->alias).  Do this in a
      // subsequent changelist, as it will impact where/when such columns
      // get PROJECTed.
      query_resolution_info->select_list_columns_to_compute()->push_back(
          std::move(resolved_computed_column));
      select_column_state->resolved_select_column = select_column;
    }
  }
}

IdString Resolver::ComputeSelectColumnAlias(
    const ASTSelectColumn* ast_select_column, int column_idx) const {
  IdString alias;
  if (ast_select_column->alias() != nullptr) {
    alias = ast_select_column->alias()->GetAsIdString();
  } else {
    alias = GetAliasForExpression(ast_select_column->expression());
    if (alias.empty()) {
      // Arbitrary locally unique name.
      alias = MakeIdString(absl::StrCat("$col", column_idx + 1));
    }
  }
  return alias;
}

// This struct represents sets of column names to be excluded or replaced.
// This is used for SELECT * EXCEPT (...) REPLACE (...) syntax.
// See (broken link).
struct ColumnReplacements {
  std::string DebugString() const {
    std::string debug_string;
    absl::StrAppend(&debug_string, "\nexcluded_columns: (",
                    absl::StrJoin(excluded_columns, ",", IdStringFormatter),
                    ")");
    absl::StrAppend(&debug_string, "\nreplaced_columns:");
    for (const auto& replaced_column : replaced_columns) {
      absl::StrAppend(&debug_string, "\n  (",
                      replaced_column.first.ToStringView(), ",",
                      (replaced_column.second == nullptr
                           ? "null"
                           : replaced_column.second->DebugString()),
                      ")");
    }
    return debug_string;
  }

  // Column names that should be skipped.
  IdStringHashSetCase excluded_columns;

  // Column names that should be replaced.  Each should get used only once.
  // This map stores a single SelectColumnState that will be used as a
  // replacement.  When the replacement happens, that object is moved out and
  // the map will store NULL.  Callers should assert that they don't find a
  // NULL in this map.
  IdStringHashMapCase<std::unique_ptr<SelectColumnState>> replaced_columns;
};

// Check if <column_name> should be excluded or replaced according to
// <column_replacements> (which may be NULL).
// Return true if the caller should skip adding this column.
// For replaces, the replacement column will have been added
// to <select_column_state_list>.
static bool ExcludeOrReplaceColumn(
    const ASTExpression* ast_expression, IdString column_name,
    ColumnReplacements* column_replacements,
    SelectColumnStateList* select_column_state_list) {
  if (column_replacements == nullptr) {
    return false;
  }
  if (column_replacements->excluded_columns.contains(column_name)) {
    return true;
  }
  if (zetasql_base::ContainsKey(column_replacements->replaced_columns, column_name)) {
    select_column_state_list->AddSelectColumn(std::move(
        zetasql_base::FindOrDie(column_replacements->replaced_columns, column_name)));
    // I'd use ZETASQL_RET_CHECK here, except then I'd have to return StatusOr<bool>.
    ABSL_DCHECK(select_column_state_list->select_column_state_list().back() !=
           nullptr);
    return true;
  }
  return false;
}

absl::Status Resolver::AddNameListToSelectList(
    const ASTSelectColumn* ast_select_column,
    const std::shared_ptr<const NameList>& name_list,
    const CorrelatedColumnsSetList& correlated_columns_set_list,
    bool ignore_excluded_value_table_fields,
    SelectColumnStateList* select_column_state_list,
    ColumnReplacements* column_replacements) {
  const ASTExpression* ast_expression = ast_select_column->expression();
  const size_t orig_num_columns = select_column_state_list->Size();
  for (const NamedColumn& named_column : name_list->columns()) {
    // Process exclusions first because MakeColumnRef will add columns
    // to referenced_columns_ and then they cannot be pruned.
    if (!named_column.is_value_table_column() &&
        ExcludeOrReplaceColumn(ast_expression, named_column.name(),
                               column_replacements, select_column_state_list)) {
      continue;
    }

    std::unique_ptr<const ResolvedColumnRef> column_ref =
        MakeColumnRefWithCorrelation(named_column.column(),
                                     correlated_columns_set_list);
    if (named_column.is_value_table_column()) {
      // For value tables with fields, SELECT * expands to those fields
      // rather than showing the container value.
      // For scalar-valued value tables, or values with zero fields,
      // we'll just get the value rather than its fields.
      ZETASQL_RET_CHECK(!named_column.name().empty());

      ZETASQL_RETURN_IF_ERROR(AddColumnFieldsToSelectList(
          ast_select_column, column_ref.get(),
          /*src_column_has_aggregation=*/false,
          /*src_column_has_analytic=*/false,
          /*src_column_has_volatile=*/false,
          /*column_alias_if_no_fields=*/named_column.name(),
          (ignore_excluded_value_table_fields
               ? &named_column.excluded_field_names()
               : nullptr),
          select_column_state_list, column_replacements));
    } else {
      select_column_state_list->AddSelectColumn(
          ast_select_column, named_column.name(), named_column.is_explicit(),
          /*has_aggregation=*/false, /*has_analytic=*/false,
          /*has_volatile=*/false, std::move(column_ref));
    }
  }
  // Detect if the * ended up expanding to zero columns after applying EXCEPT,
  // and treat that as an error.
  if (orig_num_columns == select_column_state_list->Size()) {
    ZETASQL_RET_CHECK(column_replacements != nullptr &&
              !column_replacements->excluded_columns.empty());
    return MakeSqlErrorAt(ast_expression)
           << "SELECT * expands to zero columns after applying EXCEPT";
  }
  return absl::OkStatus();
}

// static.
std::string Resolver::ColumnAliasOrPosition(IdString alias, int column_pos) {
  return IsInternalAlias(alias) ? absl::StrCat(1 + column_pos)
                                : alias.ToString();
}

// If 'resolved_expr' is a resolved path expression (zero or more
// RESOLVED_GET_*_FIELD expressions over a ResolvedColumnRef), then insert
// a new entry into 'query_resolution_info->group_by_valid_field_info_map'
// with a source ResolvedColumn that is the 'resolved_expr' source
// ResolvedColumnRef column, the name path derived from the 'resolved_expr'
// get_*_field expressions, along with the 'target_column'.
// If 'resolved_expr' is not a resolved path expression, then this has no
// effect.
absl::Status Resolver::CollectResolvedPathExpressionInfoIfRelevant(
    QueryResolutionInfo* query_resolution_info,
    const ResolvedExpr* resolved_expr, ResolvedColumn target_column) const {
  ResolvedColumn source_column;
  bool unused_is_correlated;
  ValidNamePath valid_name_path;
  if (!GetSourceColumnAndNamePath(resolved_expr, target_column, &source_column,
                                  &unused_is_correlated, &valid_name_path,
                                  id_string_pool_)) {
    return absl::OkStatus();
  }

  // The 'source_column' might itself come from resolving a path expression.
  // If so, then we need to merge the ValidNamePath that produced the
  // 'source_column' with 'valid_name_path'.  This gives us a resulting
  // ValidNamePath that relates the original ValidNamePath source column
  // through a full (concatenated) path name list to the 'target_column'.
  ResolvedColumn new_source_column = source_column;
  for (const auto& entry :
       query_resolution_info->select_list_valid_field_info_map().map()) {
    const ValidNamePathList* select_list_valid_name_path_list =
        entry.second.get();
    ZETASQL_RET_CHECK(select_list_valid_name_path_list != nullptr);
    bool found = false;
    for (const ValidNamePath& select_list_valid_name_path :
         *select_list_valid_name_path_list) {
      if (source_column == select_list_valid_name_path.target_column()) {
        const size_t total_name_path_size =
            valid_name_path.name_path().size() +
            select_list_valid_name_path.name_path().size();
        std::vector<IdString> new_name_path;
        new_name_path.reserve(total_name_path_size);
        valid_name_path.mutable_name_path()->reserve(total_name_path_size);
        new_name_path.insert(new_name_path.end(),
                             select_list_valid_name_path.name_path().begin(),
                             select_list_valid_name_path.name_path().end());
        new_name_path.insert(new_name_path.end(),
                             valid_name_path.name_path().begin(),
                             valid_name_path.name_path().end());
        valid_name_path.set_name_path(new_name_path);
        new_source_column = entry.first;
        found = true;
        break;
      }
    }
    if (found) {
      // There should only be one match, so if we find it then we are
      // done.
      break;
    }
  }
  query_resolution_info->mutable_group_by_valid_field_info_map()
      ->InsertNamePath(new_source_column, valid_name_path);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveSelectDistinct(
    const ASTSelect* select, SelectColumnStateList* select_column_state_list,
    const NameList* input_name_list,
    std::unique_ptr<const ResolvedScan>* current_scan,
    QueryResolutionInfo* query_resolution_info,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  // For DISTINCT processing, we will build and maintain a mapping from
  // SELECT list expressions to post-DISTINCT versions of columns.  This
  // mapping will be used when resolving ORDER BY expression after the
  // DISTINCT.

  // DISTINCT processing will re-use the GROUP BY information, so clear out
  // any old GROUP BY information that may exist.  But first, we must
  // preserve the mapping from SELECT list expressions to post-GROUP BY
  // columns so that we can update it during DISTINCT processing to capture the
  // mapping from SELECT list expressions to post-DISTINCT columns instead.
  query_resolution_info->ClearGroupByInfo();

  ZETASQL_RET_CHECK_EQ(select_column_state_list->Size(),
               input_name_list->num_columns());

  std::shared_ptr<NameList> name_list(new NameList);

  for (int column_pos = 0; column_pos < input_name_list->num_columns();
       ++column_pos) {
    const NamedColumn& named_column = input_name_list->column(column_pos);
    const ResolvedColumn& column = named_column.column();
    SelectColumnState* select_column_state =
        select_column_state_list->GetSelectColumnState(column_pos);
    const ASTNode* ast_column_location = select_column_state->ast_expr;

    std::string no_grouping_type;
    if (!column.type()->SupportsGrouping(language(), &no_grouping_type)) {
      return MakeSqlErrorAt(ast_column_location)
             << "Column "
             << ColumnAliasOrPosition(named_column.name(), column_pos)
             << " of type " << no_grouping_type
             << " cannot be used in SELECT DISTINCT";
    }

    const ResolvedExpr* resolved_expr =
        select_column_state->resolved_expr.get();
    if (resolved_expr == nullptr) {
      ZETASQL_RET_CHECK(select_column_state->resolved_computed_column != nullptr);
      resolved_expr = select_column_state->resolved_computed_column->expr();
    }
    ZETASQL_RET_CHECK(resolved_expr != nullptr);
    const ResolvedExpr* pre_group_by_expr =
        GetPreGroupByResolvedExpr(select_column_state);

    ResolvedColumn distinct_column;
    const ResolvedComputedColumn* existing_computed_column =
        query_resolution_info->GetEquivalentGroupByComputedColumnOrNull(
            resolved_expr);
    if (existing_computed_column == nullptr) {
      // We could not find a computed column that matches the original
      // select column state expression.  Now look for a computed column
      // that matches a reference to <column>, since if a previous SELECT
      // list computed column was created in this method then it was
      // added in the AddGroupByComputedColumnIfNeeded() call below.  We
      // detect that duplicate here so that we can re-use it, and do not
      // need to compute another column for it.
      std::unique_ptr<ResolvedColumnRef> column_ref = MakeColumnRef(column);
      existing_computed_column =
          query_resolution_info->GetEquivalentGroupByComputedColumnOrNull(
              column_ref.get());
    }
    if (existing_computed_column != nullptr) {
      // Reference the existing column rather than recompute the expression.
      distinct_column = existing_computed_column->column();
    } else {
      // Create a new DISTINCT column.
      const IdString* query_alias = &kDistinctId;
      distinct_column =
          ResolvedColumn(AllocateColumnId(), *query_alias, column.name_id(),
                         column.annotated_type());
      // Add a computed column for the new post-DISTINCT column.
      query_resolution_info->AddGroupByComputedColumnIfNeeded(
          distinct_column, MakeColumnRef(column), pre_group_by_expr);
    }

    ZETASQL_RETURN_IF_ERROR(name_list->AddColumn(named_column.name(), distinct_column,
                                         named_column.is_explicit()));
    // Update the SelectListColumnState with the new post-DISTINCT
    // ResolvedColumn information.
    select_column_state->resolved_select_column = distinct_column;

    // Store the mapping of pre-DISTINCT to post-DISTINCT column.
    query_resolution_info->mutable_group_by_valid_field_info_map()
        ->InsertNamePath(column, {/*name_path=*/{}, distinct_column});

    // If the 'resolved_expr' is a path expression, we must collect
    // information in the 'query_resolution_info' about that path
    // expression and its relationship to the 'distinct_column'.
    // This information will get used later when constructing a
    // NameScope for what names are valid post-DISTINCT, where the
    // new NameScope is used when resolving a subsequent ORDER BY.
    // If the 'resolved_expr' is not a path expression then this
    // is a no-op.
    ZETASQL_RETURN_IF_ERROR(CollectResolvedPathExpressionInfoIfRelevant(
        query_resolution_info, resolved_expr, distinct_column));
  }

  *output_name_list = name_list;

  // Set the query resolution context so subsequent ORDER BY expression
  // resolution will know it is in the context of post-DISTINCT processing.
  query_resolution_info->set_is_post_distinct(true);

  return AddAggregateScan(select, /*is_for_select_distinct=*/true,
                          query_resolution_info, current_scan);
}

absl::Status Resolver::ResolveSelectStarModifiers(
    const ASTSelectColumn* ast_select_column, const ASTStarModifiers* modifiers,
    const NameList* name_list_for_star, const Type* type_for_star,
    const NameScope* scope, QueryResolutionInfo* query_resolution_info,
    ColumnReplacements* column_replacements) {
  ZETASQL_RET_CHECK(name_list_for_star != nullptr || type_for_star != nullptr);
  ZETASQL_RET_CHECK(name_list_for_star == nullptr || type_for_star == nullptr);
  ZETASQL_RET_CHECK(modifiers != nullptr);

  const ASTStarExceptList* except_list = modifiers->except_list();
  const absl::Span<const ASTStarReplaceItem* const>& replace_items =
      modifiers->replace_items();

  if (!language().LanguageFeatureEnabled(
          FEATURE_V_1_1_SELECT_STAR_EXCEPT_REPLACE)) {
    if (except_list != nullptr) {
      return MakeSqlErrorAt(ast_select_column)
             << "SELECT * EXCEPT is not supported";
    } else {
      return MakeSqlErrorAt(ast_select_column)
             << "SELECT * REPLACE is not supported";
    }
  }

  if (except_list != nullptr) {
    for (const ASTIdentifier* ast_identifier : except_list->identifiers()) {
      const IdString identifier = ast_identifier->GetAsIdString();
      if (IsInternalAlias(identifier)) {
        return MakeSqlErrorAt(ast_identifier)
               << "Cannot use EXCEPT with internal alias "
               << ToIdentifierLiteral(identifier);
      }
      const Type::HasFieldResult has_field =
          name_list_for_star != nullptr
              ? name_list_for_star->SelectStarHasColumn(identifier)
              : type_for_star->HasField(identifier.ToString(),
                                        /*field_id=*/nullptr,
                                        /*include_pseudo_fields=*/false);
      switch (has_field) {
        case Type::HAS_NO_FIELD:
          return MakeSqlErrorAt(ast_identifier)
                 << "Column " << ToIdentifierLiteral(identifier)
                 << " in SELECT * EXCEPT list does not exist";
        case Type::HAS_FIELD:
        case Type::HAS_AMBIGUOUS_FIELD:  // Duplicate columns ok for EXCEPT.
          break;
        case Type::HAS_PSEUDO_FIELD:
          // SelectStarHasColumn can never return HAS_PSEUDO_FIELD.
          // HasField with include_pseudo_fields=false can never return
          // HAS_PSEUDO_FIELD either.
          ZETASQL_RET_CHECK_FAIL() << "Unexpected Type::HAS_PSEUDO_FIELD value";
          break;
      }
      if (!zetasql_base::InsertIfNotPresent(&column_replacements->excluded_columns,
                                   identifier)) {
        return MakeSqlErrorAt(ast_identifier)
               << "Duplicate column " << ToIdentifierLiteral(identifier)
               << " in SELECT * EXCEPT list";
      }
    }
  }
  for (const ASTStarReplaceItem* ast_replace_item : replace_items) {
    ExprResolutionInfo expr_resolution_info(
        scope, query_resolution_info, ast_replace_item->expression(),
        ast_replace_item->alias()->GetAsIdString());
    std::unique_ptr<const ResolvedExpr> resolved_expr;
    ZETASQL_RETURN_IF_ERROR(ResolveExpr(ast_replace_item->expression(),
                                &expr_resolution_info, &resolved_expr));
    if (expr_resolution_info.has_analytic) {
      // TODO This is disallowed only because it doesn't work in
      // the current implementation, because of a problem that occurs later.
      return MakeSqlErrorAt(ast_replace_item->expression())
             << "Cannot use analytic functions inside SELECT * REPLACE";
    }

    const IdString identifier = ast_replace_item->alias()->GetAsIdString();
    if (IsInternalAlias(identifier)) {
      return MakeSqlErrorAt(ast_replace_item)
             << "Cannot use REPLACE with internal alias "
             << ToIdentifierLiteral(identifier);
    }
    if (column_replacements->excluded_columns.contains(identifier)) {
      return MakeSqlErrorAt(ast_replace_item->alias())
             << "Column " << ToIdentifierLiteral(identifier)
             << " cannot occur in both SELECT * EXCEPT and REPLACE";
    }
    const Type::HasFieldResult has_field =
        name_list_for_star != nullptr
            ? name_list_for_star->SelectStarHasColumn(identifier)
            : type_for_star->HasField(identifier.ToString(),
                                      /*field_id=*/nullptr,
                                      /*include_pseudo_fields=*/false);
    switch (has_field) {
      case Type::HAS_NO_FIELD:
        return MakeSqlErrorAt(ast_replace_item->alias())
               << "Column " << ToIdentifierLiteral(identifier)
               << " in SELECT * REPLACE list does not exist";
      case Type::HAS_FIELD:
        break;
      case Type::HAS_AMBIGUOUS_FIELD:
        return MakeSqlErrorAt(ast_replace_item->alias())
               << "Column " << ToIdentifierLiteral(identifier)
               << " in SELECT * REPLACE list is ambiguous";
      case Type::HAS_PSEUDO_FIELD:
        // SelectStarHasColumn can never return HAS_PSEUDO_FIELD.
        // HasField with include_pseudo_fields=false can never return
        // HAS_PSEUDO_FIELD either.
        ZETASQL_RET_CHECK_FAIL() << "Unexpected Type::HAS_PSEUDO_FIELD value";
    }

    auto select_column_state = std::make_unique<SelectColumnState>(
        ast_select_column, identifier,
        /*is_explicit=*/true, expr_resolution_info.has_aggregation,
        expr_resolution_info.has_analytic, expr_resolution_info.has_volatile,
        std::move(resolved_expr));

    // Override the ast_expr to point at the replacement expression
    // rather than `ast_select_column`, which points at the star expression.
    select_column_state->ast_expr = ast_replace_item->expression();

    if (!column_replacements->replaced_columns
             .emplace(identifier, std::move(select_column_state))
             .second) {
      return MakeSqlErrorAt(ast_replace_item->alias())
             << "Duplicate column " << ToIdentifierLiteral(identifier)
             << " in SELECT * REPLACE list";
    }
  }

  return absl::OkStatus();
}

// NOTE: The behavior of star expansion here must match
// NameList::SelectStarHasColumn.
absl::Status Resolver::ResolveSelectStar(
    const ASTSelectColumn* ast_select_column,
    const std::shared_ptr<const NameList>& from_clause_name_list,
    const NameScope* from_scan_scope,
    QueryResolutionInfo* query_resolution_info) {
  const ASTExpression* ast_select_expr = ast_select_column->expression();

  std::string clause_name = "SELECT";

  if (in_strict_mode()) {
    return MakeSqlErrorAt(ast_select_expr) << absl::StrCat(
               clause_name, " * is not allowed in strict name resolution mode");
  }
  if (!query_resolution_info->SelectFormAllowsSelectStar()) {
    return MakeSqlErrorAt(ast_select_expr)
           << absl::StrCat(clause_name, " * must have a FROM clause");
  }
  if (from_clause_name_list->num_columns() == 0) {
    return MakeSqlErrorAt(ast_select_expr)
           << absl::StrCat(clause_name, " * would expand to zero columns");
  }

  // Process SELECT * EXCEPT(...) REPLACE(...) if present.
  ColumnReplacements column_replacements;
  if (ast_select_expr->node_kind() == AST_STAR_WITH_MODIFIERS) {
    const ASTStarWithModifiers* ast_node =
        ast_select_expr->GetAsOrDie<ASTStarWithModifiers>();
    ZETASQL_RETURN_IF_ERROR(ResolveSelectStarModifiers(
        ast_select_column, ast_node->modifiers(), from_clause_name_list.get(),
        /*type_for_star=*/nullptr, from_scan_scope, query_resolution_info,
        &column_replacements));
  }

  const CorrelatedColumnsSetList correlated_columns_set_list;
  ZETASQL_RETURN_IF_ERROR(AddNameListToSelectList(
      ast_select_column, from_clause_name_list, correlated_columns_set_list,
      /*ignore_excluded_value_table_fields=*/true,
      query_resolution_info->select_column_state_list(), &column_replacements));

  return absl::OkStatus();
}

static absl::Status MakeErrorIfTypeDotStarHasNoFields(
    const ASTNode* ast_location, const Type* type, ProductMode product_mode) {
  if (!type->HasAnyFields()) {
    if (type->IsStruct()) {
      return MakeSqlErrorAt(ast_location)
             << "Star expansion is not allowed on a struct with zero fields";
    } else if (type->IsProto()) {
      return MakeSqlErrorAt(ast_location)
             << "Star expansion is not allowed on proto "
             << type->AsProto()->descriptor()->full_name()
             << " which has zero fields";
    }
    return MakeSqlErrorAt(ast_location) << "Dot-star is not supported for type "
                                        << type->ShortTypeName(product_mode);
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveSelectDotStar(
    const ASTSelectColumn* ast_select_column, const NameScope* from_scan_scope,
    QueryResolutionInfo* query_resolution_info) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  const ASTExpression* ast_dotstar = ast_select_column->expression();
  const ASTExpression* ast_expr;
  const ASTStarModifiers* ast_modifiers = nullptr;
  if (ast_dotstar->node_kind() == AST_DOT_STAR) {
    ast_expr = ast_dotstar->GetAsOrDie<ASTDotStar>()->expr();
  } else {
    ZETASQL_RET_CHECK_EQ(ast_dotstar->node_kind(), AST_DOT_STAR_WITH_MODIFIERS);
    const ASTDotStarWithModifiers* ast_with_modifiers =
        ast_dotstar->GetAsOrDie<ASTDotStarWithModifiers>();
    ast_expr = ast_with_modifiers->expr();
    ast_modifiers = ast_with_modifiers->modifiers();
  }

  if (in_strict_mode()) {
    return MakeSqlErrorAt(ast_dotstar)
           << "Dot-star is not allowed in strict name resolution mode";
  }

  // If DotStar expression has exactly one identifier and resolves to a range
  // variable, add the scan columns directly to the select_column_state_list.
  // For anything else, we expect to resolve the lhs as a value that should
  // have type struct or proto.
  // Value table range variables are excluded here because we want to resolve
  // it to a value first and then expand its fields, if possible.
  if (ast_expr->node_kind() == AST_PATH_EXPRESSION) {
    const ASTPathExpression* path_expr =
        ast_expr->GetAsOrDie<ASTPathExpression>();

    if (path_expr->num_names() == 1) {
      NameTarget target;
      CorrelatedColumnsSetList correlated_columns_set_list;
      if (from_scan_scope->LookupName(path_expr->first_name()->GetAsIdString(),
                                      &target, &correlated_columns_set_list) &&
          target.kind() == NameTarget::RANGE_VARIABLE &&
          !target.scan_columns()->is_value_table()) {
        if (target.scan_columns()->num_columns() == 0) {
          return MakeSqlErrorAt(path_expr)
                 << "Dot-star would expand to zero columns";
        }

        // Process .* EXCEPT(...) REPLACE(...) if present.
        ColumnReplacements column_replacements;
        if (ast_modifiers != nullptr) {
          ZETASQL_RETURN_IF_ERROR(ResolveSelectStarModifiers(
              ast_select_column, ast_modifiers, target.scan_columns().get(),
              /*type_for_star=*/nullptr, from_scan_scope, query_resolution_info,
              &column_replacements));
        }

        ZETASQL_RETURN_IF_ERROR(AddNameListToSelectList(
            ast_select_column, target.scan_columns(),
            correlated_columns_set_list,
            /*ignore_excluded_value_table_fields=*/false,
            query_resolution_info->select_column_state_list(),
            &column_replacements));

        return absl::OkStatus();
      }
    }
  }

  std::unique_ptr<const ResolvedExpr> resolved_dotstar_expr;
  ExprResolutionInfo expr_resolution_info(from_scan_scope,
                                          query_resolution_info);
  ZETASQL_RETURN_IF_ERROR(
      ResolveExpr(ast_expr, &expr_resolution_info, &resolved_dotstar_expr));
  const Type* source_type = resolved_dotstar_expr->type();

  std::unique_ptr<const ResolvedColumnRef> src_column_ref;
  if (resolved_dotstar_expr->node_kind() == RESOLVED_COLUMN_REF &&
      !resolved_dotstar_expr->GetAs<ResolvedColumnRef>()->is_correlated()) {
    src_column_ref.reset(
        resolved_dotstar_expr.release()->GetAs<ResolvedColumnRef>());
    if (expr_resolution_info.has_analytic) {
      query_resolution_info
          ->dot_star_columns_with_analytic_for_second_pass_resolution()
          ->emplace_back(src_column_ref->column(), ast_expr);
    }
  } else {
    // We resolved the DotStar to be derived from an expression.
    const ResolvedColumn src_column(
        AllocateColumnId(), kPreProjectId,
        resolved_dotstar_expr->type()->IsStruct() ? kStructId : kProtoId,
        resolved_dotstar_expr->annotated_type());

    if (expr_resolution_info.has_analytic) {
      // The DotStar source expression contains analytic functions (and maybe
      // aggregate functions too), so we need to compute this expression after
      // the analytic scan, but before the final project of the SELECT (since
      // the final project of the SELECT will extract all the fields/columns
      // from this source expression).
      query_resolution_info
          ->dot_star_columns_with_analytic_for_second_pass_resolution()
          ->emplace_back(src_column, ast_expr);
    } else if (expr_resolution_info.has_aggregation) {
      // The DotStar source expression contains aggregation (but not analytic)
      // functions, so we need to compute this expression after the aggregation,
      // but before the final project of the SELECT (since the final project of
      // the SELECT will extract all the fields/columns from this source
      // expression).
      query_resolution_info
          ->dot_star_columns_with_aggregation_for_second_pass_resolution()
          ->emplace_back(src_column, ast_expr);
    } else {
      // The dot-star source expression contains neither analytic functions
      // nor aggregate functions, so it must be computed before any aggregation
      // that might be present (the dot-star columns are effectively
      // pre-GROUP BY columns).

      // However, if this dot-star source expression is in DML returning clause,
      // we do not support pre-GROUP BY columns with Project Scan. (b/207519939)
      if (query_resolution_info->is_resolving_returning_clause()) {
        // PROJECT scan for dot-star columns in DML THEN RETURN are not
        // supported.
        return MakeSqlErrorAt(ast_dotstar)
               << "Dot-star is only allowed on range variables and columns in "
                  "THEN RETURN. It cannot be applied on other expressions "
                  "including field access.";
      }

      query_resolution_info->select_list_columns_to_compute_before_aggregation()
          ->emplace_back(MakeResolvedComputedColumn(
              src_column, std::move(resolved_dotstar_expr)));
    }
    src_column_ref = MakeColumnRef(src_column);
  }
  ZETASQL_RET_CHECK(src_column_ref != nullptr);

  ZETASQL_RETURN_IF_ERROR(MakeErrorIfTypeDotStarHasNoFields(ast_dotstar, source_type,
                                                    product_mode()));

  // Process .* EXCEPT(...) REPLACE(...) if present.
  ColumnReplacements column_replacements;
  if (ast_modifiers != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveSelectStarModifiers(
        ast_select_column, ast_modifiers, /*name_list_for_star=*/nullptr,
        source_type, from_scan_scope, query_resolution_info,
        &column_replacements));
  }

  const int orig_num_columns = static_cast<int>(
      query_resolution_info->select_column_state_list()->Size());
  ZETASQL_RETURN_IF_ERROR(AddColumnFieldsToSelectList(
      ast_select_column, src_column_ref.get(),
      expr_resolution_info.has_aggregation, expr_resolution_info.has_analytic,
      expr_resolution_info.has_volatile,
      /*column_alias_if_no_fields=*/IdString(),
      /*excluded_field_names=*/nullptr,
      query_resolution_info->select_column_state_list(), &column_replacements));

  // Detect if the * ended up expanding to zero columns after applying EXCEPT,
  // and treat that as an error.
  if (orig_num_columns ==
      query_resolution_info->select_column_state_list()->Size()) {
    ZETASQL_RET_CHECK(!column_replacements.excluded_columns.empty());
    return MakeSqlErrorAt(ast_dotstar)
           << "SELECT * expands to zero columns after applying EXCEPT";
  }

  return absl::OkStatus();
}

// NOTE: The behavior of star expansion here must match
// NameList::SelectStarHasColumn.
absl::Status Resolver::AddColumnFieldsToSelectList(
    const ASTSelectColumn* ast_select_column,
    const ResolvedColumnRef* src_column_ref, bool src_column_has_aggregation,
    bool src_column_has_analytic, bool src_column_has_volatile,
    IdString column_alias_if_no_fields,
    const IdStringSetCase* excluded_field_names,
    SelectColumnStateList* select_column_state_list,
    ColumnReplacements* column_replacements) {
  const ASTExpression* ast_expression = ast_select_column->expression();
  const bool allow_no_fields = !column_alias_if_no_fields.empty();
  const Type* type = src_column_ref->type();

  // Check if the value has no fields because it either has scalar type
  // or is a compound type with zero fields.
  // Value table columns with no fields will expand in SELECT * to the
  // value itself rather than to an empty list of fields.
  if (!type->HasAnyFields()) {
    if (!allow_no_fields) {
      ZETASQL_RETURN_IF_ERROR(MakeErrorIfTypeDotStarHasNoFields(ast_expression, type,
                                                        product_mode()));
    }

    if (ExcludeOrReplaceColumn(ast_expression, column_alias_if_no_fields,
                               column_replacements, select_column_state_list)) {
      return absl::OkStatus();
    }

    // The value doesn't have any fields, but that is allowed here.
    // Just add a ColumnRef directly.
    // is_explicit=false because the column is coming from SELECT *, even if
    // we had an explicit alias for the table.
    // This is not a strict requirement and we could change it.
    select_column_state_list->AddSelectColumn(
        ast_select_column, column_alias_if_no_fields, /*is_explicit=*/false,
        src_column_has_aggregation, src_column_has_analytic,
        src_column_has_volatile, CopyColumnRef(src_column_ref));
    return absl::OkStatus();
  }

  if (type->IsStruct()) {
    const StructType* struct_type = type->AsStruct();

    for (int field_idx = 0; field_idx < struct_type->num_fields();
         ++field_idx) {
      const auto& field = struct_type->field(field_idx);
      const IdString field_name = MakeIdString(
          (field.name.empty()) ? absl::StrCat("$field", 1 + field_idx)
                               : field.name);

      if ((excluded_field_names != nullptr &&
           zetasql_base::ContainsKey(*excluded_field_names, field_name)) ||
          ExcludeOrReplaceColumn(ast_expression, field_name,
                                 column_replacements,
                                 select_column_state_list)) {
        continue;
      }

      auto get_struct_field = MakeResolvedGetStructField(
          field.type, CopyColumnRef(src_column_ref), field_idx);
      ZETASQL_RETURN_IF_ERROR(CheckAndPropagateAnnotations(
          /*error_node=*/nullptr, get_struct_field.get()));
      // is_explicit=false because we're extracting all fields of a struct.
      select_column_state_list->AddSelectColumn(
          ast_select_column, field_name,
          /*is_explicit=*/false, src_column_has_aggregation,
          src_column_has_analytic, src_column_has_volatile,
          std::move(get_struct_field));
    }
  } else {
    const ProtoType* proto_type = type->AsProto();
    const google::protobuf::Descriptor* proto_descriptor = proto_type->descriptor();

    std::map<int32_t, const google::protobuf::FieldDescriptor*>
        tag_number_ordered_field_map;
    for (int proto_idx = 0; proto_idx < proto_descriptor->field_count();
         ++proto_idx) {
      const google::protobuf::FieldDescriptor* field = proto_descriptor->field(proto_idx);
      const IdString field_name = MakeIdString(field->name());
      if (excluded_field_names != nullptr &&
          zetasql_base::ContainsKey(*excluded_field_names, field_name)) {
        continue;
      }
      ZETASQL_RET_CHECK(
          zetasql_base::InsertIfNotPresent(&tag_number_ordered_field_map,
                                  std::make_pair(field->number(), field)));
    }

    for (const auto& entry : tag_number_ordered_field_map) {
      const google::protobuf::FieldDescriptor* field = entry.second;

      const IdString field_name = MakeIdString(field->name());
      if (ExcludeOrReplaceColumn(ast_expression, field_name,
                                 column_replacements,
                                 select_column_state_list)) {
        continue;
      }

      const Type* field_type;
      Value default_value;
      ZETASQL_RETURN_IF_ERROR(
          GetProtoFieldTypeAndDefault(
              ProtoFieldDefaultOptions::FromFieldAndLanguage(field, language()),
              field, proto_type->CatalogNamePath(), type_factory_, &field_type,
              &default_value))
          .With(LocationOverride(ast_expression));
      // TODO: This really should be check for
      // !field_type->IsSupportedType(language())
      // but that breaks existing tests :(
      if (field_type->UsingFeatureV12CivilTimeType() &&
          !language().LanguageFeatureEnabled(FEATURE_V_1_2_CIVIL_TIME)) {
        return MakeSqlErrorAt(ast_expression)
               << "Dot-star expansion includes field " << field_name
               << " with unsupported type "
               << field_type->ShortTypeName(language().product_mode());
      }
      std::unique_ptr<const ResolvedExpr> resolved_expr =
          MakeResolvedGetProtoField(
              field_type, CopyColumnRef(src_column_ref), field, default_value,
              /*get_has_bit=*/false, ProtoType::GetFormatAnnotation(field),
              /*return_default_value_when_unset=*/false);
      // is_explicit=false because we're extracting all fields of a proto.
      select_column_state_list->AddSelectColumn(
          ast_select_column, field_name, /*is_explicit=*/false,
          src_column_has_aggregation, src_column_has_analytic,
          src_column_has_volatile, std::move(resolved_expr));
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveSelectColumnFirstPass(
    const ASTSelectColumn* ast_select_column, const NameScope* from_scan_scope,
    const std::shared_ptr<const NameList>& from_clause_name_list,
    int ast_select_column_idx, QueryResolutionInfo* query_resolution_info,
    const Type* inferred_type) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  const ASTExpression* ast_select_expr = ast_select_column->expression();
  switch (ast_select_expr->node_kind()) {
    case AST_STAR:
    case AST_STAR_WITH_MODIFIERS:
      return ResolveSelectStar(ast_select_column, from_clause_name_list,
                               from_scan_scope, query_resolution_info);
    case AST_DOT_STAR:
    case AST_DOT_STAR_WITH_MODIFIERS:
      return ResolveSelectDotStar(ast_select_column, from_scan_scope,
                                  query_resolution_info);
    default:
      break;
  }

  IdString select_column_alias =
      ComputeSelectColumnAlias(ast_select_column, ast_select_column_idx);
  // Save stack space for nested SELECT list subqueries.
  std::unique_ptr<ExprResolutionInfo> expr_resolution_info;
    expr_resolution_info = std::make_unique<ExprResolutionInfo>(
        from_scan_scope, query_resolution_info, ast_select_expr,
        select_column_alias);

  std::unique_ptr<const ResolvedExpr> resolved_expr;
  ZETASQL_RETURN_IF_ERROR(ResolveExpr(ast_select_expr, expr_resolution_info.get(),
                              &resolved_expr, inferred_type));

  // We can set is_explicit=true unconditionally because this either came
  // from an AS alias or from a path in the query, or it's an internal name
  // for an anonymous column (that can't be looked up).
  query_resolution_info->select_column_state_list()->AddSelectColumn(
      ast_select_column, select_column_alias, /*is_explicit=*/true,
      expr_resolution_info->has_aggregation, expr_resolution_info->has_analytic,
      expr_resolution_info->has_volatile, std::move(resolved_expr));

  return absl::OkStatus();
}

absl::Status Resolver::ResolveSelectListExprsFirstPass(
    const ASTSelectList* select_list, const NameScope* from_scan_scope,
    const std::shared_ptr<const NameList>& from_clause_name_list,
    QueryResolutionInfo* query_resolution_info,
    const Type* inferred_type_for_query) {
  for (int i = 0; i < select_list->columns().size(); ++i) {
    ZETASQL_RETURN_IF_ERROR(ResolveSelectColumnFirstPass(
        select_list->columns(i), from_scan_scope, from_clause_name_list, i,
        query_resolution_info, inferred_type_for_query));
  }
  return absl::OkStatus();
}

absl::Status Resolver::ValidateAndResolveOrderByCollate(
    const ASTCollate* ast_collate, const ASTNode* ast_order_by_item_location,
    const Type* order_by_item_column,
    std::unique_ptr<const ResolvedExpr>* resolved_collate) {
  if (!language().LanguageFeatureEnabled(FEATURE_V_1_1_ORDER_BY_COLLATE)) {
    return MakeSqlErrorAt(ast_collate) << "COLLATE is not supported";
  }
  if (!order_by_item_column->IsString()) {
    return MakeSqlErrorAt(ast_order_by_item_location)
           << "COLLATE can only be applied to expressions of type "
              "STRING, but was applied to "
           << order_by_item_column->ShortTypeName(product_mode());
  }
  return ResolveCollate(ast_collate, resolved_collate);
}

absl::StatusOr<ResolvedOrderByItemEnums::NullOrderMode>
Resolver::ResolveNullOrderMode(const ASTNullOrder* null_order) {
  if (null_order == nullptr) {
    return ResolvedOrderByItemEnums::ORDER_UNSPECIFIED;
  }
  if (!language().LanguageFeatureEnabled(
          FEATURE_V_1_3_NULLS_FIRST_LAST_IN_ORDER_BY)) {
    return MakeSqlErrorAt(null_order)
           << "NULLS FIRST and NULLS LAST are not supported";
  }
  return null_order->nulls_first() ? ResolvedOrderByItemEnums::NULLS_FIRST
                                   : ResolvedOrderByItemEnums::NULLS_LAST;
}

absl::Status Resolver::ResolveOrderingExprs(
    const absl::Span<const ASTOrderingExpression* const> ordering_expressions,
    ExprResolutionInfo* expr_resolution_info,
    std::vector<OrderByItemInfo>* order_by_info) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  for (const ASTOrderingExpression* order_by_expression :
       ordering_expressions) {
    ZETASQL_ASSIGN_OR_RETURN(ResolvedOrderByItemEnums::NullOrderMode null_order,
                     ResolveNullOrderMode(order_by_expression->null_order()));

    std::unique_ptr<const ResolvedExpr> resolved_order_expression;
    ZETASQL_RETURN_IF_ERROR(ResolveExpr(order_by_expression->expression(),
                                expr_resolution_info,
                                &resolved_order_expression));

    // If the expression was an integer literal, remember that mapping.
    if (resolved_order_expression->node_kind() == RESOLVED_LITERAL &&
        !resolved_order_expression->GetAs<ResolvedLiteral>()
             ->has_explicit_type()) {
      const Value& value =
          resolved_order_expression->GetAs<ResolvedLiteral>()->value();
      if (value.type_kind() == TYPE_INT64 && !value.is_null()) {
        if (value.int64_value() < 1) {
          return MakeSqlErrorAt(order_by_expression)
                 << "ORDER BY column number item is out of range. "
                 << "Column numbers must be greater than or equal to one. "
                 << "Found : " << value.int64_value();
        }
        const int64_t int_value = value.int64_value() - 1;  // Make it 0-based.
        order_by_info->emplace_back(
            order_by_expression, order_by_expression->collate(), int_value,
            order_by_expression->descending(), null_order);
      } else {
        return MakeSqlErrorAt(order_by_expression)
               << "Cannot ORDER BY literal values";
      }
      resolved_order_expression.reset();  // No longer needed.
    } else {
      order_by_info->emplace_back(
          order_by_expression, order_by_expression->collate(),
          std::move(resolved_order_expression),
          order_by_expression->descending(), null_order);
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::HandleGroupBySelectColumn(
    const SelectColumnState* group_by_column_state,
    QueryResolutionInfo* query_resolution_info,
    std::unique_ptr<const ResolvedExpr>* resolved_expr,
    const ResolvedExpr** pre_group_by_expr, ResolvedColumn* group_by_column) {

  // If this SELECT list column is already being grouped by then we should
  // not be calling this.
  ZETASQL_RET_CHECK(!group_by_column_state->is_group_by_column);

  // We are grouping by either a SELECT list ordinal or alias so we must
  // update the SelectColumnState to reflect it is being grouped by.
  // We need a mutable version of the SelectColumnState since we will
  // be updating its expression and other information.
  SelectColumnState* select_column_state =
      const_cast<SelectColumnState*>(group_by_column_state);

  // Move the expression from the SelectColumnState to the
  // group_by_columns list, and update the SelectColumnState to reference
  // the associated group by column.
  ZETASQL_RET_CHECK(select_column_state->resolved_expr != nullptr)
      << select_column_state->DebugString();

  const ResolvedComputedColumn* existing_computed_column =
      query_resolution_info->GetEquivalentGroupByComputedColumnOrNull(
          select_column_state->resolved_expr.get());
  if (existing_computed_column != nullptr) {
    // Make a reference to the existing column rather than recomputing the
    // expression.
    *group_by_column = existing_computed_column->column();
  } else {
    const IdString* query_alias = &kGroupById;
    *group_by_column = ResolvedColumn(
        AllocateColumnId(), *query_alias, select_column_state->alias,
        select_column_state->resolved_expr->annotated_type());
  }

  *pre_group_by_expr = GetPreGroupByResolvedExpr(select_column_state);
  *resolved_expr = std::move(select_column_state->resolved_expr);
  select_column_state->resolved_expr = MakeColumnRef(*group_by_column);
  select_column_state->is_group_by_column = true;
  // Update the SelectColumnState to reflect the grouped by version of
  // the column.
  select_column_state->resolved_select_column = *group_by_column;

  // If the 'resolved_expr' is a path expression, we must collect
  // information in the 'query_resolution_info' about that path
  // expression and its relationship to the 'group_by_column'.
  // This information will get used later when constructing a
  // NameScope for what names are valid post-GROUP BY, where the
  // new NameScope is used when resolving subsequent expressions.
  // If the 'resolved_expr' is not a path expression then this
  // is a no-op.
  ZETASQL_RETURN_IF_ERROR(CollectResolvedPathExpressionInfoIfRelevant(
      query_resolution_info, resolved_expr->get(), *group_by_column));

  return absl::OkStatus();
}

absl::Status Resolver::HandleGroupByExpression(
    const ASTExpression* ast_group_by_expr,
    QueryResolutionInfo* query_resolution_info, IdString alias,
    std::unique_ptr<const ResolvedExpr>* resolved_expr,
    const ResolvedExpr** pre_group_by_expr, ResolvedColumn* group_by_column) {
  // We're grouping by an expression that was not a SELECT list alias
  // or ordinal.
  ZETASQL_RET_CHECK(resolved_expr != nullptr && (*resolved_expr) != nullptr);

  if ((*resolved_expr)->node_kind() == RESOLVED_LITERAL &&
      !(*resolved_expr)->GetAs<ResolvedLiteral>()->has_explicit_type()) {
    return MakeSqlErrorAt(ast_group_by_expr)
           << "Cannot GROUP BY literal values";
  }

  // This expression might match one that is already going to be
  // precomputed before the GROUP BY.  If so, then set <group_by_column>
  // to it and update the expression to be a simple column reference.
  // For instance, consider:
  //   SELECT k.col1
  //   FROM valuetable k
  //   GROUP BY k.col1
  // This code is needed to detect that the GROUP BY k.col1 expression
  // is the same thing as the k.col1 that we are precomputing before the
  // GROUP BY, so we can re-use the same column and avoid an additional
  // expression evaluation.
  bool found_precomputed_expression = false;
  // Expressions in select_list_columns_to_compute_before_aggregation are
  // pre-group-by expressions, so we can set its value to pre_group_by_expr once
  // a precomputed expression is found.
  const ResolvedExpr* precomputed_expr = nullptr;
  for (const std::unique_ptr<const ResolvedComputedColumn>& computed_column :
       *query_resolution_info
            ->select_list_columns_to_compute_before_aggregation()) {
    ZETASQL_ASSIGN_OR_RETURN(bool is_same_expr,
                     IsSameExpressionForGroupBy(resolved_expr->get(),
                                                computed_column->expr()));
    if (is_same_expr) {
      *group_by_column = computed_column->column();
      precomputed_expr = computed_column->expr();
      found_precomputed_expression = true;
      break;
    }
  }
  if (found_precomputed_expression) {
    *resolved_expr = MakeColumnRef(*group_by_column);
    *pre_group_by_expr = precomputed_expr;
  }
  const ResolvedComputedColumn* existing_computed_column =
      query_resolution_info->GetEquivalentGroupByComputedColumnOrNull(
          (*resolved_expr).get());
  if (existing_computed_column != nullptr) {
    // Make a reference to the existing column rather than recomputing the
    // expression.
    *group_by_column = existing_computed_column->column();
  } else {
    const IdString* query_alias = &kGroupById;
    *group_by_column = ResolvedColumn(AllocateColumnId(), *query_alias, alias,
                                      (*resolved_expr)->annotated_type());
  }

  // If the 'resolved_expr' is a path expression, we must collect
  // information in the 'query_resolution_info' about that path
  // expression and its relationship to the 'group_by_column'.
  // This information will get used later when constructing a
  // NameScope for what names are valid post-GROUP BY, where the
  // new NameScope is used when resolving subsequent expressions.
  // If the 'resolved_expr' is not a path expression then this
  // is a no-op.
  ZETASQL_RETURN_IF_ERROR(CollectResolvedPathExpressionInfoIfRelevant(
      query_resolution_info, resolved_expr->get(), *group_by_column));

  return absl::OkStatus();
}

absl::Status Resolver::AddSelectColumnToGroupByAllComputedColumn(
    const SelectColumnState* select_column_state,
    QueryResolutionInfo* query_resolution_info) {
  auto column_desc = [select_column_state]() {
    if (IsInternalAlias(select_column_state->alias)) {
      return absl::StrCat("Column in position ",
                          select_column_state->select_list_position + 1);
    } else {
      return absl::StrCat("Column `", select_column_state->alias.ToStringView(),
                          "`");
    }
  };

  // Check if the type of the group by column contains volatile function.
  if (select_column_state->has_volatile) {
    return MakeSqlErrorAt(select_column_state->ast_expr)
           << column_desc() << ", which is included in the grouping list by "
           << "GROUP BY ALL, contains a volatile expression which must be "
           << "explicitly listed as a group by key. To include this expression "
           << "in GROUP BY, explicitly enumerate group by columns";
  }

  // Resolve group by expression to a post group by column.
  std::unique_ptr<const ResolvedExpr> group_by_expr;
  const ResolvedExpr* pre_group_by_expr = nullptr;
  ResolvedColumn group_by_column;
  ZETASQL_RETURN_IF_ERROR(HandleGroupBySelectColumn(
      select_column_state, query_resolution_info, &group_by_expr,
      &pre_group_by_expr, &group_by_column));
  ZETASQL_RET_CHECK(group_by_expr != nullptr);
  ZETASQL_RET_CHECK(pre_group_by_expr != nullptr);

  // Check if the type of the group by column is groupable.
  std::string no_grouping_type;
  if (!group_by_expr->type()->SupportsGrouping(language(), &no_grouping_type)) {
    return MakeSqlErrorAt(select_column_state->ast_expr)
           << column_desc() << ", which is included in the grouping list by "
           << "GROUP BY ALL, has type " << no_grouping_type
           << " which cannot be used in a grouping key";
  }

  query_resolution_info->AddGroupByComputedColumnIfNeeded(
      group_by_column, std::move(group_by_expr), pre_group_by_expr);
  return absl::OkStatus();
}

struct PathExpressionSelectItemForGroupBy {
  PathExpressionSelectItemForGroupBy(const ResolvedColumn& source_column,
                                     const ValidNamePath& field_path, int index)
      : source_column(source_column),
        field_path(field_path),
        select_list_index(index) {}
  ResolvedColumn source_column;
  ValidNamePath field_path;
  int select_list_index;

  bool operator<(const PathExpressionSelectItemForGroupBy& another) const {
    return field_path.name_path().size() <
               another.field_path.name_path().size() ||
           (field_path.name_path().size() ==
                another.field_path.name_path().size() &&
            select_list_index < another.select_list_index);
  }
};

absl::Status Resolver::ResolveGroupByAll(
    const ASTGroupBy* group_by, const NameScope* from_clause_scope,
    QueryResolutionInfo* query_resolution_info) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  if (!language().LanguageFeatureEnabled(FEATURE_V_1_4_GROUP_BY_ALL) &&
      !ShouldApplyImplicitGroupByAll(language(), query_resolution_info)) {
    return MakeSqlErrorAt(group_by->all()) << "GROUP BY ALL is not supported";
  }
  // No grouping items should show up in the syntax.
  if (group_by != nullptr) {
    ZETASQL_RET_CHECK(group_by->grouping_items().empty());
  }

  query_resolution_info->set_is_group_by_all(true);
  SelectColumnStateList* select_column_state_list =
      query_resolution_info->select_column_state_list();

  // Record the minimum number of columns in the select list that are needed in
  // GROUP BY ALL.
  //
  // The GROUP BY list excludes any select list expression that is aggregate,
  // windowed, or does not reference any column in the current FROM clause. Such
  // expression could be constant, or only contains correlated references to
  // external scope, or only contains references to other column references that
  // are already considered grouping keys.
  //
  // In the first pass, sort all the select items that are path expressions
  // (with one or more names) based on the length of the path.
  //
  // In the second pass, build a ValidFieldInfoMap out of the columns that come
  // from the current FROM scope. The map records all of the unique
  // ResolvedColumns and paths of simple fields that start from those unique
  // columns.
  //
  // If multiple field paths share the same field path as common prefix, we only
  // record the shortest field path which is the common prefix. Only the select
  // items with those shortest field paths will be added to the group by list.
  // For example, given (a.b, a.b.c, a.b.d.e), the map will only record an entry
  // {a, [("b")]}. Given (a, a.b.c, x.y.z, x.y), the map will record two entries
  // {a, [()]} and {x, [("y")]}.
  //
  // In the third pass, check if any of the other select items have
  // non-correlated column references or field path that does not show up in the
  // map. If so, add them to the group by list.
  //
  // In the last pass, we build the final group by list by sorting the chosen
  // select list items based on their original indexes in the select list.
  absl::flat_hash_set<int> skip_column_positions;
  std::vector<PathExpressionSelectItemForGroupBy> path_expr_select_items;

  // First pass - Eliminate aggregate and window functions and identify select
  // list items that are simple non-correlated path expressions.
  for (int i = 0; i < select_column_state_list->Size(); i++) {
    const SelectColumnState* select_column_state =
        select_column_state_list->GetSelectColumnState(i);

    // A groupable column should not contain aggregate or analytic function.
    if (select_column_state->has_aggregation ||
        select_column_state->has_analytic) {
      skip_column_positions.insert(i);
      continue;
    }

    const ResolvedExpr* select_expr =
        GetPreGroupByResolvedExpr(select_column_state);
    ResolvedColumn source_column;
    bool is_correlated;
    ValidNamePath select_expr_name_path;
    if (select_expr != nullptr &&
        GetSourceColumnAndNamePath(
            select_expr, /*target_column=*/ResolvedColumn(), &source_column,
            &is_correlated, &select_expr_name_path, id_string_pool_)) {
      // If we identify a path expression, we do not need to look at it any more
      // when we go over the select list items again in the third pass.
      skip_column_positions.insert(i);
      // If it's a correlated expression that does not reference any FROM scope
      // column, ignore this select item.
      if (is_correlated) {
        continue;
      }
      path_expr_select_items.push_back(PathExpressionSelectItemForGroupBy(
          source_column, select_expr_name_path, i));
    }
  }
  std::sort(path_expr_select_items.begin(), path_expr_select_items.end());

  // Second pass - Identify which of the path expressions must be a group by key
  // because it references the FROM clause and no other group by key is a prefix
  // of it.
  ValidFieldInfoMap chosen_group_by_keys;
  std::vector<int> final_group_by_item_indexes;
  for (const PathExpressionSelectItemForGroupBy& path_expr_select_item :
       path_expr_select_items) {
    const ValidNamePathList* old_name_path_list;
    ResolvedColumn target_column;
    int num_names_consumed;

    // If the chosen group by keys already contain a prefix of the current path
    // expression, we ignore the current select item.
    if (chosen_group_by_keys.LookupNamePathList(
            path_expr_select_item.source_column, &old_name_path_list) &&
        ValidFieldInfoMap::FindLongestMatchingPathIfAny(
            *old_name_path_list, path_expr_select_item.field_path.name_path(),
            &target_column, &num_names_consumed)) {
      continue;
    }

    // Otherwise, update the map and the group by list.
    chosen_group_by_keys.InsertNamePath(path_expr_select_item.source_column,
                                        path_expr_select_item.field_path);
    final_group_by_item_indexes.push_back(
        path_expr_select_item.select_list_index);
  }

  // Third pass - For the rest of the select list items, identify those that
  // contain at least one path expression that references the FROM clause but no
  // other group by key is a prefix of it.
  for (int i = 0; i < select_column_state_list->Size(); i++) {
    if (skip_column_positions.contains(i)) {
      continue;
    }
    const SelectColumnState* select_column_state =
        select_column_state_list->GetSelectColumnState(i);
    const ResolvedExpr* select_expr =
        GetPreGroupByResolvedExpr(select_column_state);
    ZETASQL_RET_CHECK(select_expr != nullptr);
    ZETASQL_ASSIGN_OR_RETURN(bool no_from_scope_reference,
                     AllPathsInExprHaveExpectedPrefixes(
                         *select_expr, chosen_group_by_keys, id_string_pool_));
    if (no_from_scope_reference) {
      continue;
    }
    final_group_by_item_indexes.push_back(i);
  }

  // Last pass
  std::sort(final_group_by_item_indexes.begin(),
            final_group_by_item_indexes.end());
  for (int select_list_index : final_group_by_item_indexes) {
    ZETASQL_RETURN_IF_ERROR(AddSelectColumnToGroupByAllComputedColumn(
        select_column_state_list->GetSelectColumnState(select_list_index),
        query_resolution_info));
  }

  // There are a couple of situations when neither the group by list nor the
  // aggregate list contains any columns. They act like group by 0 column which
  // produce one row of output.
  if (query_resolution_info->group_by_column_state_list().empty() &&
      query_resolution_info->aggregate_columns_to_compute().empty()) {
    query_resolution_info->SetHasAggregation(true);
    query_resolution_info->set_has_group_by(false);
  }
  return absl::OkStatus();
}

// Resolves a list of expressions from the same grouping set, while the grouping
// set type is specified by the kind argument.
//
// For each expression
// 1. If it's a ASTStructConstructorWithParens, then extract all field
//    expressions from the struct constructor, and resolve each field expression
//    to grouping set column. This is for the use case of multi-columns, e.g.
//    In the example ROLLUP((x, y), z), (x, y) is parsed to an
//    ASTStructConstructorWithParens, so we have to extract sub-expressions in
//    the parentheses manually at first.
// 2. Otherwise, resolve the expression to a grouping set column.
//
// Resolved columns will be added to GroupingSetInfo in query_resolution_info.
// Each grouping item may have multiple columns, and they are added into
// ResolvedComputedColumnList, e.g. ROLLUP(x, y) has two grouping set item.
// For multi-columns grouping set item, the column list contains columns
// extracted from the ASTStructConstructor. For non-multi-columns, the column
// list contains only one column resolved from the expression. E.g. ROLLUP((x,
// y), z) has two grouping set item, the first item's column list is [x, y] and
// the second is [z].
//
// Example for a query with the following grouping sets specifications:
//   GROUPING SETS(x, (y, z), ROLLUP(x, y), CUBE((x, y), z))
// The 4 GroupingSetInfos look like accordingly:
//
// GroupingSetInfo {
//    kind: kGroupingSet
//    grouping_set_item_list: [[x]]
// }
// GroupingSetInfo {
//    kind: kGroupingSet
//    grouping_set_item_list: [[y], [z]]
// }
// GroupingSetInfo {
//    kind: kRollup
//    grouping_set_item_list: [[x], [y]]
// }
// GroupingSetInfo {
//    kind: kCube
//    grouping_set_item_list: [[x, y], [z]]
// }
absl::Status Resolver::ResolveGroupingSetExpressions(
    const absl::Span<const ASTExpression* const> expressions,
    const NameScope* from_clause_scope, GroupingSetKind kind,
    QueryResolutionInfo* query_resolution_info) {
  GroupingSetInfo grouping_set_info = {.kind = kind};
  for (const ASTExpression* expr : expressions) {
    ResolvedComputedColumnList column_list;
    // The expr is null when it's an empty grouping set. In this case, we simply
    // put the empty column list to grouping set item list and continue, without
    // going through the resolving process.
    if (expr == nullptr) {
      // A empty grouping_set_info will be added to the grouping set list.
      continue;
    }
    // Resolve the single-level annoymous struct construtor to a column list,
    // the expression has been validated without nested struct construtors.
    // Only resolve the annoymous struct construtor to a column list when:
    // 1. The current grouping set is a cube, OR
    // 2. The current groupint set is rollup, and FEATURE_V_1_4_GROUPING_SETS is
    //    enabled. We will not change the existing behavior of ROLLUP when
    //    FEATURE_V_1_4_GROUPING_SETS is not enabled.
    if (expr->Is<ASTStructConstructorWithParens>() &&
        (kind != GroupingSetKind::kRollup ||
         language().LanguageFeatureEnabled(FEATURE_V_1_4_GROUPING_SETS))) {
      const ASTStructConstructorWithParens* struct_constructor =
          expr->GetAsOrDie<ASTStructConstructorWithParens>();
      for (const ASTExpression* field_expression :
           struct_constructor->field_expressions()) {
        ZETASQL_RET_CHECK(field_expression != nullptr);
        ZETASQL_RETURN_IF_ERROR(ResolveGroupingItemExpression(
            field_expression,
            from_clause_scope,
            /*from_grouping_set=*/true, query_resolution_info, &column_list));
      }
    } else {
      ZETASQL_RETURN_IF_ERROR(ResolveGroupingItemExpression(
          expr,
          from_clause_scope,
          /*from_grouping_set=*/true, query_resolution_info, &column_list));
    }
    grouping_set_info.grouping_set_item_list.push_back(column_list);
  }
  query_resolution_info->AddGroupingSet(grouping_set_info);
  return absl::OkStatus();
}

// Analyzes group by items. If the group by item is a GROUPING SETS, ROLLUP, or
// CUBE, validate them accordingly and resolve the list of expressions by
// ResolveGroupingSetExpressions. If the group by item is a normal expression or
// column, simply resolve it via ResolveGroupingItemExpression.
absl::Status Resolver::ResolveGroupByExprs(
    const ASTGroupBy* group_by, const NameScope* from_clause_scope,
    QueryResolutionInfo* query_resolution_info) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  // The resolution of GROUP BY ALL syntax should not reach this code path.
  ZETASQL_RET_CHECK(group_by->all() == nullptr);

  bool has_rollup_or_cube = false;
  for (const ASTGroupingItem* grouping_item : group_by->grouping_items()) {

    if (grouping_item->rollup() != nullptr) {
      // GROUP BY ROLLUP
      has_rollup_or_cube = true;
      const ASTRollup* rollup = grouping_item->rollup();
      ZETASQL_RETURN_IF_ERROR(ValidateRollup(rollup, language(),
                                     group_by->grouping_items().size()));
      ZETASQL_RETURN_IF_ERROR(ResolveGroupingSetExpressions(
          rollup->expressions(), from_clause_scope, GroupingSetKind::kRollup,
          query_resolution_info));
      // We've validated that ROLLUP is the only grouping item in the query, so
      // break early here.
      break;
    } else if (grouping_item->cube() != nullptr) {
      // GROUP BY CUBE
      has_rollup_or_cube = true;
      const ASTCube* cube = grouping_item->cube();
      ZETASQL_RETURN_IF_ERROR(
          ValidateCube(cube, language(), group_by->grouping_items().size()));
      ZETASQL_RETURN_IF_ERROR(ResolveGroupingSetExpressions(
          cube->expressions(), from_clause_scope, GroupingSetKind::kCube,
          query_resolution_info));
    } else if (grouping_item->grouping_set_list() != nullptr) {
      // GROUP BY GROUPING SETS
      const ASTGroupingSetList* grouping_set_list =
          grouping_item->grouping_set_list();
      ZETASQL_RETURN_IF_ERROR(ValidateGroupingSetList(
          grouping_set_list, language(), group_by->grouping_items().size()));
      for (const ASTGroupingSet* grouping_set :
           grouping_set_list->grouping_sets()) {
        ZETASQL_RET_CHECK(grouping_set != nullptr);
        if (grouping_set->rollup() != nullptr) {
          // ROLLUP in GROUP BY GROUPING SETS()
          has_rollup_or_cube = true;
          ZETASQL_RETURN_IF_ERROR(ResolveGroupingSetExpressions(
              grouping_set->rollup()->expressions(), from_clause_scope,
              GroupingSetKind::kRollup, query_resolution_info));
        } else if (grouping_set->cube() != nullptr) {
          // CUBE in GROUP BY GROUPING SETS()
          has_rollup_or_cube = true;
          ZETASQL_RETURN_IF_ERROR(ResolveGroupingSetExpressions(
              grouping_set->cube()->expressions(), from_clause_scope,
              GroupingSetKind::kCube, query_resolution_info));
        } else {
          // Column list in GROUP BY GROUPING SETS or empty grouping set ()
          const ASTExpression* expr = grouping_set->expression();
          std::vector<const ASTExpression*> grouping_set_item_expr_list;
          // This is an anonymous struct construtor, we treat it as a grouping
          // set item list. The node is already validated that there is no
          // nested struct constructor inside.
          if (expr != nullptr && expr->Is<ASTStructConstructorWithParens>()) {
            const ASTStructConstructorWithParens* struct_constructor =
                expr->GetAsOrDie<ASTStructConstructorWithParens>();
            ZETASQL_RET_CHECK(struct_constructor != nullptr);
            for (const ASTExpression* field_expression :
                 struct_constructor->field_expressions()) {
              ZETASQL_RET_CHECK(field_expression != nullptr);
              grouping_set_item_expr_list.push_back(field_expression);
            }
          } else {
            // This is a single expression or an empty grouping set
            grouping_set_item_expr_list.push_back(expr);
          }
          ZETASQL_RETURN_IF_ERROR(ResolveGroupingSetExpressions(
              grouping_set_item_expr_list, from_clause_scope,
              GroupingSetKind::kGroupingSet, query_resolution_info));
        }
      }
    } else if (grouping_item->expression() != nullptr) {
      // GROUP BY expressions
      ZETASQL_RETURN_IF_ERROR(ResolveGroupingItemExpression(
          grouping_item->expression(),
          from_clause_scope,
          /*from_grouping_set=*/false, query_resolution_info));
    } else {
      // This is GROUP BY ()
      // We only allow GROUP BY () being used without other grouping items.
      if (language().LanguageFeatureEnabled(FEATURE_V_1_4_GROUPING_SETS)) {
        if (group_by->grouping_items().size() > 1) {
          return MakeSqlErrorAt(grouping_item)
                 << "GROUP BY () is only allowed when there are no other "
                    "grouping items";
        }
      } else {
        return MakeSqlErrorAt(grouping_item) << "GROUP BY () is not supported";
      }
    }
  }
  if (has_rollup_or_cube &&
      language().LanguageFeatureEnabled(FEATURE_V_1_4_GROUPING_SETS)) {
    analyzer_output_properties_.MarkRelevant(REWRITE_GROUPING_SET);
  }
  return absl::OkStatus();
}

// Analyze the GROUP BY expression.  Map SELECT list ordinal and
// alias references to the appropriate SelectColumnState in
// <query_resolution_info>, and resolve other expressions against
// the <from_clause_scope>.  For GROUP BY expressions that
// are SELECT list items, update the related SelectColumnState
// information with the grouped version of the column.  Updates
// query_resolution_info with the GROUP BY ResolvedColumns and
// ResolvedComputedColumns.
//
// Note that the current logic will not allow the use of SELECT list
// aliases within GROUP BY expressions, because we only match aliases
// exactly.  For example, the following is not supported:
//
//   select a+b as foo
//   ...
//   group by foo + 1;
//
// However, in most cases these types of queries would be invalid even
// without this restriction anyway since the SELECT list expression would
// not be something that was grouped by.  But it is possible to have a
// valid example (though highly contrived), such as:
//
//   select a+b as foo
//   ...
//   group by foo, foo + 1
//
// The general logic implemented by this function for each GROUP BY column is:
// 1) Determine if the GROUP BY expression exactly matches a SELECT list alias
//    or is an integer literal (representing a SELECT list column ordinal)
// 2) If either, update the corresponding SelectColumnState
// 3) If neither, resolve the expression against the <from_clause_scope>
// 4) Provide an error if grouping by literals, constant expressions, structs,
//    protos, or arrays.
// 5) If grouping by a column, update the mapping from pre-GROUP BY column to
//    post-GROUP BY column.
// 6) If grouping by an expression, update the mapping from the pre-GROUP BY
//    expression to the post-GROUP BY column.
// 7) Add a ResolvedComputedColumn for the GROUP BY expression.
// 8) If the expression is from a grouping set, then add the
//    ResolvedComputedColumn to the passed-in GroupingSetItem.
absl::Status Resolver::ResolveGroupingItemExpression(
    const ASTExpression* ast_group_by_expr,
    const NameScope* from_clause_scope, bool from_grouping_set,
    QueryResolutionInfo* query_resolution_info,
    ResolvedComputedColumnList* column_list) {
  ZETASQL_RET_CHECK(ast_group_by_expr != nullptr);
  if (from_grouping_set) {
    ZETASQL_RET_CHECK(column_list != nullptr);
  }

  ExprResolutionInfo no_aggregation(from_clause_scope, "GROUP BY");

  ABSL_DCHECK_NE(ast_group_by_expr->node_kind(), AST_IDENTIFIER)
      << "We expect to get PathExpressions, not Identifiers here";

  const SelectColumnState* group_by_column_state = nullptr;
  // Determine if the GROUP BY expression exactly matches a SELECT list alias.
  if (ast_group_by_expr->node_kind() == AST_PATH_EXPRESSION
  ) {
    const ASTPathExpression* path_expr =
        ast_group_by_expr->GetAsOrDie<ASTPathExpression>();
    const IdString alias = path_expr->first_name()->GetAsIdString();
    ZETASQL_RETURN_IF_ERROR(query_resolution_info->select_column_state_list()
                        ->FindAndValidateSelectColumnStateByAlias(
                            /*clause_name=*/"GROUP BY clause",
                            ast_group_by_expr, alias, &no_aggregation,
                            &group_by_column_state));
    if (group_by_column_state != nullptr && path_expr->num_names() != 1) {
      // We resolved the first identifier in a path expression to a SELECT
      // list alias.  There is currently no way that accessing the column's
      // fields in the GROUP BY can possibly be valid.  Consider:
      //   SELECT foo as foo2
      //   FROM (select as struct 1 as a, 2 as b) foo
      //   GROUP BY foo2.a
      // This is invalid since 'foo' is in the SELECT list but it is not
      // in the GROUP BY.
      //
      // If we add foo2 to the GROUP BY, then the query is invalid since
      // we do not allow grouping by STRUCT.
      //
      // If we were to allow grouping by PROTO or STRUCT then the following
      // query could be valid, but at this time it is not valid:
      //   SELECT foo as foo2
      //   FROM (select as struct 1 as a, 2 as b) foo
      //   GROUP BY foo, foo.a, foo.b;
      return MakeSqlErrorAt(ast_group_by_expr)
             << "Cannot GROUP BY field references from SELECT list alias "
             << alias;
    }
  }

  std::unique_ptr<const ResolvedExpr> resolved_expr;
  const ResolvedExpr* pre_group_by_expr = nullptr;
  // This means the current group by column is not a select column alias.
  if (group_by_column_state == nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveScalarExpr(ast_group_by_expr, from_clause_scope,
                                      "GROUP BY", &resolved_expr));

    // Determine if the GROUP BY expression is an integer literal
    // representing a SELECT list column ordinal.  Look for GROUP BY 1,2,3.
    if (resolved_expr->node_kind() == RESOLVED_LITERAL &&
        !resolved_expr->GetAs<ResolvedLiteral>()->has_explicit_type()) {
      const Value& value = resolved_expr->GetAs<ResolvedLiteral>()->value();
      if (value.type_kind() == TYPE_INT64 && !value.is_null()) {
        ZETASQL_RETURN_IF_ERROR(query_resolution_info->select_column_state_list()
                            ->FindAndValidateSelectColumnStateByOrdinal(
                                /*expr_description=*/"GROUP BY",
                                ast_group_by_expr, value.int64_value(),
                                &no_aggregation, &group_by_column_state));
      }
    }
  }

  IdString alias = GetAliasForExpression(ast_group_by_expr);
    if (alias.empty()) {
      alias = MakeIdString(absl::StrCat(
          "$groupbycol",
          query_resolution_info->group_by_column_state_list().size() + 1));
    }

  ResolvedColumn group_by_column;
  // This means the current group by column is a select column alias.
  if (group_by_column_state != nullptr) {
    if (group_by_column_state->is_group_by_column) {
      // We are already grouping by this SELECT list column, so we do not need
      // to do more unless the query uses GROUPING SETS, in which case we need
      // to add another entry in the grouping set list for it.
      if (!from_grouping_set) {
        return absl::OkStatus();
      }

      const ResolvedComputedColumn* existing_computed_column = nullptr;
      for (const GroupByColumnState& group_by_column :
           query_resolution_info->group_by_column_state_list()) {
        if (group_by_column_state->resolved_select_column ==
            group_by_column.computed_column->column()) {
          existing_computed_column = group_by_column.computed_column.get();
          break;
        }
      }

      ZETASQL_RET_CHECK_NE(existing_computed_column, nullptr)
          << "Expected to find existing group by column matching "
          << group_by_column_state->resolved_select_column.DebugString();
      // Field paths may repeat inside the grouping set list. We have already
      // resolved this field path, so just add another entry for it.
      column_list->push_back(existing_computed_column);
      return absl::OkStatus();
    }
    ZETASQL_RETURN_IF_ERROR(HandleGroupBySelectColumn(
        group_by_column_state, query_resolution_info, &resolved_expr,
        &pre_group_by_expr, &group_by_column));
  } else {
    // If the same expression already has a computed column, then reuse the
    // existing computed column. This is to make sure the group_by_list only has
    // one expression when multiple duplicated expressions appear in the group
    // by clause.
    // This change only applies to grouping sets, rollup and cube when
    // FEATURE_V_1_4_GROUPING_SETS is enabled for SAFETY. This is because a
    // global deduplication may have subtle impact to existing DISTINCT, normal
    // GROUP BY, legacy ROLLUP queries, though theoretically there shouldn't be.
    // We will apply this global deduplication once it's verifed.
    if (from_grouping_set &&
        language().LanguageFeatureEnabled(FEATURE_V_1_4_GROUPING_SETS)) {
      for (const GroupByColumnState& group_by_column :
           query_resolution_info->group_by_column_state_list()) {
        ZETASQL_ASSIGN_OR_RETURN(
            bool is_same_expression,
            IsSameExpressionForGroupBy(
                resolved_expr.get(), group_by_column.computed_column->expr()));

        if (is_same_expression) {
          column_list->push_back(group_by_column.computed_column.get());
          return absl::OkStatus();
        }
      }
    }

    ZETASQL_RETURN_IF_ERROR(HandleGroupByExpression(
        ast_group_by_expr, query_resolution_info, alias, &resolved_expr,
        &pre_group_by_expr, &group_by_column));
  }
  ZETASQL_RET_CHECK(resolved_expr != nullptr);

  // Check if the type of the group by column is groupable.
  std::string no_grouping_type;
  if (!resolved_expr->type()->SupportsGrouping(language(), &no_grouping_type)) {
    return MakeSqlErrorAt(ast_group_by_expr)
           << "Grouping by expressions of type " << no_grouping_type
           << " is not allowed";
  }

  const ResolvedComputedColumn* computed_column =
      query_resolution_info->AddGroupByComputedColumnIfNeeded(
          group_by_column, std::move(resolved_expr), pre_group_by_expr);
  if (from_grouping_set) {
    column_list->push_back(computed_column);
  }

  return absl::OkStatus();
}

// Performs a second pass of analysis over a SELECT list column,
// (re)evaluating the expression if necessary.
//
// SELECT list expressions that already have a ResolvedColumn assigned
// are not re-resolved.  Those that do not are re-resolved against the
// post-GROUP BY NameScope <group_by_scope> (in this case the ResolvedExpr
// from the first pass is deleted).
//
// Aggregate expressions that were resolved in the first pass are not
// re-resolved, but use the ResolvedExpr from the first pass.
//
// All necessary computed columns are created and assigned to the relevant
// computed column list (dot-star computed columns, and computed columns that
// can be referenced by GROUP BY/etc.).
//
// After this pass, all SELECT list columns have initialized output
// ResolvedColumns.
absl::Status Resolver::ResolveSelectColumnSecondPass(
    IdString query_alias, const NameScope* group_by_scope,
    SelectColumnState* select_column_state,
    std::shared_ptr<NameList>* final_project_name_list,
    QueryResolutionInfo* query_resolution_info) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  if (!select_column_state->resolved_select_column.IsInitialized()) {
    // If we have not already fully resolved this SELECT list expression
    // to a ResolvedColumn, then resolve the SELECT list expression.

    // First, look for SELECT list columns that resulted from star
    // expansion.  Star expansion was already performed in
    // ResolveSelectColumnFirstPass().   It is not done here.  The
    // star-expanded columns are often simple column references, but
    // could be GET_PROTO_FIELD or GET_STRUCT_FIELD if a FROM clause
    // subquery is SELECT AS PROTO or SELECT AS STRUCT.
    if (select_column_state->ast_expr->node_kind() == AST_DOT_STAR ||
        select_column_state->ast_expr->node_kind() ==
            AST_DOT_STAR_WITH_MODIFIERS ||
        select_column_state->ast_expr->node_kind() == AST_STAR ||
        select_column_state->ast_expr->node_kind() == AST_STAR_WITH_MODIFIERS) {
      if (select_column_state->resolved_expr->node_kind() ==
              RESOLVED_COLUMN_REF &&
          !select_column_state->resolved_expr->GetAs<ResolvedColumnRef>()
               ->is_correlated()) {
        // We already have column references after star expansion.  Do not
        // re-resolve the expression.  Just update the SelectColumnState
        // to reflect the associated ResolvedColumn.
        if (query_resolution_info->HasGroupByOrAggregation()) {
          // If this column is part of the grouping key, mark the post-grouping
          // column ref as such.
          const ResolvedExpr* const original_resolved_expr =
              select_column_state->resolved_expr.get();
          ZETASQL_RETURN_IF_ERROR(ResolveColumnRefExprToPostGroupingColumn(
              select_column_state->ast_expr, /*clause_name=*/"Star expansion",
              query_resolution_info, &select_column_state->resolved_expr));

          if (original_resolved_expr !=
              select_column_state->resolved_expr.get()) {
            // <select_column_state->resolved_expr> has been replaced with the
            // post-grouping column.  This indicates that this column is part of
            // the grouping key.
            ZETASQL_RET_CHECK_EQ(RESOLVED_COLUMN_REF,
                         select_column_state->resolved_expr->node_kind());
            select_column_state->is_group_by_column = true;
          }
        }
        select_column_state->resolved_select_column =
            select_column_state->resolved_expr->GetAs<ResolvedColumnRef>()
                ->column();
      } else {
        ResolvedColumn select_column(
            AllocateColumnId(), query_alias, select_column_state->alias,
            select_column_state->resolved_expr->annotated_type());
        query_resolution_info->select_list_columns_to_compute()->push_back(
            MakeResolvedComputedColumn(
                select_column, std::move(select_column_state->resolved_expr)));
        select_column_state->resolved_computed_column =
            query_resolution_info->select_list_columns_to_compute()
                ->back()
                .get();
        // Update the SelectColumnState to reflect the associated
        // ResolvedColumn.
        select_column_state->resolved_select_column = select_column;
      }
      // Check if the source of the '*' is a correlated column reference,
      // i.e., 'outercol.*'.
      bool is_correlated_column_ref = false;
      if (select_column_state->resolved_computed_column != nullptr &&
          select_column_state->resolved_computed_column->expr()->node_kind() ==
              RESOLVED_COLUMN_REF &&
          select_column_state->resolved_computed_column->expr()
              ->GetAs<ResolvedColumnRef>()
              ->is_correlated()) {
        is_correlated_column_ref = true;
      }
      // If the query has grouping or aggregation, then check the
      // *-expansion columns to ensure that they are either outer correlation
      // references or they are being grouped by.  If not, then produce
      // an error.
      //
      // We do not do this check for columns with aggregation or analytic
      // functions because those column expressions will be re-resolved
      // against the post-GROUP BY NameScope and errors will be detected then.
      if (query_resolution_info->HasGroupByOrAggregation() &&
          !select_column_state->is_group_by_column &&
          !is_correlated_column_ref && !select_column_state->has_aggregation &&
          !select_column_state->has_analytic) {
        return MakeSqlErrorAt(select_column_state->ast_expr)
               << "Star expansion expression references column "
               << select_column_state->alias
               << " which is neither grouped nor aggregated";
      }
    } else {
      const char* clause_name = "SELECT list";
      ExprResolutionInfo expr_resolution_info(
          group_by_scope, group_by_scope, group_by_scope,
          /*allows_aggregation_in=*/true,
          /*allows_analytic_in=*/true,
          query_resolution_info->HasGroupByOrAggregation(), clause_name,
          query_resolution_info, select_column_state->ast_expr,
          select_column_state->alias);
      std::unique_ptr<const ResolvedExpr> resolved_expr;
      const absl::Status resolve_expr_status = ResolveExpr(
          select_column_state->ast_expr, &expr_resolution_info, &resolved_expr);
      if (!resolve_expr_status.ok()) {
        ZETASQL_RET_CHECK_NE(select_column_state->resolved_expr.get(), nullptr);
        // Look at the QueryResolutionInfo to see if there is a GROUP BY
        // expression that exactly matches the ResolvedExpr from the
        // first pass resolution.
        bool found_group_by_expression = false;
          for (const GroupByColumnState& group_by_column_state :
               query_resolution_info->group_by_column_state_list()) {
            ZETASQL_ASSIGN_OR_RETURN(
                bool is_same_expr,
                IsSameExpressionForGroupBy(
                    select_column_state->resolved_expr.get(),
                    group_by_column_state.computed_column->expr()));
            if (is_same_expr) {
              // We matched this SELECT list expression to a GROUP BY
              // expression.
              // Update the select_column_state to point at the GROUP BY
              // computed column.
              select_column_state->resolved_select_column =
                  group_by_column_state.computed_column->column();
              found_group_by_expression = true;
              break;
            }
          }
        if (!found_group_by_expression) {
          // TODO: Improve error message to say that expressions didn't
          // match.
          ZETASQL_RET_CHECK(!resolve_expr_status.ok());
          return resolve_expr_status;
        }
      } else if (resolved_expr->node_kind() == RESOLVED_COLUMN_REF &&
                 !resolved_expr->GetAs<ResolvedColumnRef>()->is_correlated()) {
        // The expression was already resolved to a column.  If it was not
        // correlated, just use the column.
        const ResolvedColumn& select_column =
            resolved_expr->GetAs<ResolvedColumnRef>()->column();
        select_column_state->resolved_select_column = select_column;
      } else {
        ResolvedColumn select_column(AllocateColumnId(), query_alias,
                                     select_column_state->alias,
                                     resolved_expr->annotated_type());
        std::unique_ptr<ResolvedComputedColumn> computed_column =
            MakeResolvedComputedColumn(select_column, std::move(resolved_expr));
        query_resolution_info->select_list_columns_to_compute()->push_back(
            std::move(computed_column));
        select_column_state->resolved_select_column = select_column;
      }
    }
  }

  ZETASQL_RETURN_IF_ERROR((*final_project_name_list)
                      ->AddColumn(select_column_state->alias,
                                  select_column_state->resolved_select_column,
                                  select_column_state->is_explicit));

  return absl::OkStatus();
}

absl::Status Resolver::ResolveSelectListExprsSecondPass(
    IdString query_alias, const NameScope* group_by_scope,
    std::shared_ptr<NameList>* final_project_name_list,
    QueryResolutionInfo* query_resolution_info) {
  SelectColumnStateList* select_column_state_list =
      query_resolution_info->select_column_state_list();

  for (const std::unique_ptr<SelectColumnState>& select_column_state :
       select_column_state_list->select_column_state_list()) {
    ZETASQL_RETURN_IF_ERROR(ResolveSelectColumnSecondPass(
        query_alias, group_by_scope, select_column_state.get(),
        final_project_name_list, query_resolution_info));

    // Some sanity checks.
    ZETASQL_RET_CHECK(select_column_state->GetType() != nullptr);
    ZETASQL_RET_CHECK(select_column_state->resolved_select_column.IsInitialized());
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveSelectAs(
    const ASTSelectAs* select_as,
    const SelectColumnStateList& select_column_state_list,
    std::unique_ptr<const ResolvedScan> input_scan,
    const NameList* input_name_list,
    std::unique_ptr<const ResolvedScan>* output_scan,
    std::shared_ptr<const NameList>* output_name_list) {
  if (select_as->is_select_as_struct()) {
    // Convert to an anonymous struct type.
    return ConvertScanToStruct(select_as, /*named_struct_type=*/nullptr,
                               std::move(input_scan), input_name_list,
                               output_scan, output_name_list);
  } else if (select_as->is_select_as_value()) {
    // For SELECT AS VALUE, we just check that the input has exactly one
    // column, and then build a new NameList with is_value_table true.
    if (input_name_list->num_columns() != 1) {
      return MakeSqlErrorAt(select_as)
             << "SELECT AS VALUE query must have exactly one column";
    }
    std::unique_ptr<NameList> name_list(new NameList);
    ZETASQL_RETURN_IF_ERROR(name_list->AddValueTableColumn(
        kValueColumnId, input_name_list->column(0).column(), select_as));
    ZETASQL_RETURN_IF_ERROR(name_list->SetIsValueTable());
    *output_name_list = std::move(name_list);
    *output_scan = std::move(input_scan);
    return absl::OkStatus();
  } else {
    ABSL_DCHECK(select_as->type_name() != nullptr);

    const Type* type;
    ZETASQL_RETURN_IF_ERROR(ResolvePathExpressionAsType(
        select_as->type_name(), /*is_single_identifier=*/false, &type));

    if (type->IsStruct()) {
      // Convert to a named struct type.
      return ConvertScanToStruct(select_as->type_name(), type->AsStruct(),
                                 std::move(input_scan), input_name_list,
                                 output_scan, output_name_list);
    } else if (type->IsProto()) {
      return ConvertScanToProto(select_as->type_name(),
                                select_column_state_list, type->AsProto(),
                                std::move(input_scan), input_name_list,
                                output_scan, output_name_list);
    } else if (product_mode() == PRODUCT_EXTERNAL) {
      return MakeSqlErrorAt(select_as->type_name())
             << "SELECT AS TypeName can only be used for type STRUCT";
    } else {
      return MakeSqlErrorAt(select_as->type_name())
             << "SELECT AS TypeName can only be used for STRUCT or PROTO "
                "types, but "
             << select_as->type_name()->ToIdentifierPathString() << " has type "
             << type->ShortTypeName(product_mode());
    }
  }
}

absl::Status Resolver::ConvertScanToStruct(
    const ASTNode* ast_location,
    const StructType* named_struct_type,  // May be NULL.
    std::unique_ptr<const ResolvedScan> input_scan,
    const NameList* input_name_list,
    std::unique_ptr<const ResolvedScan>* output_scan,
    std::shared_ptr<const NameList>* output_name_list) {
  if (named_struct_type != nullptr) {
    // TODO Implement named struct construction - match fields
    // by name and verify types, with coercion.
    return MakeSqlErrorAt(ast_location)
           << "Constructing named STRUCT types in subqueries not implemented "
              "yet";
  }

  std::unique_ptr<ResolvedComputedColumn> computed_column;
  const CorrelatedColumnsSetList correlated_columns_set_list;
  ZETASQL_RETURN_IF_ERROR(CreateStructFromNameList(
      input_name_list, correlated_columns_set_list, &computed_column));

  const ResolvedColumn& struct_column = computed_column->column();
  NameList* mutable_name_list;
  output_name_list->reset((mutable_name_list = new NameList));
  // is_explicit=false because the created column is always anonymous.
  ZETASQL_RET_CHECK(IsInternalAlias(struct_column.name()));
  ZETASQL_RETURN_IF_ERROR(mutable_name_list->AddValueTableColumn(
      struct_column.name_id(), struct_column, ast_location));
  // Make the output table a value table.
  ZETASQL_RETURN_IF_ERROR(mutable_name_list->SetIsValueTable());

  *output_scan = MakeResolvedProjectScan(
      std::vector<ResolvedColumn>{struct_column},
      MakeNodeVector(std::move(computed_column)), std::move(input_scan));
  return absl::OkStatus();
}

absl::Status Resolver::CreateStructFromNameList(
    const NameList* name_list,
    const CorrelatedColumnsSetList& correlated_column_sets,
    std::unique_ptr<ResolvedComputedColumn>* computed_column) {
  ZETASQL_RET_CHECK(computed_column != nullptr);
  ZETASQL_RET_CHECK(*computed_column == nullptr);

  std::vector<std::unique_ptr<const ResolvedExpr>> field_exprs;
  std::vector<StructType::StructField> fields;

  for (const auto& named_column : name_list->columns()) {
    // Internal aliases mean the column has no visible name, so we make a
    // struct with an anonymous field.
    fields.emplace_back(IsInternalAlias(named_column.name())
                            ? ""
                            : named_column.name().ToString(),
                        named_column.column().type());
    field_exprs.emplace_back(MakeColumnRefWithCorrelation(
        named_column.column(), correlated_column_sets));
  }
  const StructType* struct_type;
  ZETASQL_RETURN_IF_ERROR(type_factory_->MakeStructType(fields, &struct_type));

  auto make_struct =
      MakeResolvedMakeStruct(struct_type, std::move(field_exprs));

  ZETASQL_RETURN_IF_ERROR(CheckAndPropagateAnnotations(
      /*error_node=*/nullptr, make_struct.get()));
  const ResolvedColumn struct_column(AllocateColumnId(), kMakeStructId,
                                     kStructId, make_struct->annotated_type());
  *computed_column =
      MakeResolvedComputedColumn(struct_column, std::move(make_struct));
  return absl::OkStatus();
}

absl::Status Resolver::ConvertScanToProto(
    const ASTNode* ast_type_location,
    const SelectColumnStateList& select_column_state_list,
    const ProtoType* proto_type, std::unique_ptr<const ResolvedScan> input_scan,
    const NameList* input_name_list,
    std::unique_ptr<const ResolvedScan>* output_scan,
    std::shared_ptr<const NameList>* output_name_list) {
  ZETASQL_RET_CHECK_EQ(select_column_state_list.Size(), input_name_list->num_columns());

  std::vector<ResolvedBuildProtoArg> arguments;
  for (int i = 0; i < input_name_list->num_columns(); ++i) {
    const ASTNode* ast_column_location =
        select_column_state_list.GetSelectColumnState(i)->ast_expr;
    const NamedColumn& named_column = input_name_list->column(i);
    if (IsInternalAlias(named_column.name())) {
      return MakeSqlErrorAt(ast_column_location)
             << "Cannot construct PROTO from query result because column "
             << (i + 1) << " has no name";
    }

    std::unique_ptr<ResolvedColumnRef> expr =
        MakeColumnRef(named_column.column());
    MaybeRecordParseLocation(ast_column_location, expr.get());
    arguments.emplace_back(
        ast_column_location, std::move(expr),
        std::make_unique<AliasOrASTPathExpression>(named_column.name()));
  }

  std::unique_ptr<const ResolvedExpr> resolved_build_proto_expr;
  ZETASQL_RETURN_IF_ERROR(ResolveBuildProto(ast_type_location, proto_type,
                                    input_scan.get(), "Column", "Query",
                                    &arguments, &resolved_build_proto_expr));

  // Wrap resolved_query with a projection that creates the proto.
  const ResolvedColumn proto_column(AllocateColumnId(), kMakeProtoId, kProtoId,
                                    proto_type);

  *output_scan = MakeResolvedProjectScan(
      std::vector<ResolvedColumn>{proto_column},
      MakeNodeVector(MakeResolvedComputedColumn(
          proto_column, std::move(resolved_build_proto_expr))),
      std::move(input_scan));

  NameList* mutable_name_list;
  output_name_list->reset((mutable_name_list = new NameList));
  // is_explicit=false because the created column is always anonymous.
  ZETASQL_RET_CHECK(IsInternalAlias(proto_column.name()));
  ZETASQL_RETURN_IF_ERROR(mutable_name_list->AddValueTableColumn(
      MakeIdString(proto_column.name()), proto_column, ast_type_location));
  // Make the output table a value table.
  ZETASQL_RETURN_IF_ERROR(mutable_name_list->SetIsValueTable());

  return absl::OkStatus();
}

static absl::Status GetSetScanEnumType(
    const ASTSetOperation* set_operation,
    ResolvedSetOperationScan::SetOperationType* op_type) {
  switch (set_operation->op_type()) {
    case ASTSetOperation::UNION:
      *op_type = set_operation->distinct()
                     ? ResolvedSetOperationScan::UNION_DISTINCT
                     : ResolvedSetOperationScan::UNION_ALL;
      break;

    case ASTSetOperation::EXCEPT:
      *op_type = set_operation->distinct()
                     ? ResolvedSetOperationScan::EXCEPT_DISTINCT
                     : ResolvedSetOperationScan::EXCEPT_ALL;
      break;

    case ASTSetOperation::INTERSECT:
      *op_type = set_operation->distinct()
                     ? ResolvedSetOperationScan::INTERSECT_DISTINCT
                     : ResolvedSetOperationScan::INTERSECT_ALL;
      break;

    case ASTSetOperation::NOT_SET:
      return MakeSqlErrorAtLocalNode(set_operation)
             << "Invalid set operation type";
  }

  return absl::OkStatus();
}

static absl::Status GetRecursiveScanEnumType(
    const ASTSetOperation* set_operation,
    ResolvedRecursiveScan::RecursiveSetOperationType* op_type) {
  ZETASQL_RET_CHECK_EQ(set_operation->op_type(), ASTSetOperation::UNION);
  if (set_operation->distinct()) {
    *op_type = ResolvedRecursiveScan::UNION_DISTINCT;
  } else {
    *op_type = ResolvedRecursiveScan::UNION_ALL;
  }
  return absl::OkStatus();
}

static std::string FormatColumnCount(const NameList& name_list) {
  return name_list.is_value_table()
             ? std::string(" is value table with 1 column")
             : absl::StrCat(" has ", name_list.num_columns(), " column",
                            (name_list.num_columns() == 1 ? "" : "s"));
}

static std::string ColumnPropagationModeToString(
    ASTSetOperation::ColumnPropagationMode mode) {
  switch (mode) {
    case ASTSetOperation::FULL:
      return "FULL";
    case ASTSetOperation::LEFT:
      return "LEFT";
    case ASTSetOperation::STRICT:
      return "STRICT";
    case ASTSetOperation::INNER:
      return "";
  }
}

static ASTSetOperation::ColumnMatchMode GetColumnMatchMode(
    const ASTSetOperationMetadata& metadata) {
  if (metadata.column_match_mode() == nullptr) {
    return ASTSetOperation::BY_POSITION;
  }
  return metadata.column_match_mode()->value();
}

static ASTSetOperation::ColumnPropagationMode GetColumnPropagationMode(
    const ASTSetOperationMetadata& metadata) {
  if (metadata.column_propagation_mode() != nullptr) {
    return metadata.column_propagation_mode()->value();
  }
  // If column match mode is BY_POSITION, the default is STRICT to align with
  // the existing behavior; otherwise default is INNER.
  ASTSetOperation::ColumnMatchMode column_match_mode =
      GetColumnMatchMode(metadata);
  if (column_match_mode == ASTSetOperation::BY_POSITION) {
    return ASTSetOperation::STRICT;
  }
  return ASTSetOperation::INNER;
}

static ResolvedSetOperationScan::SetOperationColumnMatchMode
GetResolvedSetOperationColumnMatchMode(
    ASTSetOperation::ColumnMatchMode column_match_mode) {
  switch (column_match_mode) {
    case ASTSetOperation::CORRESPONDING:
      return ResolvedSetOperationScan::CORRESPONDING;
    case ASTSetOperation::CORRESPONDING_BY:
      return ResolvedSetOperationScan::CORRESPONDING_BY;
    case ASTSetOperation::BY_POSITION:
      return ResolvedSetOperationScan::BY_POSITION;
  }
}

static ResolvedSetOperationScan::SetOperationColumnPropagationMode
GetResolvedSetOperationColumnPropagationMode(
    ASTSetOperation::ColumnPropagationMode column_propagate_mode) {
  switch (column_propagate_mode) {
    case ASTSetOperation::LEFT:
      return ResolvedSetOperationScan::LEFT;
    case ASTSetOperation::FULL:
      return ResolvedSetOperationScan::FULL;
    case ASTSetOperation::STRICT:
      return ResolvedSetOperationScan::STRICT;
    case ASTSetOperation::INNER:
      return ResolvedSetOperationScan::INNER;
  }
}

Resolver::SetOperationResolver::SetOperationResolver(
    const ASTSetOperation* set_operation, Resolver* resolver)
    : set_operation_(set_operation),
      resolver_(resolver),
      op_type_str_(resolver_->MakeIdString(
          absl::StrCat("$", absl::AsciiStrToLower(ReplaceFirst(
                                set_operation_->GetSQLForOperation(),
                                /*oldsub=*/" ", /*newsub=*/"_"))))) {}

ASTSetOperation::ColumnMatchMode
Resolver::SetOperationResolver::ASTColumnMatchMode() const {
  return GetColumnMatchMode(
      *set_operation_->metadata()->set_operation_metadata_list(0));
}

ASTSetOperation::ColumnPropagationMode
Resolver::SetOperationResolver::ASTColumnPropagationMode() const {
  return GetColumnPropagationMode(
      *set_operation_->metadata()->set_operation_metadata_list(0));
}

// Returns the identifiers provided in the CORRESPONDING BY list of the given
// `metadata`. If `metadata` does not have a corresponding by list (is nullptr),
// returns an empty vector.
static std::vector<IdString> GetCorrespondingByIdStrings(
    const ASTSetOperationMetadata& metadata) {
  if (metadata.corresponding_by_column_list() == nullptr) {
    return {};
  }
  std::vector<IdString> identifier_names;
  const absl::Span<const ASTIdentifier* const> identifiers =
      metadata.corresponding_by_column_list()->identifiers();
  identifier_names.reserve(identifiers.size());
  for (const ASTIdentifier* const identifier : identifiers) {
    identifier_names.push_back(identifier->GetAsIdString());
  }
  return identifier_names;
}

// Returns whether `by_list_1` and `by_list_2` are the same. IdStrings are
// compared case-insensitively.
static bool IsSameByList(const std::vector<IdString>& by_list_1,
                         const std::vector<IdString>& by_list_2) {
  return absl::c_equal(
      by_list_1, by_list_2,
      [](const IdString& a, const IdString& b) { return a.CaseEquals(b); });
}

absl::Status Resolver::SetOperationResolver::Resolve(
    const NameScope* scope, const Type* inferred_type_for_query,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  ZETASQL_RET_CHECK_GE(set_operation_->inputs().size(), 2);
  ZETASQL_RETURN_IF_ERROR(ValidateHint());
  ZETASQL_RETURN_IF_ERROR(ValidateCorresponding());
  ZETASQL_RETURN_IF_ERROR(ValidateIdenticalSetOperator());

  ResolvedSetOperationScan::SetOperationType op_type;
  ZETASQL_RETURN_IF_ERROR(GetSetScanEnumType(set_operation_, &op_type));

  std::vector<ResolvedInputResult> resolved_inputs;
  resolved_inputs.reserve(set_operation_->inputs().size());
  for (int idx = 0; idx < set_operation_->inputs().size(); ++idx) {
    ZETASQL_ASSIGN_OR_RETURN(resolved_inputs.emplace_back(),
                     ResolveInputQuery(scope, idx, inferred_type_for_query));
  }

  ResolvedColumnList final_column_list;
  std::shared_ptr<const NameList> name_list_template;
  if (ASTColumnMatchMode() == ASTSetOperation::BY_POSITION) {
    ZETASQL_RETURN_IF_ERROR(CheckSameColumnNumber(resolved_inputs));
    ZETASQL_ASSIGN_OR_RETURN(
        std::vector<std::vector<InputArgumentType>> column_type_lists,
        BuildColumnTypeListsByPosition(absl::MakeSpan(resolved_inputs)));
    ZETASQL_ASSIGN_OR_RETURN(
        std::vector<const Type*> super_types,
        GetSuperTypes(column_type_lists, /*column_identifier_in_error_string=*/
                      [&](int col_idx) -> std::string {
                        return absl::StrCat(col_idx + 1);
                      }));
    ZETASQL_ASSIGN_OR_RETURN(
        final_column_list,
        BuildFinalColumnList(resolved_inputs[0].name_list->GetColumnNames(),
                             super_types));
    // Type coercion, if needed.
    for (int idx = 0; idx < resolved_inputs.size(); ++idx) {
      ResolvedSetOperationItem* input = resolved_inputs.at(idx).node.get();
      ZETASQL_RETURN_IF_ERROR(CreateWrapperScanWithCastsForSetOperationItem(
          final_column_list, idx, input));
    }
    // The first subquery determines the name and explicit attribute of each
    // column, as well as whether the result is a value table.
    name_list_template = resolved_inputs.front().name_list;
  } else {
    ZETASQL_RETURN_IF_ERROR(CheckNoValueTable(resolved_inputs));
    std::vector<IdString> final_column_names;
    if (ASTColumnMatchMode() == ASTSetOperation::CORRESPONDING) {
      ZETASQL_ASSIGN_OR_RETURN(
          final_column_names,
          CalculateFinalColumnNamesForCorresponding(resolved_inputs));
    } else {
      // CORRESPONDING BY.
      ZETASQL_ASSIGN_OR_RETURN(
          final_column_names,
          CalculateFinalColumnNamesForCorrespondingBy(resolved_inputs));
    }
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<IndexMapper> index_mapper,
                     BuildIndexMapping(resolved_inputs, final_column_names));
    ZETASQL_ASSIGN_OR_RETURN(
        std::vector<std::vector<InputArgumentType>> column_type_lists,
        BuildColumnTypeListsForCorresponding(
            /*final_column_num=*/static_cast<int>(final_column_names.size()),
            resolved_inputs, index_mapper.get()));
    ZETASQL_ASSIGN_OR_RETURN(
        std::vector<const Type*> super_types,
        GetSuperTypes(column_type_lists,
                      /*column_identifier_in_error_string=*/
                      [&](int col_idx) -> std::string {
                        return final_column_names[col_idx].ToString();
                      }));
    ZETASQL_ASSIGN_OR_RETURN(final_column_list,
                     BuildFinalColumnList(final_column_names, super_types));
    ZETASQL_ASSIGN_OR_RETURN(name_list_template,
                     BuildNameListTemplateForCorresponding(
                         final_column_list, *resolved_inputs.front().name_list,
                         index_mapper.get()));
    ZETASQL_RETURN_IF_ERROR(AddTypeCastIfNeededForCorresponding(
        final_column_list, absl::MakeSpan(resolved_inputs),
        index_mapper.get()));
    // WARNING: After this function the output_column_list of each items in
    // `resolved_inputs` can be updated, so
    // - `output_column_list` and `name_list` do not necessarily match anymore.
    // - The mapping stored in `index_mapper` may not be valid for
    //   `output_column_list` anymore.
    ZETASQL_RETURN_IF_ERROR(AdjustAndReorderColumns(
        final_column_list, index_mapper.get(), resolved_inputs));
  }

  ResolvedSetOperationScanBuilder set_op_scan_builder =
      ResolvedSetOperationScanBuilder()
          .set_column_list(final_column_list)
          .set_op_type(op_type)
          .set_input_item_list(
              ExtractSetOperationItems(absl::MakeSpan(resolved_inputs)))
          .set_column_match_mode(
              GetResolvedSetOperationColumnMatchMode(ASTColumnMatchMode()))
          .set_column_propagation_mode(
              GetResolvedSetOperationColumnPropagationMode(
                  ASTColumnPropagationMode()));
  // Resolve the ResolvedOption (Query Hint), if present.
  if (set_operation_->hint() != nullptr) {
    std::vector<std::unique_ptr<const ResolvedOption>> hint_list;
    ZETASQL_RETURN_IF_ERROR(
        resolver_->ResolveHintAndAppend(set_operation_->hint(), &hint_list));
    set_op_scan_builder.set_hint_list(std::move(hint_list));
  }
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedSetOperationScan> set_op_scan,
                   std::move(set_op_scan_builder).Build());
  ZETASQL_RETURN_IF_ERROR(resolver_->CheckAndPropagateAnnotations(
      set_operation_,
      const_cast<ResolvedSetOperationScan*>(set_op_scan.get())));

  ZETASQL_ASSIGN_OR_RETURN(
      *output_name_list,
      BuildFinalNameList(*name_list_template, set_op_scan->column_list()));
  *output = std::move(set_op_scan);
  return absl::OkStatus();
}

absl::Status Resolver::SetOperationResolver::CheckSameColumnNumber(
    const std::vector<ResolvedInputResult>& resolved_inputs) const {
  const NameList& first_name_list = *resolved_inputs.front().name_list;
  for (int idx = 1; idx < resolved_inputs.size(); ++idx) {
    const NameList& curr_name_list = *resolved_inputs.at(idx).name_list;
    if (curr_name_list.num_columns() != first_name_list.num_columns()) {
      return MakeSqlErrorAt(set_operation_->inputs()[idx])
             << "Queries in " << set_operation_->GetSQLForOperation()
             << " have mismatched column count; query 1"
             << FormatColumnCount(first_name_list) << ", query " << (idx + 1)
             << FormatColumnCount(curr_name_list);
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::SetOperationResolver::CheckNoValueTable(
    absl::Span<const ResolvedInputResult> resolved_inputs) const {
  const ASTNode* op_type =
      set_operation_->metadata()->set_operation_metadata_list(0)->op_type();
  for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
    if (resolved_inputs[query_idx].name_list->is_value_table()) {
      return MakeSqlErrorAtLocalNode(op_type)
             << "Value table type not allowed in set operations when "
             << "CORRESPONDING is used: query " << (query_idx + 1);
    }
  }
  return absl::OkStatus();
}

// `named_columns` should not contain any duplicate names.
static absl::StatusOr<
    absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>>
ToColumnNameSet(const std::vector<NamedColumn>& named_columns) {
  absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
      column_names;
  for (const NamedColumn& named_column : named_columns) {
    ZETASQL_RET_CHECK(column_names.insert(named_column.name()).second);
  }
  return column_names;
}

// Similar to `ToColumnNameSet` but allows the input `named_columns` to have
// duplicate names.
static absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
ToColumnNameSetAllowDuplicates(const std::vector<NamedColumn>& named_columns) {
  absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
      column_names;
  for (const NamedColumn& named_column : named_columns) {
    column_names.insert(named_column.name());
  }
  return column_names;
}

static IdStringHashSetCase ToColumnNameSetAllowDuplicates(
    const absl::Span<const ASTIdentifier* const> identifiers) {
  IdStringHashSetCase column_names;
  for (const ASTIdentifier* const identifier : identifiers) {
    column_names.insert(identifier->GetAsIdString());
  }
  return column_names;
}

static std::string ColumnNamesToString(
    const std::vector<NamedColumn>& named_columns) {
  return absl::StrCat(
      "[",
      absl::StrJoin(named_columns, ", ",
                    [](std::string* out, const NamedColumn& named_column) {
                      absl::StrAppend(out,
                                      ToIdentifierLiteral(named_column.name()));
                    }),
      "]");
}

absl::StatusOr<std::vector<IdString>>
Resolver::SetOperationResolver::CalculateFinalColumnNamesForCorresponding(
    absl::Span<const ResolvedInputResult> resolved_inputs) const {
  // Validation needed for CORRESPONDING regardless of column_propagation_mode.
  for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
    absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
        column_names;
    const NameList* name_list = resolved_inputs[query_idx].name_list.get();
    const std::vector<NamedColumn>& named_column_list = name_list->columns();
    for (int column_idx = 0; column_idx < named_column_list.size();
         ++column_idx) {
      const NamedColumn& column = named_column_list[column_idx];
      if (IsInternalAlias(column.name())) {
        return MakeSqlErrorAtLocalNode(set_operation_->inputs(query_idx))
               << "Anonymous columns are not allowed in set operations when "
               << "CORRESPONDING is used: query " << (query_idx + 1)
               << ", column " << (column_idx + 1);
      }
      if (!column_names.insert(column.name()).second) {
        return MakeSqlErrorAtLocalNode(set_operation_->inputs(query_idx))
               << "Duplicate columns found when using CORRESPONDING in set "
               << "operations: " << column.name().ToString() << " in query "
               << (query_idx + 1);
      }
    }
  }

  // Calculates the final column list based on column_propagation_mode.
  switch (ASTColumnPropagationMode()) {
    case ASTSetOperation::INNER: {
      absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
          column_intersection;
      for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
        absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
            column_names;
        for (const NamedColumn& named_column :
             resolved_inputs[query_idx].name_list->columns()) {
          ZETASQL_RET_CHECK(column_names.insert(named_column.name()).second);
        }
        if (query_idx > 0) {
          absl::erase_if(column_intersection, [&](const IdString& name) {
            return !column_names.contains(name);
          });
        } else {
          column_intersection = column_names;
        }
      }
      if (column_intersection.empty()) {
        // Report error at the set operation.
        const ASTNode* op_type = set_operation_->metadata()
                                     ->set_operation_metadata_list(0)
                                     ->op_type();
        return MakeSqlErrorAtLocalNode(op_type)
               << "Queries of the set operation using CORRESPONDING do not "
               << "have any columns in common";
      }
      std::vector<IdString> matched_column_list;
      // The columns in `matched_column_list` are returned in the order they
      // appear in the first query input.
      for (const NamedColumn& column :
           resolved_inputs.front().name_list->columns()) {
        if (column_intersection.contains(column.name())) {
          matched_column_list.push_back(column.name());
        }
      }
      return matched_column_list;
    }
    case ASTSetOperation::LEFT: {
      // The output columns are identical to the columns in the first scan.
      // Subsequent scans need at least one common column name with the first
      // scan, otherwise an SQL error occurs. Columns from these scans not found
      // in the first scan are excluded.
      //
      // For example, consider the following statement:
      //
      // ```SQL
      // SELECT 1 AS A, 2 AS B
      // FULL UNION ALL CORRESPONDING
      // SELECT 3 AS B, 4 AS A, 5 AS D
      // ```
      //
      // The output columns will be [A, B]. The column D of the second scan is
      // dropped, and the order of A and B follows the order in the first scan.
      absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
          first_query_column_names;
      for (const NamedColumn& named_column :
           resolved_inputs.front().name_list->columns()) {
        ZETASQL_RET_CHECK(first_query_column_names.insert(named_column.name()).second);
      }
      for (int query_idx = 1; query_idx < resolved_inputs.size(); ++query_idx) {
        bool has_common = false;
        for (const NamedColumn& named_column :
             resolved_inputs[query_idx].name_list->columns()) {
          if (first_query_column_names.contains(named_column.name())) {
            has_common = true;
            break;
          }
        }
        if (!has_common) {
          const ASTNode* op_type = set_operation_->metadata()
                                       ->set_operation_metadata_list(0)
                                       ->op_type();
          return MakeSqlErrorAtLocalNode(op_type)
                 << "Query " << (query_idx + 1)
                 << " of the set operation with LEFT mode does not share "
                 << "common columns with the first query";
        }
      }
      // Output columns are the same as the ones in the first query.
      return resolved_inputs.front().name_list->GetColumnNames();
    }
    case ASTSetOperation::FULL: {
      // The output columns is a union of all input scan columns ensuring no
      // duplicate aliases. The column order reflects the sequence of input
      // scans, and within each scan, the left-to-right order of first
      // appearance of each unique alias.
      //
      // For example, consider the following statement:
      //
      // ```SQL
      // SELECT 1 AS A, 2 AS B
      // FULL UNION ALL CORRESPONDING
      // SELECT 3 AS C, 4 AS A, 5 AS D
      // ```
      //
      // The output columns will be [A, B, C, D]. The column A of the second
      // scan does not show up twice in the list, and its order in the list
      // is determined by its first appearance, i.e. the column A of the first
      // scan.
      absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
          seen_columns;
      std::vector<IdString> unique_column_names;
      unique_column_names.reserve(
          resolved_inputs.front().name_list->columns().size());
      for (const ResolvedInputResult& resolved_input : resolved_inputs) {
        for (const NamedColumn& column : resolved_input.name_list->columns()) {
          if (seen_columns.insert(column.name()).second) {
            unique_column_names.push_back(column.name());
          }
        }
      }
      return unique_column_names;
    }
    case ASTSetOperation::STRICT: {
      // The output columns are identical to the columns in the first scan.
      // Subsequent scans must share the same set of column names (column order
      // can be different), else an SQL error occurs.
      absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
          first_query_column_names;
      ZETASQL_ASSIGN_OR_RETURN(
          first_query_column_names,
          ToColumnNameSet(resolved_inputs.front().name_list->columns()));
      for (int query_idx = 1; query_idx < resolved_inputs.size(); ++query_idx) {
        absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
            column_names;
        ZETASQL_ASSIGN_OR_RETURN(
            column_names,
            ToColumnNameSet(resolved_inputs[query_idx].name_list->columns()));
        if (!zetasql_base::HashSetEquality(first_query_column_names, column_names)) {
          const ASTNode* op_type = set_operation_->metadata()
                                       ->set_operation_metadata_list(0)
                                       ->op_type();
          return MakeSqlErrorAtLocalNode(op_type)
                 << "STRICT CORRESPONDING requires all input queries to have "
                    "identical column names, but the first query has "
                 << ColumnNamesToString(
                        resolved_inputs.front().name_list->columns())
                 << " and query " << (query_idx + 1) << " has "
                 << ColumnNamesToString(
                        resolved_inputs[query_idx].name_list->columns());
        }
      }
      // Output columns are the same as the ones in the first query.
      return resolved_inputs.front().name_list->GetColumnNames();
    }
  }
}

// Returns the first identifier in `identifiers` that does not appear in
// `scan_column_names`.
static const ASTIdentifier* GetFirstNonPresentIdentifier(
    const absl::Span<const ASTIdentifier* const> identifiers,
    const absl::flat_hash_set<IdString, IdStringCaseHash,
                              IdStringCaseEqualFunc>& scan_column_names) {
  for (const ASTIdentifier* identifier : identifiers) {
    if (!scan_column_names.contains(identifier->GetAsIdString())) {
      return identifier;
    }
  }
  return nullptr;
}

IdStringHashSetCase Resolver::SetOperationResolver::GetAllColumnNames(
    const std::vector<ResolvedInputResult>& resolved_inputs) const {
  IdStringHashSetCase all_column_names;
  for (const ResolvedInputResult& resolved_input : resolved_inputs) {
    std::vector<IdString> scan_column_names =
        resolved_input.name_list->GetColumnNames();
    all_column_names.insert(scan_column_names.begin(), scan_column_names.end());
  }
  return all_column_names;
}

absl::StatusOr<std::vector<IdString>>
Resolver::SetOperationResolver::CalculateFinalColumnNamesForCorrespondingBy(
    const std::vector<ResolvedInputResult>& resolved_inputs) const {
  const absl::Span<const ASTIdentifier* const> by_list =
      set_operation_->metadata()
          ->set_operation_metadata_list(0)
          ->corresponding_by_column_list()
          ->identifiers();
  // Enforced by the grammar that the by list must not be empty.
  // TODO: Replace the ZETASQL_RET_CHECK error with a SQL error once the
  // grammar allows empty CORRESPONDING BY list.
  ZETASQL_RET_CHECK(!by_list.empty());
  // No duplicate identifiers in `by_list`.
  absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
      by_list_identifier_set;
  for (const ASTIdentifier* identifier : by_list) {
    if (!by_list_identifier_set.insert(identifier->GetAsIdString()).second) {
      return MakeSqlErrorAtLocalNode(identifier)
             << "Duplicate identifier are not allowed in the CORRESPONDING BY "
             << "list: " << identifier->GetAsIdString().ToStringView();
    }
  }
  // No scans have multiple columns with the name.
  for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
    const ResolvedInputResult& resolved_input = resolved_inputs[query_idx];
    absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
        in_by_list_scan_columns;
    for (const NamedColumn& column : resolved_input.name_list->columns()) {
      if (!by_list_identifier_set.contains(column.name())) {
        // It is ok for the columns not in the by_list to have duplicates.
        continue;
      }
      if (!in_by_list_scan_columns.insert(column.name()).second) {
        return MakeSqlErrorAtLocalNode(set_operation_->inputs(query_idx))
               << "The identifier " << column.name().ToStringView()
               << " from the CORRESPONDING BY list appears multiple times in "
               << "the query " << (query_idx + 1);
      }
    }
  }
  // Outer-mode specific checks.
  switch (ASTColumnPropagationMode()) {
    case ASTSetOperation::INNER: {
      // All identifiers in by_list must appear in every input query.
      for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
        const ResolvedInputResult& resolved_input = resolved_inputs[query_idx];
        absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
            scan_column_names = ToColumnNameSetAllowDuplicates(
                resolved_input.name_list->columns());
        const ASTIdentifier* non_present_identifier =
            GetFirstNonPresentIdentifier(by_list, scan_column_names);
        if (non_present_identifier != nullptr) {
          return MakeSqlErrorAtLocalNode(non_present_identifier)
                 << "The identifier " << non_present_identifier->GetAsIdString()
                 << " from the CORRESPONDING BY list does not appear in the "
                 << "input query " << (query_idx + 1)
                 << ". All columns in the BY list must appear in each input "
                 << "query unless FULL CORRESPONDING or LEFT CORRESPONDING is "
                 << "specified";
        }
      }
      break;
    }
    case ASTSetOperation::FULL: {
      // Each identifier in by_list must appear in at least one of the input
      // queries.
      IdStringHashSetCase all_column_names = GetAllColumnNames(resolved_inputs);
      for (const ASTIdentifier* identifier : by_list) {
        if (!all_column_names.contains(identifier->GetAsIdString())) {
          return MakeSqlErrorAtLocalNode(identifier)
                 << "The identifier " << identifier->GetAsIdString()
                 << " from the CORRESPONDING BY list does not appear in any "
                 << "input queries.";
        }
      }
      break;
    }
    case ASTSetOperation::LEFT: {
      // All identifiers in by_list must appear in the first input query.
      IdStringHashSetCase first_scan_column_names =
          ToColumnNameSetAllowDuplicates(
              resolved_inputs.front().name_list->columns());
      const ASTIdentifier* non_present_identifier =
          GetFirstNonPresentIdentifier(by_list, first_scan_column_names);
      if (non_present_identifier != nullptr) {
        return MakeSqlErrorAtLocalNode(non_present_identifier)
               << "The column name '" << non_present_identifier->GetAsIdString()
               << "' from the LEFT CORRESPONDING BY list must appear in the "
               << "first query";
      }
      break;
    }
    case ASTSetOperation::STRICT: {
      // All queries must share the same set of column names as the by_list.
      IdStringHashSetCase by_list_identifier_set =
          ToColumnNameSetAllowDuplicates(by_list);
      for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
        const IdStringHashSetCase scan_column_names =
            ToColumnNameSetAllowDuplicates(
                resolved_inputs[query_idx].name_list->columns());
        if (!zetasql_base::HashSetEquality(by_list_identifier_set, scan_column_names)) {
          return MakeSqlErrorAtLocalNode(set_operation_->inputs(query_idx))
                 << "Query #" << (query_idx + 1)
                 << " must share the same set of column names as the BY list "
                 << "when using STRICT CORRESPONDING BY";
        }
      }
      break;
    }
  }
  // All validations have passed, return the IdStrings.
  std::vector<IdString> final_column_names;
  final_column_names.reserve(by_list.size());
  for (const ASTIdentifier* identifier : by_list) {
    final_column_names.push_back(identifier->GetAsIdString());
  }
  return final_column_names;
}

absl::StatusOr<std::optional<int>>
Resolver::SetOperationResolver::IndexMapper::GetOutputColumnIndex(
    int query_idx, int final_column_idx) const {
  auto two_way_mapping = index_mapping_.find(query_idx);
  ZETASQL_RET_CHECK(two_way_mapping != index_mapping_.end());

  const auto& final_to_output = two_way_mapping->second.final_to_output;
  auto output_column_idx = final_to_output.find(final_column_idx);

  if (output_column_idx == final_to_output.end()) {
    return std::nullopt;
  }
  return output_column_idx->second;
}

absl::StatusOr<std::optional<int>>
Resolver::SetOperationResolver::IndexMapper::GetFinalColumnIndex(
    int query_idx, int output_column_idx) const {
  auto two_way_mapping = index_mapping_.find(query_idx);
  ZETASQL_RET_CHECK(two_way_mapping != index_mapping_.end());

  const auto& output_to_final = two_way_mapping->second.output_to_final;
  auto final_column_idx = output_to_final.find(output_column_idx);

  if (final_column_idx == output_to_final.end()) {
    return std::nullopt;
  }
  return final_column_idx->second;
}

absl::Status Resolver::SetOperationResolver::IndexMapper::AddMapping(
    int query_idx, int final_column_idx, int output_column_idx) {
  auto& two_way_mapping = index_mapping_[query_idx];
  ZETASQL_RET_CHECK(two_way_mapping.final_to_output
                .insert({final_column_idx, output_column_idx})
                .second);
  ZETASQL_RET_CHECK(two_way_mapping.output_to_final
                .insert({output_column_idx, final_column_idx})
                .second);
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<Resolver::SetOperationResolver::IndexMapper>>
Resolver::SetOperationResolver::BuildIndexMapping(
    absl::Span<const ResolvedInputResult> resolved_inputs,
    absl::Span<const IdString> final_column_names) const {
  auto index_mapper = std::make_unique<IndexMapper>(resolved_inputs.size());
  absl::flat_hash_set<IdString, IdStringCaseHash, IdStringCaseEqualFunc>
      final_column_names_set(final_column_names.begin(),
                             final_column_names.end());
  for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
    std::vector<IdString> curr_names =
        resolved_inputs[query_idx].name_list->GetColumnNames();
    absl::flat_hash_map<IdString, int, IdStringCaseHash, IdStringCaseEqualFunc>
        name_to_output_index;
    for (int output_column_idx = 0; output_column_idx < curr_names.size();
         ++output_column_idx) {
      if (!final_column_names_set.contains(curr_names[output_column_idx])) {
        // This column does not show up in the final column list, skip it. This
        // check is needed because CORRESPONDING BY allows the scan columns to
        // have duplicate names if they do not show up in the by list, and
        // `name_to_output_index` does not allow duplicate column names.
        continue;
      }
      ZETASQL_RET_CHECK(name_to_output_index
                    .insert({curr_names[output_column_idx], output_column_idx})
                    .second);
    }

    for (int final_column_idx = 0; final_column_idx < final_column_names.size();
         ++final_column_idx) {
      IdString final_column_name = final_column_names[final_column_idx];
      auto output_column_index = name_to_output_index.find(final_column_name);
      if (output_column_index == name_to_output_index.end()) {
        continue;
      }
      ZETASQL_RET_CHECK_OK(index_mapper->AddMapping(query_idx, final_column_idx,
                                            output_column_index->second));
    }
  }
  return index_mapper;
}

absl::StatusOr<std::vector<std::vector<InputArgumentType>>>
Resolver::SetOperationResolver::BuildColumnTypeListsForCorresponding(
    int final_column_num, absl::Span<const ResolvedInputResult> resolved_inputs,
    const IndexMapper* index_mapper) const {
  std::vector<std::vector<InputArgumentType>> column_type_lists;
  column_type_lists.resize(final_column_num);

  for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
    const ResolvedScan* resolved_scan = resolved_inputs[query_idx].node->scan();
    const NameList& curr_name_list = *resolved_inputs.at(query_idx).name_list;
    for (int i = 0; i < final_column_num; ++i) {
      ZETASQL_ASSIGN_OR_RETURN(std::optional<int> output_column_index,
                       index_mapper->GetOutputColumnIndex(query_idx, i));
      if (!output_column_index.has_value()) {
        // This query does not have a column corresponding to the i-th final
        // column; a NULL column will be padded which can coerce to any types.
        column_type_lists[i].push_back(InputArgumentType::UntypedNull());
        continue;
      }
      const ResolvedColumn& column =
          curr_name_list.column(*output_column_index).column();
      column_type_lists[i].push_back(
          GetColumnInputArgumentType(column, resolved_scan));
    }
  }
  return column_type_lists;
}

absl::Status
Resolver::SetOperationResolver::AddTypeCastIfNeededForCorresponding(
    const ResolvedColumnList& final_column_list,
    absl::Span<ResolvedInputResult> resolved_inputs,
    const IndexMapper* index_mapper) const {
  for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
    ResolvedSetOperationItem* item = resolved_inputs.at(query_idx).node.get();
    ZETASQL_ASSIGN_OR_RETURN(ResolvedColumnList matched_final_columns,
                     GetCorrespondingFinalColumns(final_column_list,
                                                  item->output_column_list(),
                                                  query_idx, index_mapper));
    ZETASQL_RETURN_IF_ERROR(CreateWrapperScanWithCastsForSetOperationItem(
        matched_final_columns, query_idx, item));
  }
  return absl::OkStatus();
}

absl::StatusOr<ResolvedColumnList>
Resolver::SetOperationResolver::GetCorrespondingFinalColumns(
    const ResolvedColumnList& final_column_list,
    const ResolvedColumnList& output_column_list, int query_idx,
    const IndexMapper* index_mapper) const {
  ResolvedColumnList matched_final_columns;
  matched_final_columns.reserve(output_column_list.size());

  for (int i = 0; i < output_column_list.size(); ++i) {
    ZETASQL_ASSIGN_OR_RETURN(std::optional<int> final_column_index,
                     index_mapper->GetFinalColumnIndex(query_idx, i));
    if (!final_column_index.has_value()) {
      // This column does not show up in the final_column_list, use itself as
      // the "final" column so that no type cast is needed.
      matched_final_columns.push_back(output_column_list[i]);
      continue;
    }
    ZETASQL_RET_CHECK_GE(*final_column_index, 0);
    ZETASQL_RET_CHECK_LT(*final_column_index, final_column_list.size());
    matched_final_columns.push_back(final_column_list[*final_column_index]);
  }
  return matched_final_columns;
}

absl::Status Resolver::SetOperationResolver::AdjustAndReorderColumns(
    const ResolvedColumnList& final_column_list,
    const IndexMapper* index_mapper,
    std::vector<ResolvedInputResult>& resolved_inputs) const {
  for (int query_idx = 0; query_idx < resolved_inputs.size(); ++query_idx) {
    ResolvedColumnList new_output_column_list;
    new_output_column_list.reserve(final_column_list.size());
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> null_columns;

    // Reorder output_column_list based on the column order in
    // final_column_list, and pad NULL columns if needed.
    for (int final_column_idx = 0; final_column_idx < final_column_list.size();
         ++final_column_idx) {
      const ResolvedColumn& column = final_column_list[final_column_idx];
      ZETASQL_ASSIGN_OR_RETURN(
          std::optional<int> output_column_index,
          index_mapper->GetOutputColumnIndex(query_idx, final_column_idx));

      if (output_column_index.has_value()) {
        const ResolvedColumnList output_column_list =
            resolved_inputs[query_idx].node->output_column_list();
        ZETASQL_RET_CHECK_GE(*output_column_index, 0);
        ZETASQL_RET_CHECK_LT(*output_column_index, output_column_list.size());
        new_output_column_list.push_back(
            output_column_list[*output_column_index]);
        continue;
      }

      // INNER and STRICT do not allow padding NULL columns.
      ZETASQL_RET_CHECK_NE(ASTColumnPropagationMode(), ASTSetOperation::INNER);
      ZETASQL_RET_CHECK_NE(ASTColumnPropagationMode(), ASTSetOperation::STRICT);
      // This column does not appear in this query. Prepare a computed NULL
      // column.
      std::unique_ptr<const ResolvedComputedColumn> null_column =
          MakeResolvedComputedColumn(
              ResolvedColumn(
                  resolver_->AllocateColumnId(),
                  resolver_->MakeIdString("$null_column_for_outer_set_op"),
                  resolver_->MakeIdString(column.name_id().ToStringView()),
                  // No annotations are assigned to the created NULL columns.
                  // This is consistent with the case when an input to a set
                  // operation has a literal null, e.g.
                  //
                  // SELECT COLLATE('abc', 'en-us')
                  // UNION ALL
                  // SELECT NULL
                  column.type()),
              zetasql::MakeResolvedLiteral(Value::Null(column.type())));
      resolver_->RecordColumnAccess(null_column->column());
      new_output_column_list.push_back(null_column->column());
      null_columns.push_back(std::move(null_column));
    }

    ResolvedSetOperationItem* input = resolved_inputs[query_idx].node.get();
    if (new_output_column_list != input->scan()->column_list()) {
      // Add a ProjectScan to adjust the output columns, including cases when:
      // - only some of the expected columns are selected
      // - original columns are reordered
      // - there are padded NULL columns
      //
      // Note: `input->output_column_list()` is the same as
      // `input->scan()->column_list()` except for the "SELECT DISTINCT" edge
      // case where a ProjectScan is missing: b/36095506. For example, in the
      // following resolved ast, `output_column_list` has more columns than
      // `scan.column_list()`:
      //
      // ```
      // scan=AggregateScan(column_list=$distinct.[int32#19, int64_t#20])
      // output_column_list=$distinct.[int32#19, int64_t#20, int32_t#19]
      // ```
      ZETASQL_ASSIGN_OR_RETURN(
          std::unique_ptr<const ResolvedProjectScan> project_scan,
          ResolvedProjectScanBuilder()
              .set_column_list(new_output_column_list)
              .set_expr_list(std::move(null_columns))
              .set_input_scan(input->release_scan())
              .set_node_source(kNodeSourceResolverSetOperationCorresponding)
              .Build());
      input->set_scan(std::move(project_scan));
    }
    *input->mutable_output_column_list() = std::move(new_output_column_list);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<const NameList>>
Resolver::SetOperationResolver::BuildNameListTemplateForCorresponding(
    const ResolvedColumnList& final_column_list,
    const NameList& first_item_name_list,
    const IndexMapper* index_mapper) const {
  ZETASQL_RET_CHECK(!first_item_name_list.is_value_table());
  std::shared_ptr<NameList> name_list(new NameList);
  for (int i = 0; i < final_column_list.size(); ++i) {
    ZETASQL_ASSIGN_OR_RETURN(std::optional<int> output_column_index,
                     index_mapper->GetOutputColumnIndex(/*query_idx=*/0, i));
    bool is_explicit;
    if (output_column_index.has_value()) {
      // This final column is present in the first input. Use the explicit
      // attribute of its corresponding column.
      is_explicit =
          first_item_name_list.column(*output_column_index).is_explicit();
    } else {
      // Padded NULL columns are not explicit columns.
      is_explicit = false;
    }
    ZETASQL_RETURN_IF_ERROR(name_list->AddColumn(final_column_list[i].name_id(),
                                         final_column_list[i], is_explicit));
  }
  return name_list;
}

absl::StatusOr<std::vector<const Type*>>
Resolver::SetOperationResolver::GetSuperTypes(
    absl::Span<const std::vector<InputArgumentType>> column_type_lists,
    std::function<std::string(int)> column_identifier_in_error_string) const {
  // Point at the start of the second query so the error is close to the set
  // operation keyword.
  const ASTNode* ast_input_location = set_operation_->inputs()[1];
  std::vector<const Type*> supertypes;
  supertypes.reserve(column_type_lists.size());

  for (int i = 0; i < column_type_lists.size(); ++i) {
    InputArgumentTypeSet type_set;
    for (const InputArgumentType& type : column_type_lists[i]) {
      type_set.Insert(type);
    }
    const Type* supertype = nullptr;
    ZETASQL_RETURN_IF_ERROR(
        resolver_->coercer_.GetCommonSuperType(type_set, &supertype));
    if (supertype == nullptr) {
      return MakeSqlErrorAt(ast_input_location)
             << "Column " << column_identifier_in_error_string(i) << " in "
             << set_operation_->GetSQLForOperation()
             << " has incompatible types: "
             << InputArgumentType::ArgumentsToString(column_type_lists[i]);
    }

    std::string no_grouping_type;
    bool column_types_must_support_grouping =
        set_operation_->op_type() != ASTSetOperation::UNION ||
        set_operation_->distinct();
    if (column_types_must_support_grouping &&
        !supertype->SupportsGrouping(resolver_->language(),
                                     &no_grouping_type)) {
      return MakeSqlErrorAt(ast_input_location)
             << "Column " << column_identifier_in_error_string(i) << " in "
             << set_operation_->GetSQLForOperation()
             << " has type that does not support set operation comparisons: "
             << no_grouping_type;
    }
    supertypes.push_back(supertype);
  }
  return supertypes;
}

InputArgumentType Resolver::SetOperationResolver::GetColumnInputArgumentType(
    const ResolvedColumn& column, const ResolvedScan* resolved_scan) const {
  // If this column was computed, find the expr that computed it.
  const ResolvedExpr* expr = nullptr;
  if (resolved_scan->node_kind() == RESOLVED_PROJECT_SCAN) {
    expr = FindProjectExpr(resolved_scan->GetAs<ResolvedProjectScan>(), column);
  }
  if (expr != nullptr) {
    return GetInputArgumentTypeForExpr(
        expr, /*pick_default_type_for_untyped_expr=*/false);
  } else {
    return InputArgumentType(column.type());
  }
}

absl::StatusOr<ResolvedColumnList>
Resolver::SetOperationResolver::BuildFinalColumnList(
    const std::vector<IdString>& final_column_names,
    const std::vector<const Type*>& final_column_types) const {
  ZETASQL_RET_CHECK_EQ(final_column_names.size(), final_column_types.size());

  ResolvedColumnList column_list;
  column_list.reserve(final_column_types.size());

  for (int i = 0; i < final_column_names.size(); ++i) {
    column_list.push_back(ResolvedColumn(resolver_->AllocateColumnId(),
                                         op_type_str_, final_column_names[i],
                                         final_column_types[i]));
    resolver_->RecordColumnAccess(column_list.back());
  }
  return column_list;
}

std::vector<std::unique_ptr<const ResolvedSetOperationItem>>
Resolver::SetOperationResolver::ExtractSetOperationItems(
    absl::Span<ResolvedInputResult> resolved_inputs) const {
  std::vector<std::unique_ptr<const ResolvedSetOperationItem>>
      resolved_input_set_op_items;
  resolved_input_set_op_items.reserve(resolved_inputs.size());
  for (int i = 0; i < resolved_inputs.size(); ++i) {
    std::unique_ptr<ResolvedSetOperationItem> item =
        std::move(resolved_inputs[i].node);
    resolved_input_set_op_items.push_back(std::move(item));
  }
  return resolved_input_set_op_items;
}

std::vector<InputArgumentType>
Resolver::SetOperationResolver::BuildColumnTypeList(
    const ResolvedInputResult& resolved_input) const {
  std::vector<InputArgumentType> column_types;
  column_types.reserve(resolved_input.name_list->num_columns());
  for (const ResolvedColumn& column :
       resolved_input.name_list->GetResolvedColumns()) {
    column_types.push_back(
        GetColumnInputArgumentType(column, resolved_input.node->scan()));
  }
  return column_types;
}

Resolver::ValidateRecursiveTermVisitor::ValidateRecursiveTermVisitor(
    const Resolver* resolver, IdString recursive_query_name)
    : resolver_(resolver), recursive_query_name_(recursive_query_name) {}

absl::Status Resolver::ValidateRecursiveTermVisitor::DefaultVisit(
    const ResolvedNode* node) {
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  return absl::OkStatus();
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedAggregateScan(
    const ResolvedAggregateScan* node) {
  ++aggregate_scan_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --aggregate_scan_count_;
  return absl::OkStatus();
}

absl::Status
Resolver::ValidateRecursiveTermVisitor::VisitResolvedFunctionArgument(
    const ResolvedFunctionArgument* node) {
  ++tvf_argument_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --tvf_argument_count_;
  return absl::OkStatus();
}

absl::Status
Resolver::ValidateRecursiveTermVisitor::VisitResolvedLimitOffsetScan(
    const ResolvedLimitOffsetScan* node) {
  ++limit_offset_scan_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --limit_offset_scan_count_;
  return absl::OkStatus();
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedWithEntry(
    const ResolvedWithEntry* node) {
  ++nested_with_entry_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --nested_with_entry_count_;
  return absl::OkStatus();
}

absl::Status
Resolver::ValidateRecursiveTermVisitor::VisitResolvedSetOperationScan(
    const ResolvedSetOperationScan* node) {
  switch (node->op_type()) {
    case ResolvedSetOperationScan::EXCEPT_ALL:
      if (node->input_item_list_size() == 0) {
        return absl::OkStatus();
      }
      ZETASQL_RETURN_IF_ERROR(node->input_item_list(0)->Accept(this));
      ++except_clause_count_;
      for (int i = 1; i < node->input_item_list_size(); ++i) {
        ZETASQL_RETURN_IF_ERROR(node->input_item_list(i)->Accept(this));
      }
      --except_clause_count_;
      return absl::OkStatus();
    case ResolvedSetOperationScan::EXCEPT_DISTINCT:
    case ResolvedSetOperationScan::INTERSECT_DISTINCT:
    case ResolvedSetOperationScan::UNION_DISTINCT:
      ++setop_distinct_count_;
      ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
      --setop_distinct_count_;
      return absl::OkStatus();
    default:
      return node->ChildrenAccept(this);
  }
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedAnalyticScan(
    const ResolvedAnalyticScan* node) {
  ++analytic_scan_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --analytic_scan_count_;
  return absl::OkStatus();
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedSampleScan(
    const ResolvedSampleScan* node) {
  ++sample_scan_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --sample_scan_count_;
  return absl::OkStatus();
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedOrderByScan(
    const ResolvedOrderByScan* node) {
  ++order_by_scan_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --order_by_scan_count_;
  return absl::OkStatus();
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedSubqueryExpr(
    const ResolvedSubqueryExpr* node) {
  ++subquery_expr_count_;
  ZETASQL_RETURN_IF_ERROR(node->ChildrenAccept(this));
  --subquery_expr_count_;
  return absl::OkStatus();
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedRecursiveScan(
    const ResolvedRecursiveScan* node) {
  // If we have an inner recursive query, process only the query's non-recursive
  // term. As recursive queries are validated in a bottom-up fashon, the inner
  // query's recursive term has already been validated, and part of the
  // inner query's validation prevents its recursive term from referencing the
  // current (outer) query.
  ZETASQL_RETURN_IF_ERROR(node->non_recursive_term()->Accept(this));
  return absl::OkStatus();
}

int* Resolver::ValidateRecursiveTermVisitor::GetJoinCountField(
    const ResolvedJoinScan::JoinType join_type, bool left_operand) {
  switch (join_type) {
    case ResolvedJoinScan::LEFT:
      return left_operand ? nullptr : &right_operand_of_left_join_count_;
    case ResolvedJoinScan::RIGHT:
      return left_operand ? &left_operand_of_right_join_count_ : nullptr;
    case ResolvedJoinScan::FULL:
      return &full_join_operand_count_;
    case ResolvedJoinScan::INNER:
      return nullptr;
  }
}

void Resolver::ValidateRecursiveTermVisitor::MaybeAdjustJoinCount(
    const ResolvedJoinScan::JoinType join_type, bool left_operand, int offset) {
  int* field = GetJoinCountField(join_type, left_operand);
  if (field != nullptr) {
    (*field) += offset;
  }
}

absl::Status Resolver::ValidateRecursiveTermVisitor::VisitResolvedJoinScan(
    const ResolvedJoinScan* node) {
  // Process left operand
  MaybeAdjustJoinCount(node->join_type(), /*left_operand=*/true, 1);
  ZETASQL_RETURN_IF_ERROR(node->left_scan()->Accept(this));
  MaybeAdjustJoinCount(node->join_type(), /*left_operand=*/true, -1);

  // Process right operand
  MaybeAdjustJoinCount(node->join_type(), /*left_operand=*/false, 1);
  ZETASQL_RETURN_IF_ERROR(node->right_scan()->Accept(this));
  MaybeAdjustJoinCount(node->join_type(), /*left_operand=*/false, -1);

  // Process ON expression
  if (node->join_expr() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(node->join_expr()->Accept(this));
  }

  return absl::OkStatus();
}

absl::Status
Resolver::ValidateRecursiveTermVisitor::VisitResolvedRecursiveRefScan(
    const ResolvedRecursiveRefScan* node) {
  auto it = resolver_->recursive_ref_info_.find(node);
  ZETASQL_RET_CHECK(it != resolver_->recursive_ref_info_.end());
  const RecursiveRefScanInfo& info = it->second;
  std::string query_type =
      (recursive_query_name_.ToStringView() == "$view") ? "view" : "table";

  if (seen_recursive_reference_) {
    return MakeSqlErrorAt(info.path)
           << "Multiple recursive references to " << query_type << " '"
           << info.path->ToIdentifierPathString() << "' are not allowed";
  }
  seen_recursive_reference_ = true;

  if (!info.recursive_query_unique_name.Equals(recursive_query_name_)) {
    return MakeSqlErrorAt(info.path)
           << query_type << " '" << info.path->ToIdentifierPathString()
           << "' may not be recursively referenced from inside an "
              "inner WITH entry";
  }

  if (nested_with_entry_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << query_type << " '" << info.path->ToIdentifierPathString()
           << "' may not be recursively referenced from inside an "
              "inner WITH entry";
  }

  if (subquery_expr_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A recursive reference from inside an expression subquery is not "
              "allowed";
  }

  if (aggregate_scan_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A subquery containing a recursive reference may not use "
              "DISTINCT, GROUP BY, or any aggregate function";
  }

  if (setop_distinct_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A subquery containing a recursive reference may not use "
              "INTERSECT, UNION, or EXCEPT with the DISTINCT modifier";
  }

  if (analytic_scan_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A subquery containing a recursive reference may not use an "
              "analytic function";
  }

  if (sample_scan_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A subquery containing a recursive reference may not use the "
              "TABLESAMPLE operator";
  }

  if (order_by_scan_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A subquery containing a recursive reference may not contain an "
              "ORDER BY clause";
  }

  if (limit_offset_scan_count_ > 0) {
    return MakeSqlErrorAt(info.path) << "A query containing a recursive "
                                        "reference may not use a LIMIT clause";
  }

  if (right_operand_of_left_join_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A query containing a recursive reference may not be used as the "
              "right operand of a LEFT JOIN";
  }

  if (left_operand_of_right_join_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A query containing a recursive reference may not be used as the "
              "left operand of a RIGHT JOIN";
  }

  if (full_join_operand_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A query containing a recursive reference may not be used as an "
              "operand of a FULL OUTER JOIN";
  }

  if (tvf_argument_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A query containing a recursive reference may not be used as an "
              "argument to a table-valued function";
  }

  if (except_clause_count_ > 0) {
    return MakeSqlErrorAt(info.path)
           << "A subquery containing a recursive reference may not be used as "
              "the right operand of EXCEPT";
  }

  return absl::OkStatus();
}

static bool IsBasicCorrespondingEnabled(
    const LanguageOptions& language_options) {
  return language_options.LanguageFeatureEnabled(FEATURE_V_1_4_CORRESPONDING) ||
         language_options.LanguageFeatureEnabled(
             FEATURE_V_1_4_CORRESPONDING_FULL);
}

absl::Status
Resolver::SetOperationResolver::ValidateNoCorrespondingForRecursive() const {
  ZETASQL_RET_CHECK_NE(set_operation_->metadata(), nullptr);
  for (const auto* metadata :
       set_operation_->metadata()->set_operation_metadata_list()) {
    if (!IsBasicCorrespondingEnabled(resolver_->language())) {
      if (metadata->column_match_mode() != nullptr) {
        return MakeSqlErrorAt(metadata->column_match_mode())
               << "CORRESPONDING and CORRESPONDING BY for WITH RECURSIVE are "
               << "not supported";
      }
      if (metadata->column_propagation_mode() != nullptr) {
        return MakeSqlErrorAt(metadata->column_propagation_mode())
               << "Column propagation mode (FULL/LEFT/STRICT) for WITH "
               << "RECURSIVE are not supported";
      }
      continue;
    }
    if (metadata->column_match_mode() != nullptr) {
      return MakeSqlErrorAt(metadata->column_match_mode())
             << "CORRESPONDING for set operations cannot be used in WITH "
             << "RECURSIVE";
    }
    if (metadata->column_propagation_mode() != nullptr) {
      return MakeSqlErrorAt(metadata->column_propagation_mode())
             << ColumnPropagationModeToString(
                    metadata->column_propagation_mode()->value())
             << " cannot be in WITH RECURSIVE";
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::SetOperationResolver::ResolveRecursive(
    const NameScope* scope, const std::vector<IdString>& recursive_alias,
    const IdString& recursive_query_unique_name,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  ZETASQL_RET_CHECK_GE(set_operation_->inputs().size(), 2);

  ZETASQL_RETURN_IF_ERROR(ValidateHint());
  ZETASQL_RETURN_IF_ERROR(ValidateNoCorrespondingForRecursive());
  ZETASQL_RETURN_IF_ERROR(ValidateIdenticalSetOperator());
  ResolvedRecursiveScan::RecursiveSetOperationType recursive_op_type;
  ZETASQL_RETURN_IF_ERROR(GetRecursiveScanEnumType(set_operation_, &recursive_op_type));

  // Register a NULL entry for the named subquery so that any references to it
  // from within the non-recursive term result in an error. We'll change this
  // to an actual NamedSubquery object when the recursive term resolves.
  resolver_->named_subquery_map_[recursive_alias].push_back(nullptr);

  // The standard requires that any recursion be confined to the rhs of the
  // UNION. However, the ZetaSQL parser collapses chains of UNION's like:
  //   SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 ...
  // into a single ASTSetOperation node, which is left-associative. When this
  // happens, we will treat the UNION of all terms except the last as the
  // overall non-recursive term, and the last term of the UNION as the overall
  // recursive term. To override and include an inner UNION within the recursive
  // term, the user will need to provide explicit parentheses.
  std::vector<ResolvedInputResult> nonrecursive_resolved_inputs;
  int num_nonrecursive_inputs =
      static_cast<int>(set_operation_->inputs().size() - 1);
  for (int idx = 0; idx < num_nonrecursive_inputs; ++idx) {
    ZETASQL_ASSIGN_OR_RETURN(nonrecursive_resolved_inputs.emplace_back(),
                     ResolveInputQuery(scope, idx,
                                       /*inferred_type_for_query=*/nullptr));
  }

  ZETASQL_RET_CHECK_EQ(set_operation_->inputs().size() - 1,
               nonrecursive_resolved_inputs.size());

  // Determine the UNION's column list and name list using only the
  // non-recursive terms.
  // TODO: Add a branch to handle CORRESPONDING when we are ready
  // to support CORRESPONDING in WITH RECURSIVE.
  ResolvedColumnList column_list;
  {
    ZETASQL_RETURN_IF_ERROR(CheckSameColumnNumber(nonrecursive_resolved_inputs));
    std::vector<std::vector<InputArgumentType>> column_type_lists;
    ZETASQL_ASSIGN_OR_RETURN(column_type_lists,
                     BuildColumnTypeListsByPosition(
                         absl::MakeSpan(nonrecursive_resolved_inputs)));
    ZETASQL_ASSIGN_OR_RETURN(std::vector<const Type*> super_types,
                     GetSuperTypes(column_type_lists, [](int column_idx) {
                       return absl::StrCat(column_idx + 1);
                     }));
    ZETASQL_ASSIGN_OR_RETURN(
        column_list,
        BuildFinalColumnList(
            nonrecursive_resolved_inputs.front().name_list->GetColumnNames(),
            super_types));
  }

  // Type coercion, if needed.
  for (int i = 0; i < nonrecursive_resolved_inputs.size(); ++i) {
    ResolvedSetOperationItem* item = nonrecursive_resolved_inputs[i].node.get();
    ZETASQL_RETURN_IF_ERROR(
        CreateWrapperScanWithCastsForSetOperationItem(column_list, i, item));
  }

  std::vector<std::unique_ptr<const ResolvedSetOperationItem>>
      resolved_nonrecursive_input_set_op_items = ExtractSetOperationItems(
          absl::MakeSpan(nonrecursive_resolved_inputs));

  // Create the recursive scan with the non-recursive term and propagate
  // annotations to output columns of the recursive scan before we register the
  // subquery, so that the recursive term can be resolved with column list of
  // proper annotations.
  std::unique_ptr<ResolvedRecursiveScan> recursive_scan;
  if (nonrecursive_resolved_inputs.size() == 1) {
    recursive_scan = MakeResolvedRecursiveScan(
        column_list, recursive_op_type,
        std::move(resolved_nonrecursive_input_set_op_items.at(0)),
        /*recursive_term=*/nullptr);
  } else {
    // If we have multiple non-recursive operands, wrap them in an inner UNION
    // so that the ResolvedRecursive scan can have just one non-recursive
    // operand.
    ResolvedSetOperationScan::SetOperationType op_type;
    ZETASQL_RETURN_IF_ERROR(GetSetScanEnumType(set_operation_, &op_type));

    // Clone the column list so that the columns in the inner set operation used
    // for the non-recursive term have unique ids.
    ResolvedColumnList inner_set_op_column_list;
    for (const auto& column : column_list) {
      inner_set_op_column_list.push_back(
          ResolvedColumn(resolver_->AllocateColumnId(), op_type_str_,
                         column.name_id(), column.annotated_type()));
      resolver_->RecordColumnAccess(inner_set_op_column_list.back());
    }

    ZETASQL_ASSIGN_OR_RETURN(
        std::unique_ptr<const ResolvedSetOperationScan> set_op_scan,
        ResolvedSetOperationScanBuilder()
            .set_column_list(inner_set_op_column_list)
            .set_op_type(op_type)
            .set_input_item_list(
                std::move(resolved_nonrecursive_input_set_op_items))
            .set_column_match_mode(
                GetResolvedSetOperationColumnMatchMode(ASTColumnMatchMode()))
            .set_column_propagation_mode(
                GetResolvedSetOperationColumnPropagationMode(
                    ASTColumnPropagationMode()))
            .Build());
    ZETASQL_RETURN_IF_ERROR(resolver_->CheckAndPropagateAnnotations(
        set_operation_,
        const_cast<ResolvedSetOperationScan*>(set_op_scan.get())));

    // Clone column list so that the output columns have correct annotations.
    ZETASQL_RET_CHECK_EQ(inner_set_op_column_list.size(),
                 set_op_scan->column_list_size());
    ResolvedColumnList non_recursive_term_output_list;
    non_recursive_term_output_list.reserve(inner_set_op_column_list.size());
    for (int i = 0; i < inner_set_op_column_list.size(); ++i) {
      const auto& column = inner_set_op_column_list.at(i);
      non_recursive_term_output_list.push_back(
          ResolvedColumn(column.column_id(), op_type_str_, column.name_id(),
                         set_op_scan->column_list(i).annotated_type()));
    }
    auto non_recursive_operand = MakeResolvedSetOperationItem(
        std::move(set_op_scan), non_recursive_term_output_list);
    recursive_scan = MakeResolvedRecursiveScan(column_list, recursive_op_type,
                                               std::move(non_recursive_operand),
                                               /*recursive_term=*/nullptr);
  }
  // Propagate annotations from non-recursive term to the recursive scan.
  ZETASQL_RETURN_IF_ERROR(resolver_->CheckAndPropagateAnnotations(
      set_operation_, recursive_scan.get()));

  std::shared_ptr<const NameList> final_name_list;
  ZETASQL_ASSIGN_OR_RETURN(
      final_name_list,
      BuildFinalNameList(*nonrecursive_resolved_inputs.front().name_list,
                         recursive_scan->column_list()));

  // Register the WITH entry so that self-references in the recursive term can
  // resolve correctly, with column list of proper annotations.
  resolver_->named_subquery_map_.at(recursive_alias).back() =
      std::make_unique<NamedSubquery>(recursive_query_unique_name,
                                      /*recursive_in=*/true,
                                      recursive_scan->column_list(),
                                      final_name_list);

  // Resolve the recursive term.
  ZETASQL_ASSIGN_OR_RETURN(
      ResolvedInputResult resolved_recursive_input,
      ResolveInputQuery(scope,
                        static_cast<int>(set_operation_->inputs().size() - 1),
                        /*inferred_type_for_query=*/nullptr));

  // The recursive query is now resolved; clear the 'recursive' flag in
  // named_subquery_map_ so that future references simply resolve like an
  // ordinary WITH entry.
  resolver_->named_subquery_map_[recursive_alias].back()->is_recursive = false;

  if (resolved_recursive_input.name_list->num_columns() != column_list.size()) {
    return MakeSqlErrorAt(set_operation_->inputs().back())
           << "Queries in " << set_operation_->GetSQLForOperation()
           << " have mismatched column count; query 1"
           << FormatColumnCount(*nonrecursive_resolved_inputs.front().name_list)
           << ", query " << (nonrecursive_resolved_inputs.size() + 1)
           << FormatColumnCount(*resolved_recursive_input.name_list);
  }

  ValidateRecursiveTermVisitor validation_visitor(resolver_,
                                                  recursive_query_unique_name);
  ZETASQL_RETURN_IF_ERROR(resolved_recursive_input.node->Accept(&validation_visitor));

  // Obtain the type of each column in the recursive term and verify that it
  // is coercible to the corresponding column type based on the non-recursive
  // terms.
  // TODO: Add a branch to handle CORRESPONDING when we are ready
  // to support CORRESPONDING in WITH RECURSIVE.
  std::vector<InputArgumentType> recursive_column_type_list =
      BuildColumnTypeList(resolved_recursive_input);

  for (int i = 0; i < recursive_scan->column_list().size(); ++i) {
    SignatureMatchResult result;
    if (!resolver_->coercer_.CoercesTo(
            recursive_column_type_list[i],
            recursive_scan->column_list().at(i).type(),
            /*is_explicit=*/false, &result)) {
      return MakeSqlErrorAt(set_operation_->inputs().back())
             << "Cannot coerce column " << (i + 1) << " of recursive term "
             << "("
             << recursive_column_type_list[i].type()->ShortTypeName(
                    resolver_->analyzer_options_.language().product_mode())
             << ") to column type in non-recursive term ( "
             << recursive_scan->column_list().at(i).type()->ShortTypeName(
                    resolver_->analyzer_options_.language().product_mode())
             << ")";
    }
  }

  // Add casts as needed to ensure that every column produced by the recursive
  // term is the correct type (we already verified that the casts are legal
  // through implicit coercion).
  // TODO: The error location remains at the 0-th item to preserve
  // the original code behavior, but it should point to the last input instead.
  ZETASQL_RETURN_IF_ERROR(CreateWrapperScanWithCastsForSetOperationItem(
      column_list, /*item_index=*/0, resolved_recursive_input.node.get()));

  // Set resolved input on the ResolvedRecursiveScan node.
  recursive_scan->set_recursive_term(std::move(resolved_recursive_input.node));
  // Resolve the ResolvedOption (Query Hint), if present.
  if (set_operation_->hint() != nullptr) {
    std::vector<std::unique_ptr<const ResolvedOption>> hint_list;
    ZETASQL_RETURN_IF_ERROR(
        resolver_->ResolveHintAndAppend(set_operation_->hint(), &hint_list));
    recursive_scan->set_hint_list(std::move(hint_list));
  }
  // TODO: Update `final_name_list` again after this annotation
  // propagation. Context: We should update final_column_list whenever
  // annotations are propagated because it is possible that the merged
  // annotation is becomes different. In this specific case, however, it "ok" to
  // not update (for now) because WITH RECURSIVE has the check that the
  // recursive and non-recursive terms have the same annotations; if the merged
  // annotation changes, the annotations of the recursive and non-recursive term
  // must be different and a SQL error will be thrown.
  ZETASQL_RETURN_IF_ERROR(resolver_->CheckAndPropagateAnnotations(
      set_operation_, recursive_scan.get()));

  // Check output columns in non-recursive term and recursive term have the same
  // annotation.
  // TODO: Support queries where output columns in non-recursive
  // term and recursive term have different collations.
  ZETASQL_RET_CHECK_EQ(recursive_scan->non_recursive_term()->output_column_list_size(),
               recursive_scan->recursive_term()->output_column_list_size());
  for (int i = 0;
       i < recursive_scan->non_recursive_term()->output_column_list_size();
       i++) {
    const AnnotationMap* non_recursive_column_annotation =
        recursive_scan->non_recursive_term()
            ->output_column_list(i)
            .type_annotation_map();
    const AnnotationMap* recursive_column_annotation =
        recursive_scan->recursive_term()
            ->output_column_list(i)
            .type_annotation_map();
    // TODO: Consider change the error message to be type mismatch
    // with details, e.g., "a has type STRING while b has STRING COLLATE
    // 'und:ci'"
    if (!AnnotationMap::HasEqualAnnotations(
            non_recursive_column_annotation, recursive_column_annotation,
            zetasql::CollationAnnotation::GetId())) {
      return MakeSqlErrorAt(set_operation_->inputs().back())
             << "Collation conflict: \""
             << (non_recursive_column_annotation == nullptr
                     ? "null"
                     : non_recursive_column_annotation->DebugString())
             << "\" vs. \""
             << (recursive_column_annotation == nullptr
                     ? "null"
                     : recursive_column_annotation->DebugString())
             << "\"; in column " << (i + 1) << " of recursive scan";
    }
  }

  // Update named subquery column list.
  resolver_->named_subquery_map_[recursive_alias].back()->column_list =
      recursive_scan->column_list();
  *output = std::move(recursive_scan);
  *output_name_list = final_name_list;

  return absl::OkStatus();
}

absl::Status Resolver::SetOperationResolver::FinishResolveRecursionWithModifier(
    const ASTNode* ast_location, const std::vector<IdString>& recursive_alias,
    std::unique_ptr<const ResolvedRecursionDepthModifier> depth_modifier,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  ZETASQL_RET_CHECK((*output)->Is<ResolvedRecursiveScan>());

  const auto& name_list = *output_name_list;
  if (name_list->is_value_table()) {
    return MakeSqlErrorAt(ast_location)
           << "WITH DEPTH modifier is not allowed when the recursive query "
              "produces a value table.";
  }

  // Handles the case when the recursion depth column alias is ambiguous.
  const ResolvedColumn& depth_column =
      depth_modifier->recursion_depth_column()->column();
  NameTarget name_target;
  if (name_list->LookupName(depth_column.name_id(), &name_target)) {
    return MakeSqlErrorAt(ast_location)
           << "WITH DEPTH modifier depth column is named "
           << ToSingleQuotedStringLiteral(depth_column.name_id().ToStringView())
           << " which collides with one of the existing names.";
  }

  // Adds the recursion depth column to output name list.
  auto name_list_with_modifier = std::make_shared<NameList>();
  ZETASQL_RETURN_IF_ERROR(name_list_with_modifier->MergeFrom(*name_list, ast_location));
  ZETASQL_RETURN_IF_ERROR(name_list_with_modifier->AddColumn(depth_column.name_id(),
                                                     depth_column,
                                                     /*is_explicit=*/true));

  // Updates the resolved ast to add depth column to the column list.
  ZETASQL_ASSIGN_OR_RETURN(
      auto recursive_scan,
      ToBuilder(
          absl::WrapUnique(output->release()->GetAs<ResolvedRecursiveScan>()))
          .set_recursion_depth_modifier(std::move(depth_modifier))
          .add_column_list(depth_column)
          .Build());

  // Updates the named subquery corresponding to the recursive subquery.
  auto& named_subquery = resolver_->named_subquery_map_[recursive_alias].back();
  auto modified_named_subquery = std::make_unique<NamedSubquery>(
      named_subquery->unique_alias, named_subquery->is_recursive,
      recursive_scan->column_list(), std::move(name_list_with_modifier));
  named_subquery = std::move(modified_named_subquery);

  *output_name_list = std::move(name_list_with_modifier);
  *output = std::move(recursive_scan);
  return absl::OkStatus();
}

absl::StatusOr<Resolver::SetOperationResolver::ResolvedInputResult>
Resolver::SetOperationResolver::ResolveInputQuery(
    const NameScope* scope, int query_index,
    const Type* inferred_type_for_query) const {
  ZETASQL_RET_CHECK_GE(query_index, 0);
  ZETASQL_RET_CHECK_LT(query_index, set_operation_->inputs().size());
  const IdString query_alias = resolver_->MakeIdString(
      absl::StrCat(op_type_str_.ToStringView(), query_index + 1));

  ResolvedInputResult result;
  std::unique_ptr<const ResolvedScan> resolved_scan;
  ZETASQL_RETURN_IF_ERROR(resolver_->ResolveQueryExpression(
      set_operation_->inputs()[query_index], scope, query_alias,
      /*force_new_columns_for_projected_outputs=*/false, &resolved_scan,
      &result.name_list, inferred_type_for_query));

  // Columns in set operations are matched positionally, so don't let
  // columns get pruned.
  resolver_->RecordColumnAccess(result.name_list->GetResolvedColumns());

  result.node = MakeResolvedSetOperationItem(
      std::move(resolved_scan), result.name_list->GetResolvedColumns());
  return result;
}

absl::StatusOr<std::vector<std::vector<InputArgumentType>>>
Resolver::SetOperationResolver::BuildColumnTypeListsByPosition(
    absl::Span<ResolvedInputResult> resolved_inputs) const {
  std::vector<std::vector<InputArgumentType>> column_type_lists;
  column_type_lists.resize(resolved_inputs.front().name_list->num_columns());

  for (int idx = 0; idx < resolved_inputs.size(); ++idx) {
    const ResolvedScan* resolved_scan = resolved_inputs[idx].node->scan();
    const NameList& curr_name_list = *resolved_inputs.at(idx).name_list;
    for (int i = 0; i < curr_name_list.num_columns(); ++i) {
      const ResolvedColumn& column = curr_name_list.column(i).column();
      column_type_lists[i].push_back(
          GetColumnInputArgumentType(column, resolved_scan));
    }
  }
  return column_type_lists;
}

absl::Status
Resolver::SetOperationResolver::CreateWrapperScanWithCastsForSetOperationItem(
    const ResolvedColumnList& column_list, int item_index,
    ResolvedSetOperationItem* set_operation_item) const {
  std::unique_ptr<const ResolvedScan> resolved_scan =
      set_operation_item->release_scan();

  ZETASQL_RETURN_IF_ERROR(resolver_->CreateWrapperScanWithCasts(
      set_operation_->inputs()[item_index], column_list,
      resolver_->MakeIdString(
          absl::StrCat(op_type_str_.ToStringView(), item_index + 1, "_cast")),
      &resolved_scan, set_operation_item->mutable_output_column_list()));

  set_operation_item->set_scan(std::move(resolved_scan));
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<const NameList>>
Resolver::SetOperationResolver::BuildFinalNameList(
    const NameList& name_list_template,
    const ResolvedColumnList& final_column_list) const {
  ZETASQL_RET_CHECK_EQ(name_list_template.num_columns(), final_column_list.size());
  std::shared_ptr<NameList> name_list(new NameList);
  for (int i = 0; i < final_column_list.size(); ++i) {
    const IdString name = name_list_template.column(i).name();
    ZETASQL_RETURN_IF_ERROR(name_list->AddColumnMaybeValueTable(
        name, final_column_list.at(i),
        name_list_template.column(i).is_explicit(), set_operation_,
        name_list_template.is_value_table()));
  }

  if (name_list_template.is_value_table()) {
    ZETASQL_RETURN_IF_ERROR(name_list->SetIsValueTable());
  }
  return name_list;
}

absl::Status Resolver::SetOperationResolver::ValidateHint() const {
  ZETASQL_RET_CHECK_NE(set_operation_->metadata(), nullptr);
  const absl::Span<const ASTSetOperationMetadata* const> metadata_list =
      set_operation_->metadata()->set_operation_metadata_list();
  for (int i = 0; i < metadata_list.size(); ++i) {
    const ASTSetOperationMetadata* metadata = metadata_list[i];
    if (i > 0 && metadata->hint() != nullptr) {
      return MakeSqlErrorAt(metadata->hint())
             << "Syntax error: Hints on set operations must appear on the "
                "first  operation.";
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::SetOperationResolver::ValidateIdenticalSetOperator()
    const {
  ZETASQL_RET_CHECK_NE(set_operation_->metadata(), nullptr);
  const auto& metadata_list =
      set_operation_->metadata()->set_operation_metadata_list();
  ZETASQL_RET_CHECK(!metadata_list.empty());

  const ASTSetOperation::OperationType op_type =
      metadata_list[0]->op_type()->value();
  const ASTSetOperation::AllOrDistinct all_or_distinct =
      metadata_list[0]->all_or_distinct()->value();
  const ASTSetOperation::ColumnMatchMode column_match_mode =
      GetColumnMatchMode(*metadata_list[0]);
  const ASTSetOperation::ColumnPropagationMode column_propagation_mode =
      GetColumnPropagationMode(*metadata_list[0]);
  const std::vector<IdString> by_list =
      GetCorrespondingByIdStrings(*metadata_list[0]);

  for (int i = 1; i < metadata_list.size(); ++i) {
    const ASTSetOperationMetadata* metadata = metadata_list[i];
    if (metadata->op_type()->value() != op_type ||
        metadata->all_or_distinct()->value() != all_or_distinct) {
      // We report the error at metadata->op_type(), even if it is the distinct
      // that is different, to align with the error message from the parser for
      // `query_set_operation_prefix`.
      // TODO: Remove the "Syntax error" prefix and update all the
      // affected tests.
      return MakeSqlErrorAt(metadata->op_type())
             << "Syntax error: Different set operations cannot be used in the "
                "same query without using parentheses for grouping";
    }

    if (column_match_mode != GetColumnMatchMode(*metadata)) {
      const ASTNode* err_location = metadata->column_match_mode() == nullptr
                                        ? metadata_list[0]->column_match_mode()
                                        : metadata->column_match_mode();
      return MakeSqlErrorAt(err_location)
             << "Different column match modes cannot be used in the same "
                "query without using parentheses for grouping";
    }

    if (column_propagation_mode != GetColumnPropagationMode(*metadata)) {
      const ASTNode* err_location =
          metadata->column_propagation_mode() == nullptr
              ? metadata_list[0]->column_propagation_mode()
              : metadata->column_propagation_mode();
      return MakeSqlErrorAt(err_location)
             << "Different column propagation modes (FULL/LEFT/STRICT) cannot "
                "be used in the same query without using parentheses for "
                "grouping";
    }

    if (!IsSameByList(by_list, GetCorrespondingByIdStrings(*metadata))) {
      const ASTNode* err_location = metadata->corresponding_by_column_list();
      return MakeSqlErrorAt(err_location)
             << "Different CORRESPONDING BY lists cannot be used in the same "
                "query without using parentheses for grouping";
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::SetOperationResolver::ValidateCorresponding() const {
  ZETASQL_RET_CHECK_NE(set_operation_->metadata(), nullptr);
  for (const auto* metadata :
       set_operation_->metadata()->set_operation_metadata_list()) {
    // Verify that the related language features are turned on if
    // column_match_mode and column_propagation_mode are specified.
    {
      const ASTSetOperationColumnMatchMode* column_match_mode =
          metadata->column_match_mode();
      if (column_match_mode != nullptr) {
        if (!IsBasicCorrespondingEnabled(resolver_->language()) &&
            column_match_mode->value() == ASTSetOperation::CORRESPONDING) {
          return MakeSqlErrorAt(column_match_mode)
                 << "CORRESPONDING for set operations is not supported";
        }
        if (!resolver_->language().LanguageFeatureEnabled(
                FEATURE_V_1_4_CORRESPONDING_FULL) &&
            column_match_mode->value() == ASTSetOperation::CORRESPONDING_BY) {
          return MakeSqlErrorAt(column_match_mode)
                 << "CORRESPONDING BY for set operations is not supported";
        }
      }

      const ASTSetOperationColumnPropagationMode* column_propagation_mode =
          metadata->column_propagation_mode();
      if (!resolver_->language().LanguageFeatureEnabled(
              FEATURE_V_1_4_CORRESPONDING_FULL) &&
          column_propagation_mode != nullptr) {
        return MakeSqlErrorAt(metadata->column_propagation_mode())
               << "Column propagation mode (FULL/LEFT/STRICT) for set "
               << "operations are not supported";
      }
    }

    ASTSetOperation::ColumnMatchMode match_mode = GetColumnMatchMode(*metadata);
    ASTSetOperation::ColumnPropagationMode propagation_mode =
        GetColumnPropagationMode(*metadata);
    switch (match_mode) {
      case ASTSetOperation::BY_POSITION:
        // No column_propagation_mode is allowed to be specified when column
        // match mode is BY_POSITION, although we default
        // `column_propagation_mode` to STRICT in analyzer for BY_POSITION.
        if (metadata->column_propagation_mode() != nullptr) {
          return MakeSqlErrorAt(metadata->column_propagation_mode())
                 << ColumnPropagationModeToString(propagation_mode)
                 << " in set operations cannot be used without CORRESPONDING";
        }
        break;
      case ASTSetOperation::CORRESPONDING:
      case ASTSetOperation::CORRESPONDING_BY:
        break;
    }
  }
  return absl::OkStatus();
}

// Note that we allow set operations between value tables and regular
// tables with exactly one column.  The output will be a value table if
// the first subquery was a value table.
absl::Status Resolver::ResolveSetOperation(
    const ASTSetOperation* set_operation, const NameScope* scope,
    const Type* inferred_type_for_query,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  SetOperationResolver resolver(set_operation, this);
  return resolver.Resolve(scope, inferred_type_for_query, output,
                          output_name_list);
}

absl::Status Resolver::ValidateIntegerParameterOrLiteral(
    const char* clause_name, const ASTNode* ast_location,
    const ResolvedExpr& expr) const {
  if ((expr.node_kind() != RESOLVED_PARAMETER &&
       expr.node_kind() != RESOLVED_LITERAL) ||
      !expr.type()->IsInteger()) {
    return MakeSqlErrorAt(ast_location)
           << clause_name << " expects an integer literal or parameter";
  }
  return absl::OkStatus();
}

absl::Status Resolver::ValidateParameterOrLiteralAndCoerceToInt64IfNeeded(
    const char* clause_name, const ASTNode* ast_location,
    std::unique_ptr<const ResolvedExpr>* expr) const {
  if ((*expr)->type()->IsInt64() && (*expr)->node_kind() == RESOLVED_CAST) {
    // We allow CAST(<expr> AS INT64), as long as <expr> is a parameter
    // or literal.
    ZETASQL_RETURN_IF_ERROR(ValidateIntegerParameterOrLiteral(
        clause_name, ast_location, *(*expr)->GetAs<ResolvedCast>()->expr()));
  } else {
    ZETASQL_RETURN_IF_ERROR(
        ValidateIntegerParameterOrLiteral(clause_name, ast_location, **expr));
  }
  // kExplicitCoercion is safe to use here because the above lines validate the
  // type. If those restrictions are relaxed we might need to do something
  // differently.
  ZETASQL_RETURN_IF_ERROR(CoerceExprToType(ast_location, type_factory_->get_int64(),
                                   kExplicitCoercion, expr));

  if ((*expr)->node_kind() == RESOLVED_LITERAL) {
    // If a literal, we can also validate its value.
    const Value& value = (*expr)->GetAs<ResolvedLiteral>()->value();
    if (value.is_null()) {
      return MakeSqlErrorAt(ast_location) << clause_name << " must not be null";
    }
    if (value.int64_value() < 0) {
      return MakeSqlErrorAt(ast_location)
             << clause_name
             << " expects a non-negative integer literal or parameter";
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveLimitOrOffsetExpr(
    const ASTExpression* ast_expr, const char* clause_name,
    ExprResolutionInfo* expr_resolution_info,
    std::unique_ptr<const ResolvedExpr>* resolved_expr) {
  ZETASQL_RETURN_IF_ERROR(ResolveExpr(ast_expr, expr_resolution_info, resolved_expr));
  ABSL_DCHECK(resolved_expr != nullptr);

  if (!language().LanguageFeatureEnabled(
          FEATURE_V_1_4_LIMIT_OFFSET_EXPRESSIONS)) {
    return ValidateParameterOrLiteralAndCoerceToInt64IfNeeded(
        clause_name, ast_expr, resolved_expr);
  }

  ZETASQL_RETURN_IF_ERROR(CoerceExprToType(
      ast_expr, type_factory_->get_int64(), kImplicitCoercion,
      [](absl::string_view target_type_name,
         absl::string_view actual_type_name) {
        return absl::StrCat("LIMIT ... OFFSET ... expects ", target_type_name,
                            ", got ", actual_type_name);
      },
      resolved_expr));
  ZETASQL_ASSIGN_OR_RETURN(bool is_constant_expr,
                   IsConstantExpression(resolved_expr->get()));
  if (!is_constant_expr) {
    return MakeSqlErrorAt(ast_expr)
           << clause_name << " expression must be constant";
  }

  return absl::OkStatus();
}

absl::Status Resolver::ResolveHavingModifier(
    const ASTHavingModifier* ast_having_modifier,
    ExprResolutionInfo* expr_resolution_info,
    std::unique_ptr<const ResolvedAggregateHavingModifier>* resolved_having) {
  std::unique_ptr<const ResolvedExpr> resolved_expr;
  ZETASQL_RETURN_IF_ERROR(ResolveExpr(ast_having_modifier->expr(), expr_resolution_info,
                              &resolved_expr));
  // TODO: Introduce a feature to support ARRAY type in HAVING
  // The HAVING MIN/MAX expression type must support ordering, and it cannot
  // be an array since MIN/MAX is currently undefined for arrays (even if
  // the array element supports MIN/MAX).
  if (!resolved_expr->type()->SupportsOrdering(language(),
                                               /*type_description=*/nullptr) ||
      resolved_expr->type()->IsArray()) {
    return MakeSqlErrorAt(ast_having_modifier)
           << "HAVING modifier does not support expressions of type "
           << resolved_expr->type()->ShortTypeName(product_mode());
  }

  if (language().LanguageFeatureEnabled(FEATURE_DISALLOW_GROUP_BY_FLOAT) &&
      resolved_expr->type()->IsFloatingPoint()) {
    return MakeSqlErrorAt(ast_having_modifier)
           << "HAVING modifier does not support expressions of type "
           << resolved_expr->type()->ShortTypeName(product_mode());
  }

  ABSL_DCHECK(resolved_having != nullptr);
  ResolvedAggregateHavingModifier::HavingModifierKind kind;
  if (ast_having_modifier->modifier_kind() ==
      ASTHavingModifier::ModifierKind::MAX) {
    kind = ResolvedAggregateHavingModifier::MAX;
  } else {
    kind = ResolvedAggregateHavingModifier::MIN;
  }
  *resolved_having =
      MakeResolvedAggregateHavingModifier(kind, std::move(resolved_expr));
  return absl::OkStatus();
}

// Resolves a LimitOffsetScan.
// If an OFFSET is not supplied, then the default value, 0, is used.
// It is an error to not supply a LIMIT.
absl::Status Resolver::ResolveLimitOffsetScan(
    const ASTLimitOffset* limit_offset, const NameScope* name_scope,
    std::unique_ptr<const ResolvedScan>* scan) {
  ZETASQL_RET_CHECK(limit_offset->limit() != nullptr);
  return ResolveLimitOffsetScan(limit_offset->limit(), limit_offset->offset(),
                                name_scope, scan);
}

// Resolves a LimitOffsetScan.
// If an OFFSET is not supplied, then a default value of 0 is used.
// If a LIMIT is not supplied then a default value of kint64max is used.
absl::Status Resolver::ResolveLimitOffsetScan(
    const ASTExpression* limit, const ASTExpression* offset,
    const NameScope* name_scope, std::unique_ptr<const ResolvedScan>* scan) {
  // LIMIT and OFFSET cannot reference the current name scope. Theoretically
  // they can reference the parent scope. Practically this makes no difference
  // than passing in an empty scope because LIMIT and OFFSET can currently only
  // take constant expressions and correlated expressions are not that. We pass
  // in the parent scope here to get a better error message, and allow easier
  // migration when correlated column references are supported in the future.
  ExprResolutionInfo expr_resolution_info(name_scope->previous_scope(),
                                          "LIMIT OFFSET");

  // Resolve and validate the LIMIT.
  std::unique_ptr<const ResolvedExpr> limit_expr;
  if (limit == nullptr) {
    // There is no specific location that we can associate with the virtual
    // literal so we set `ast_location` to nullptr.
    limit_expr = MakeResolvedLiteral(
        /*ast_location=*/nullptr,
        Value::Int64(std::numeric_limits<int64_t>::max()));
  } else {
    ZETASQL_RETURN_IF_ERROR(ResolveLimitOrOffsetExpr(
        limit,
        /*clause_name=*/"LIMIT", &expr_resolution_info, &limit_expr));
  }

  // Resolve and validate the OFFSET.
  std::unique_ptr<const ResolvedExpr> offset_expr;
  if (offset != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveLimitOrOffsetExpr(
        offset, /*clause_name=*/"OFFSET", &expr_resolution_info, &offset_expr));
  }

  const std::vector<ResolvedColumn>& column_list = (*scan)->column_list();
  *scan = MakeResolvedLimitOffsetScan(column_list, std::move(*scan),
                                      std::move(limit_expr),
                                      std::move(offset_expr));
  return absl::OkStatus();
}

// Tries to return the AST node corresponding to <column_index> in the
// input <ast_location>.  <num_columns> is the expected total number of
// columns.
static const ASTNode* GetASTNodeForColumn(const ASTNode* ast_location,
                                          int column_index, int num_columns) {
  if (ast_location->node_kind() == AST_QUERY) {
    ast_location = ast_location->GetAsOrDie<ASTQuery>()->query_expr();
  }
  if (ast_location->node_kind() == AST_SELECT) {
    // We can only find a specific column if the column list matches 1:1 with
    // output columns.  If there is a SELECT *, the column indexes won't match.
    // Star can never expand to zero columns so if parse node has N children
    // and we have N columns, we know they must match 1:1.
    const ASTSelectList* select_list =
        ast_location->GetAsOrDie<ASTSelect>()->select_list();
    if (select_list->columns().size() == num_columns) {
      ast_location = select_list->columns(column_index);
    }
  }
  return ast_location;
}

// TODO: If all of the columns that need coercing are
// literals then we do not need to add a wrapper scan and the literals should
// be converted in place instead.
absl::Status Resolver::CreateWrapperScanWithCasts(
    const ASTQueryExpression* ast_query,
    const ResolvedColumnList& target_column_list, IdString scan_alias,
    std::unique_ptr<const ResolvedScan>* scan,
    ResolvedColumnList* scan_column_list) {
  ZETASQL_RET_CHECK(scan != nullptr && *scan != nullptr);
  ZETASQL_RET_CHECK_EQ(target_column_list.size(), scan_column_list->size());

  bool needs_casts = false;
  for (int i = 0; i < target_column_list.size(); ++i) {
    if (!target_column_list[i].type()->Equals((*scan_column_list)[i].type())) {
      needs_casts = true;
      break;
    }
  }
  if (needs_casts) {
    ResolvedColumnList casted_column_list;
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> casted_exprs;

    for (int i = 0; i < target_column_list.size(); ++i) {
      const Type* target_type = target_column_list[i].type();
      const ResolvedColumn& scan_column = (*scan_column_list)[i];

      if (target_type->Equals(scan_column.type())) {
        casted_column_list.emplace_back(scan_column);
      } else {
        // Determine the AST expression corresponding to the column. If the
        // query is not a SELECT clause (e.g., a set operation),
        // AddCastOrConvertLiteral below will not convert literals but add
        // casts. Hence, <ast_location> will be used for error reporting only
        // and not for recording parse location of literals.
        const ASTNode* ast_location = GetASTNodeForColumn(
            ast_query, i, static_cast<int>(target_column_list.size()));
        std::unique_ptr<const ResolvedExpr> casted_expr =
            MakeColumnRef(scan_column);
        AnnotatedType annotated_target_type = {target_type,
                                               /*annotation_map=*/nullptr};
        // If the feature is enabled, we keep the original <type_annotation_map>
        // when coercing types of the columns since we may need to process the
        // annotations (e.g. collation) of the original columns at a later
        // stage.
        if (language().LanguageFeatureEnabled(
                FEATURE_V_1_4_PRESERVE_ANNOTATION_IN_IMPLICIT_CAST_IN_SCAN)) {
          annotated_target_type = {target_type,
                                   scan_column.type_annotation_map()};
        }
        Collation collation_for_cast;
        if (annotated_target_type.annotation_map != nullptr) {
          ZETASQL_ASSIGN_OR_RETURN(
              collation_for_cast,
              Collation::MakeCollation(*annotated_target_type.annotation_map));
        }
        ZETASQL_RETURN_IF_ERROR(function_resolver_->AddCastOrConvertLiteral(
            ast_location, annotated_target_type, /*format=*/nullptr,
            /*time_zone=*/nullptr,
            TypeModifiers::MakeTypeModifiers(TypeParameters(),
                                             std::move(collation_for_cast)),
            scan->get(),
            /*set_has_explicit_type=*/false, /*return_null_on_error=*/false,
            &casted_expr));
        const ResolvedColumn casted_column(AllocateColumnId(), scan_alias,
                                           scan_column.name_id(),
                                           annotated_target_type);

        // These casted columns should not get pruned.  We wouldn't create them
        // if they weren't required for the query.
        RecordColumnAccess(casted_column);

        casted_column_list.emplace_back(casted_column);
        casted_exprs.push_back(
            MakeResolvedComputedColumn(casted_column, std::move(casted_expr)));
      }
    }

    *scan = MakeResolvedProjectScan(casted_column_list, std::move(casted_exprs),
                                    std::move(*scan));
    casted_exprs.clear();  // Avoid deletion after ownership transfer.

    ZETASQL_RET_CHECK_EQ(scan_column_list->size(), casted_column_list.size());
    *scan_column_list = casted_column_list;
  }

  return absl::OkStatus();
}

absl::Status Resolver::ResolveFromClauseAndCreateScan(
    const ASTSelect* select, const ASTOrderBy* order_by,
    const NameScope* external_scope,
    std::unique_ptr<const ResolvedScan>* output_scan,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  if (select->from_clause() != nullptr) {
    ZETASQL_RET_CHECK(select->from_clause()->table_expression() != nullptr);
    return ResolveTableExpression(select->from_clause()->table_expression(),
                                  external_scope, external_scope, output_scan,
                                  output_name_list);
  } else {
    // No-from-clause query has special rules about what else can exist.
    if (select->where_clause() != nullptr) {
      return MakeSqlErrorAt(select->where_clause())
             << "Query without FROM clause cannot have a WHERE clause";
    }
    if (select->distinct()) {
      return MakeSqlErrorAt(select)
             << "Query without FROM clause cannot use SELECT DISTINCT";
    }
    if (select->group_by() != nullptr) {
      return MakeSqlErrorAt(select->group_by())
             << "Query without FROM clause cannot have a GROUP BY clause";
    }
    if (select->having() != nullptr) {
      return MakeSqlErrorAt(select->having())
             << "Query without FROM clause cannot have a HAVING clause";
    }
    if (select->window_clause() != nullptr) {
      return MakeSqlErrorAt(select->window_clause())
             << "Query without FROM clause cannot have a WINDOW clause";
    }
    if (order_by != nullptr) {
      return MakeSqlErrorAt(order_by)
             << "Query without FROM clause cannot have an ORDER BY clause";
    }
    if (select->qualify() != nullptr) {
      return MakeSqlErrorAt(select->qualify())
             << "Query without FROM clause cannot have a QUALIFY clause";
    }

    // All children of the select node that are allowed on no-FROM-clause
    // queries should be listed here.  Ones that are not allowed should have
    // errors above.  This checks we didn't miss anything.
    for (int i = 0; i < select->num_children(); ++i) {
      const ASTNode* child = select->child(i);
      if (child != select->select_list() && child != select->select_as() &&
          child != select->hint()) {
        ZETASQL_RET_CHECK_FAIL() << "Select without FROM clause has child of type "
                         << child->GetNodeKindString()
                         << " that wasn't caught with an error";
      }
    }

    // Set up a SingleRowScan for this from clause, which produces one
    // row with zero columns. All output columns will come from
    // expressions in the select-list.
    *output_scan = MakeResolvedSingleRowScan();
    *output_name_list = empty_name_list_;
  }
  return absl::OkStatus();
}

// This is a self-contained table expression.  It can be an UNNEST, but
// only as a leaf - not one that has to wrap another scan and flatten it.
absl::Status Resolver::ResolveTableExpression(
    const ASTTableExpression* table_expr, const NameScope* external_scope,
    const NameScope* local_scope, std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  switch (table_expr->node_kind()) {
    case AST_TABLE_PATH_EXPRESSION:
      return ResolveTablePathExpression(
          table_expr->GetAsOrDie<ASTTablePathExpression>(), external_scope,
          local_scope, output, output_name_list);

    case AST_TABLE_SUBQUERY:
      return ResolveTableSubquery(table_expr->GetAsOrDie<ASTTableSubquery>(),
                                  external_scope, output, output_name_list);

    case AST_JOIN:
      return ResolveJoin(table_expr->GetAsOrDie<ASTJoin>(), external_scope,
                         local_scope, output, output_name_list);

    case AST_PARENTHESIZED_JOIN:
      return ResolveParenthesizedJoin(
          table_expr->GetAsOrDie<ASTParenthesizedJoin>(), external_scope,
          local_scope, output, output_name_list);

    case AST_TVF: {
      std::vector<ResolvedTVFArg> tvf_args;
      return ResolveTVF(table_expr->GetAsOrDie<ASTTVF>(), external_scope,
                        local_scope, &tvf_args, output, output_name_list);
    }

    default:
      return MakeSqlErrorAt(table_expr)
             << "Unhandled node type in from clause: "
             << table_expr->GetNodeKindString();
  }
}

bool Resolver::IsPathExpressionStartingFromScope(const ASTPathExpression& expr,
                                                 const NameScope& scope) {
  return scope.HasName(expr.first_name()->GetAsIdString());
}

bool Resolver::ShouldResolveAsArrayScan(const ASTTablePathExpression* table_ref,
                                        const NameScope* scope) {
  // Return true if it has UNNEST, or it is a path with at least two
  // identifiers where the first comes from <scope>.
  // Single-word identifiers are always resolved as table names.
  return table_ref->unnest_expr() != nullptr ||
         (table_ref->path_expr()->num_names() > 1 &&
          IsPathExpressionStartingFromScope(*(table_ref->path_expr()), *scope));
}

absl::Status Resolver::CheckValidValueTable(const ASTPathExpression& path_expr,
                                            const Table& table) const {
  if (table.NumColumns() == 0 || table.GetColumn(0)->IsPseudoColumn()) {
    return MakeSqlErrorAt(path_expr)
           << "Table " << path_expr.ToIdentifierPathString()
           << " is a value table but does not have a value column";
  }
  for (int i = 1; i < table.NumColumns(); ++i) {
    if (!table.GetColumn(i)->IsPseudoColumn()) {
      return MakeSqlErrorAt(path_expr)
             << "Table " << path_expr.ToIdentifierPathString()
             << " is a value table but has multiple columns";
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::CheckValidValueTableFromTVF(
    const ASTTVF* path_expr, absl::string_view full_tvf_name,
    const TVFRelation& schema) const {
  int64_t num_pseudo_columns = std::count_if(
      schema.columns().begin(), schema.columns().end(),
      [](const TVFSchemaColumn& column) { return column.is_pseudo_column; });
  if (schema.num_columns() - num_pseudo_columns != 1) {
    return MakeSqlErrorAt(path_expr)
           << "Table-valued functions returning value tables should have "
           << "exactly one column, but value table TVF " << full_tvf_name
           << " returned has " << schema.num_columns() - num_pseudo_columns
           << " columns";
  }
  if (schema.column(0).is_pseudo_column) {
    return MakeSqlErrorAt(path_expr)
           << "Table-valued functions returning value tables should have "
           << "a value column at index 0, but value table TVF " << full_tvf_name
           << " returned has a pseudo column at index 0";
  }
  return absl::OkStatus();
}

absl::Status Resolver::AppendPivotColumnNameViaStringCast(
    const Value& pivot_value, std::string* column_name) {
  ZETASQL_ASSIGN_OR_RETURN(
      Value string_value,
      CastValue(pivot_value, analyzer_options_.default_time_zone(),
                analyzer_options_.language(), type_factory_->get_string(),
                /*catalog=*/nullptr, /*canonicalize_zero=*/true));

  const std::string& raw_column_name = string_value.string_value();

  // Cast of any numeric type to string should never produce an empty string.
  ZETASQL_RET_CHECK(!raw_column_name.empty());

  // Transform the result so that it is a valid SQL identifier, which can be
  // accessed without backticks:
  // - If the entire column name (including prefixes from the pivot expression)
  //   would start with a digit, prepend an "_".
  // - For negative values, replace "-" as "minus_" for the minus sign.
  // - For decimal values, replace "." with "_point_" for the decimal point.
  if (column_name->empty() && isdigit(raw_column_name[0])) {
    absl::StrAppend(column_name, "_");
  }

  absl::StrAppend(column_name,
                  absl::StrReplaceAll(raw_column_name,
                                      {{"-", "minus_"}, {".", "_point_"}}));
  return absl::OkStatus();
}

absl::Status Resolver::AppendPivotColumnName(const Value& pivot_value,
                                             const ASTNode* ast_location,
                                             std::string* column_name) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  const Type* type = pivot_value.type();
  if (pivot_value.is_null()) {
    absl::StrAppend(column_name, "NULL");
    return absl::OkStatus();
  }
  if (type->IsInt32() || type->IsInt64() || type->IsUint32() ||
      type->IsUint64() || type->IsNumericType() || type->IsBigNumericType() ||
      type->IsBool()) {
    ZETASQL_RETURN_IF_ERROR(
        AppendPivotColumnNameViaStringCast(pivot_value, column_name));
    return absl::OkStatus();
  }
  if (type->IsString()) {
    if (pivot_value.string_value().empty()) {
      if (!column_name->empty()) {
        absl::StrAppend(column_name, "_");
      }
      absl::StrAppend(column_name, "empty_string_value");
    } else {
      absl::StrAppend(column_name, pivot_value.string_value());
    }
    return absl::OkStatus();
  }
  if (type->IsDate()) {
    std::string formatted_date;
    ZETASQL_RETURN_IF_ERROR(functions::FormatDateToString(
        "%Y_%m_%d", pivot_value.date_value(),
        {.expand_Q = false, .expand_J = false}, &formatted_date));

    // If the column name is empty so far, prefix the date with an underscore
    // so that it's a valid SQL identifier.
    if (column_name->empty()) {
      absl::StrAppend(column_name, "_", formatted_date);
    } else {
      absl::StrAppend(column_name, formatted_date);
    }
    return absl::OkStatus();
  }
  if (type->IsEnum()) {
    const std::string* enum_value_name;
    if (type->AsEnum()->FindName(pivot_value.enum_value(), &enum_value_name)) {
      absl::StrAppend(column_name, *enum_value_name);
    } else {
      // As a fallback, treat the underlying value as an INT32.
      ZETASQL_RETURN_IF_ERROR(AppendPivotColumnNameViaStringCast(
          Value::Int32(pivot_value.enum_value()), column_name));
    }
    return absl::OkStatus();
  }
  if (type->IsStruct()) {
    for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
      if (i != 0) {
        absl::StrAppend(column_name, "_");
      }
      const StructField& field = type->AsStruct()->field(i);
      if (!field.name.empty()) {
        absl::StrAppend(column_name, field.name, "_");
      }
      ZETASQL_RETURN_IF_ERROR(AppendPivotColumnName(pivot_value.field(i), ast_location,
                                            column_name));
    }
    return absl::OkStatus();
  }
  return MakeSqlErrorAt(ast_location)
         << "PIVOT values of type " << type->ShortTypeName(product_mode())
         << " must specify an alias";
}

// Consumes a ResolvedExpr used for an IN-clause value inside of a PIVOT clause.
// Returns the underlying value, if known a resolution time, or absl::nullopt
// otherwise.
//
// Currently, this function supports only literal values and struct constructors
// where all arguments are either literals or other struct constructors.
static std::optional<Value> GetPivotValue(const ResolvedExpr* node);

static std::optional<Value> GetStructPivotValue(
    const ResolvedMakeStruct* node) {
  const StructType* struct_type = node->type()->AsStruct();
  std::vector<Value> fields;
  fields.reserve(struct_type->num_fields());
  for (const auto& field : node->field_list()) {
    std::optional<Value> field_value = GetPivotValue(field.get());
    if (!field_value.has_value()) {
      // The value of one of the struct's fields is unknown, so the value of the
      // struct itself is also unknown.
      return std::nullopt;
    }
    fields.push_back(std::move(field_value.value()));
  }

  return Value::UnsafeStruct(struct_type, std::move(fields));
}

static std::optional<Value> GetPivotValue(const ResolvedExpr* node) {
  switch (node->node_kind()) {
    case RESOLVED_LITERAL: {
      // Mark this literal for preservation in the literal remover. It cannot
      // be replaced by a query parameter without causing the query to fail to
      // resolve (because we do not support implicit pivot-value aliases on
      // query parameters and, even if we ever did, the name wouldn't match the
      // one generated by the actual value).
      const ResolvedLiteral* literal = node->GetAs<ResolvedLiteral>();
      const_cast<ResolvedLiteral*>(literal)->set_preserve_in_literal_remover(
          true);
      return literal->value();
    }
    case RESOLVED_MAKE_STRUCT:
      return GetStructPivotValue(node->GetAs<ResolvedMakeStruct>());
    default:
      return std::nullopt;
  }
}

absl::StatusOr<ResolvedColumn> Resolver::CreatePivotColumn(
    const ASTPivotExpression* ast_pivot_expr,
    const ResolvedExpr* resolved_pivot_expr, bool is_only_pivot_expr,
    const ASTPivotValue* ast_pivot_value,
    const ResolvedExpr* resolved_pivot_value) {
  std::string column_name;

  // Generate the name of the pivot column according to rules described in
  // (broken link).
  if (ast_pivot_expr->alias() != nullptr) {
    absl::StrAppend(&column_name, ast_pivot_expr->alias()->GetAsString(), "_");
  } else if (!is_only_pivot_expr) {
    return MakeSqlErrorAt(ast_pivot_expr)
           << "PIVOT expression must specify an alias unless it is the only "
              "pivot expression in the PIVOT clause";
  }

  if (ast_pivot_value->alias() != nullptr) {
    absl::StrAppend(&column_name, ast_pivot_value->alias()->GetAsString());
  } else {
    std::optional<Value> pivot_value = GetPivotValue(resolved_pivot_value);
    if (!pivot_value.has_value()) {
      return MakeSqlErrorAt(ast_pivot_value)
             << "Generating an implicit alias for this PIVOT value is not "
                "supported; please provide an explicit alias";
    }
    ZETASQL_RETURN_IF_ERROR(AppendPivotColumnName(pivot_value.value(), ast_pivot_value,
                                          &column_name));
  }

  return ResolvedColumn(AllocateColumnId(), kPivotId,
                        analyzer_options_.id_string_pool()->Make(column_name),
                        resolved_pivot_expr->annotated_type());
}

absl::Status Resolver::ResolvePivotExpressions(
    const ASTPivotExpressionList* ast_pivot_expr_list, const NameScope* scope,
    std::vector<std::unique_ptr<const ResolvedExpr>>* pivot_expr_columns,
    QueryResolutionInfo& query_resolution_info) {
  ExprResolutionInfo info(scope, scope, scope,
                          /*allows_aggregation_in=*/true,
                          /*allows_analytic_in=*/false,
                          /*use_post_grouping_columns_in=*/false, "PIVOT",
                          &query_resolution_info);
  for (const ASTPivotExpression* pivot_expr :
       ast_pivot_expr_list->expressions()) {
    std::unique_ptr<const ResolvedExpr> resolved_pivot_expr;
    ZETASQL_RETURN_IF_ERROR(
        ResolveExpr(pivot_expr->expression(), &info, &resolved_pivot_expr));

    // Unlike scalar functions, aggregate function calls don't appear directly
    // in the ResolvedExpr. Instead, <resolved_pivot_expr> contains a
    // post-aggregation expression, which refers to the aggregation result by
    // accessing a special column, whose definition is stored off to the side
    // in the QueryResolutionInfo.
    //
    // Verify that
    //   1) We indeed have an aggregate function call saved in the
    //        QueryResolutionInfo --and--
    //   2) The ResolvedExpr is just a ColumnRef referring to the aggregate
    //        column from 1 (since post-aggregation logic in pivot expressions
    //        is not supported).
    std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>
        pivot_expr_column_vector =
            query_resolution_info.release_aggregate_columns_to_compute();
    if (pivot_expr_column_vector.size() == 1) {
      auto& pivot_expr_column = pivot_expr_column_vector[0];
      ZETASQL_RET_CHECK(pivot_expr_column->Is<ResolvedComputedColumn>());
      if (resolved_pivot_expr->node_kind() == RESOLVED_COLUMN_REF &&
          (resolved_pivot_expr->GetAs<ResolvedColumnRef>()
               ->column()
               .column_id() == pivot_expr_column->column().column_id())) {
        // The rewriter requires the root nodes of PIVOT expressions to have a
        // parse location, which gets used in error messages if the function
        // call is not supported. So, always include the parse location for this
        // node, whether explicitly asked for or not.
        auto* mutable_pivot_expr_column = const_cast<ResolvedComputedColumn*>(
            pivot_expr_column->GetAs<ResolvedComputedColumn>());
        std::unique_ptr<const ResolvedExpr> standalone_pivot_expr =
            mutable_pivot_expr_column->release_expr();
        const_cast<ResolvedExpr*>(standalone_pivot_expr.get())
            ->SetParseLocationRange(pivot_expr->GetParseLocationRange());

        pivot_expr_columns->push_back(std::move(standalone_pivot_expr));
        continue;
      }
    }
    return MakeSqlErrorAt(pivot_expr->expression())
           << "PIVOT expression must be an aggregate function call";
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveForExprInPivotClause(
    const ASTExpression* for_expr, const NameScope* scope,
    std::unique_ptr<const ResolvedExpr>* resolved_for_expr) {
  QueryResolutionInfo query_resolution_info(this);
  ExprResolutionInfo info(scope, scope, scope,
                          /*allows_aggregation_in=*/false,
                          /*allows_analytic_in=*/false,
                          /*use_post_grouping_columns_in=*/false, "PIVOT",
                          &query_resolution_info);
  ZETASQL_RETURN_IF_ERROR(ResolveExpr(for_expr, &info, resolved_for_expr));
  // TODO: figure out a way to remove the const_cast here.
  const_cast<ResolvedExpr*>(resolved_for_expr->get())
      ->SetParseLocationRange(for_expr->GetParseLocationRange());

  std::string no_grouping_type;
  if (!resolved_for_expr->get()->type()->SupportsGrouping(language(),
                                                          &no_grouping_type)) {
    return MakeSqlErrorAt(for_expr)
           << "Type " << no_grouping_type
           << " cannot be used as a FOR expression because it is not groupable";
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveInClauseInPivotClause(
    const ASTPivotValueList* pivot_values, const NameScope* scope,
    const Type* for_expr_type,
    std::vector<std::unique_ptr<const ResolvedExpr>>* resolved_in_exprs) {
  for (const ASTPivotValue* ast_pivot_value : pivot_values->values()) {
    // Even though pivot values must be constant, we still resolve them in a
    // scope which includes columns from the input table; this way, when such
    // columns are referenced, you get an error message about the pivot value
    // not being constant vs. the column not existing.
    QueryResolutionInfo query_resolution_info(this);
    ExprResolutionInfo info(scope, scope, scope,
                            /*allows_aggregation_in=*/false,
                            /*allows_analytic_in=*/false,
                            /*use_post_grouping_columns_in=*/false, "IN clause",
                            &query_resolution_info);

    std::unique_ptr<const ResolvedExpr> resolved_in_expr;
    ZETASQL_RETURN_IF_ERROR(
        ResolveExpr(ast_pivot_value->value(), &info, &resolved_in_expr));
    ZETASQL_ASSIGN_OR_RETURN(bool resolved_in_expr_is_constant_expr,
                     IsConstantExpression(resolved_in_expr.get()));
    if (!resolved_in_expr_is_constant_expr) {
      return MakeSqlErrorAt(ast_pivot_value->value())
             << "IN expression in PIVOT clause must be constant";
    }

    ZETASQL_RETURN_IF_ERROR(CoerceExprToType(ast_pivot_value->value(), for_expr_type,
                                     kImplicitCoercion,
                                     "PIVOT IN list item must be type $0 to "
                                     "match the PIVOT FOR expression; found $1",
                                     &resolved_in_expr));

    resolved_in_exprs->push_back(std::move(resolved_in_expr));
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolvePivotClause(
    std::unique_ptr<const ResolvedScan> input_scan,
    std::shared_ptr<const NameList> input_name_list,
    const NameScope* previous_scope, bool input_is_subquery,
    const ASTPivotClause* ast_pivot_clause,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  if (!language().LanguageFeatureEnabled(FEATURE_V_1_3_PIVOT)) {
    return MakeSqlErrorAt(ast_pivot_clause) << "PIVOT is not supported";
  }

  if (in_strict_mode() && !input_is_subquery) {
    // PIVOT treats all unreferenced columns in the input table as GROUP BY
    // columns, allowing it to effectively function like "SELECT *". So, we
    // allow PIVOT in strict mode only when the input is a subquery. Since
    // strict mode require subqueries to explicitly state their columns in the
    // SELECT list, all of the grouping columns are explicitly stated in the
    // query and resiliant to new columns being added to underlying tables.
    return MakeSqlErrorAt(ast_pivot_clause)
           << "Input to PIVOT must be a subquery in strict name resolution "
              "mode";
  }

  if (input_name_list->HasValueTableColumns()) {
    return MakeSqlErrorAt(ast_pivot_clause)
           << "PIVOT is not allowed on value tables";
  }

  // Resolve pivot expressions
  QueryResolutionInfo query_resolution_info(this);
  auto pivot_scope =
      std::make_unique<NameScope>(previous_scope, input_name_list);
  std::vector<std::unique_ptr<const ResolvedExpr>> pivot_expr_columns;
  ZETASQL_RETURN_IF_ERROR(ResolvePivotExpressions(
      ast_pivot_clause->pivot_expressions(), pivot_scope.get(),
      &pivot_expr_columns, query_resolution_info));

  // If one or more of the pivot expressions contains an ORDER BY clause,
  // wrap the input scan with a ProjectScan that computes the order-by columns.
  ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
      query_resolution_info
          .release_select_list_columns_to_compute_before_aggregation(),
      &input_scan));

  // Resolve FOR expression
  std::unique_ptr<const ResolvedExpr> resolved_for_expr;
  ZETASQL_RETURN_IF_ERROR(
      ResolveForExprInPivotClause(ast_pivot_clause->for_expression(),
                                  pivot_scope.get(), &resolved_for_expr));

  // Resolve IN expressions.
  std::vector<std::unique_ptr<const ResolvedExpr>> resolved_in_exprs;
  ZETASQL_RETURN_IF_ERROR(ResolveInClauseInPivotClause(
      ast_pivot_clause->pivot_values(), pivot_scope.get(),
      resolved_for_expr->type(), &resolved_in_exprs));

  // Determine final column list, starting with the grouping columns.
  auto final_name_list = std::make_shared<NameList>();
  absl::flat_hash_set<ResolvedColumn> referenced_columns;
  FindReferencedColumnsVisitor visitor(&referenced_columns);
  for (const auto& pivot_expr : pivot_expr_columns) {
    ZETASQL_RETURN_IF_ERROR(pivot_expr->Accept(&visitor));
  }
  ZETASQL_RETURN_IF_ERROR(resolved_for_expr->Accept(&visitor));

  std::vector<std::unique_ptr<const ResolvedComputedColumn>> group_by_list;
  std::vector<ResolvedColumn> output_column_list;
  std::vector<std::unique_ptr<const ResolvedPivotColumn>>
      output_column_detail_list;
  output_column_list.reserve(input_scan->column_list().size());
  for (int i = 0; i < input_name_list->num_columns(); ++i) {
    const NamedColumn& named_col = input_name_list->column(i);
    const ResolvedColumn& col = named_col.column();
    if (!referenced_columns.contains(col)) {
      std::string no_grouping_type;
      if (!col.type()->SupportsGrouping(language(), &no_grouping_type)) {
        return MakeSqlErrorAt(ast_pivot_clause)
               << "Column " << ColumnAliasOrPosition(named_col.name(), i)
               << " of type " << no_grouping_type
               << " cannot be used as an implicit grouping column of a PIVOT "
                  "clause";
      }

      // Columns from the input table not referenced in either a pivot
      // expression or FOR expression are considered implicit group-by columns.
      ResolvedColumn output_col(AllocateColumnId(), kGroupById,
                                named_col.name(), col.annotated_type());
      output_column_list.push_back(output_col);

      group_by_list.push_back(
          MakeResolvedComputedColumn(output_col, MakeColumnRef(col)));

      ZETASQL_RETURN_IF_ERROR(final_name_list->AddColumn(named_col.name(), output_col,
                                                 /*is_explicit=*/true));
    }
  }

  // Add a pivot column for each pivot-expr/pivot-value combination.
  output_column_list.reserve(resolved_in_exprs.size() *
                             pivot_expr_columns.size());
  for (int i = 0; i < resolved_in_exprs.size(); ++i) {
    for (int j = 0; j < pivot_expr_columns.size(); ++j) {
      ZETASQL_ASSIGN_OR_RETURN(
          ResolvedColumn pivot_column,
          CreatePivotColumn(
              ast_pivot_clause->pivot_expressions()->expressions()[j],
              pivot_expr_columns[j].get(),
              /*is_only_pivot_expr=*/pivot_expr_columns.size() == 1,
              ast_pivot_clause->pivot_values()->values()[i],
              resolved_in_exprs[i].get()));
      output_column_detail_list.push_back(
          MakeResolvedPivotColumn(pivot_column,
                                  /*pivot_expr_index=*/j,
                                  /*pivot_value_index=*/i));
      output_column_list.push_back(pivot_column);
      ZETASQL_RETURN_IF_ERROR(final_name_list->AddColumn(
          pivot_column.name_id(), pivot_column, /*is_explicit=*/true));
    }
  }

  *output_name_list = final_name_list;
  if (ast_pivot_clause->output_alias() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(*output_name_list,
                     NameList::AddRangeVariableInWrappingNameList(
                         ast_pivot_clause->output_alias()->GetAsIdString(),
                         ast_pivot_clause->output_alias(), *output_name_list));
  }
  for (const auto& pivot_expr : pivot_expr_columns) {
    ZETASQL_RET_CHECK(pivot_expr->Is<ResolvedAggregateFunctionCall>());
    const auto& aggregate_func_call =
        pivot_expr->GetAs<ResolvedAggregateFunctionCall>();
    if (aggregate_func_call->function()->Is<SQLFunctionInterface>() ||
        aggregate_func_call->function()->Is<TemplatedSQLFunction>()) {
      analyzer_output_properties_.MarkRelevant(REWRITE_INLINE_SQL_UDAS);
    }
  }
  *output = MakeResolvedPivotScan(
      output_column_list, std::move(input_scan), std::move(group_by_list),
      std::move(pivot_expr_columns), std::move(resolved_for_expr),
      std::move(resolved_in_exprs), std::move(output_column_detail_list));
  analyzer_output_properties_.MarkRelevant(REWRITE_PIVOT);

  return absl::OkStatus();
}

absl::Status Resolver::ResolveUnpivotOutputValueColumns(
    const ASTPathExpressionList* ast_unpivot_expr_list,
    std::vector<ResolvedColumn>* unpivot_value_columns,
    const std::vector<const Type*>& value_column_types,
    const NameScope* scope) {
  QueryResolutionInfo query_resolution_info(this);
  ExprResolutionInfo info(scope, scope, scope,
                          /*allows_aggregation_in=*/false,
                          /*allows_analytic_in=*/false,
                          /*use_post_grouping_columns_in=*/false,
                          "UNPIVOT clause", &query_resolution_info);

  int column_index = 0;
  if (value_column_types.size() !=
      ast_unpivot_expr_list->path_expression_list().size()) {
    return MakeSqlErrorAt(ast_unpivot_expr_list)
           << "The number of new columns introduced as value columns must be "
              "the same as the number of columns in the column groups of "
              "UNPIVOT IN clause";
  }
  for (const ASTPathExpression* unpivot_column :
       ast_unpivot_expr_list->path_expression_list()) {
    if (unpivot_column->num_names() > 1) {
      return MakeSqlErrorAt(unpivot_column)
             << "Only names of the new columns are accepted as value columns "
                "in UNPIVOT. Qualified names are not allowed";
    }
    IdString column_name = unpivot_column->first_name()->GetAsIdString();
    ResolvedColumn unpivot_resolved_column(
        AllocateColumnId(), /*table_name=*/kUnpivotColumnId, column_name,
        AnnotatedType(value_column_types.at(column_index),
                      /*annotation_map=*/nullptr));
    column_index++;
    unpivot_value_columns->push_back(unpivot_resolved_column);
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveUnpivotInClause(
    const ASTUnpivotInItemList* ast_unpivot_expr_list,
    std::vector<std::unique_ptr<const ResolvedUnpivotArg>>* resolved_in_items,
    const std::vector<ResolvedColumn>* input_scan_columns,
    absl::flat_hash_set<ResolvedColumn>* in_clause_input_columns,
    std::vector<const Type*>* value_column_types, const Type** label_type,
    std::vector<std::unique_ptr<const ResolvedLiteral>>* resolved_label_list,
    const ASTUnpivotClause* ast_unpivot_clause, const NameScope* scope) {
  QueryResolutionInfo query_resolution_info(this);
  ExprResolutionInfo info(scope, scope, scope,
                          /*allows_aggregation_in=*/false,
                          /*allows_analytic_in=*/false,
                          /*use_post_grouping_columns_in=*/false, "IN clause",
                          &query_resolution_info);

  absl::flat_hash_set<ResolvedColumn> input_columns;
  input_columns.insert(input_scan_columns->begin(), input_scan_columns->end());
  absl::flat_hash_set<ResolvedColumn> input_columns_seen;
  TypeKind label_type_kind = TYPE_UNKNOWN;
  // The first column group is used to determine the size and order of datatypes
  // of the column groups in IN clause.
  bool is_first_column_group = true;
  for (const ASTUnpivotInItem* in_column_list :
       ast_unpivot_expr_list->in_items()) {
    // IN clause contains nested columns, i.e. groups of columns inside a list,
    // Example .. FOR .. IN( (a , b) , ( c , d ) ).
    // in_column_group will contain the columns lists resolved from inner groups
    // and the resulting list will be pushed onto the resolved_in_items vector,
    // in the same order as they appear in the IN clause.
    std::vector<std::unique_ptr<const ResolvedColumnRef>> in_column_group;
    in_column_group.reserve(
        in_column_list->unpivot_columns()->path_expression_list().size());

    if (!is_first_column_group &&
        value_column_types->size() !=
            in_column_list->unpivot_columns()->path_expression_list().size()) {
      return MakeSqlErrorAt(in_column_list->unpivot_columns())
             << "All column groups in UNPIVOT IN clause must have the same "
                "number of columns";
    }
    int column_index = 0;
    for (const ASTPathExpression* in_column :
         in_column_list->unpivot_columns()->path_expression_list()) {
      // Get resolved column for column in the IN clause.
      std::unique_ptr<const ResolvedExpr> resolved_expr_out;
      const ASTExpression* in_column_expr = in_column->GetAs<ASTExpression>();
      ZETASQL_RETURN_IF_ERROR(ResolveExpr(in_column_expr, &info, &resolved_expr_out));
      std::unique_ptr<const ResolvedColumnRef> in_column_ref;
      if (resolved_expr_out->node_kind() != RESOLVED_COLUMN_REF) {
        return MakeSqlErrorAt(in_column)
               << "UNPIVOT IN clause cannot have expressions, only column "
                  "names are allowed";
      }
      in_column_ref.reset(
          resolved_expr_out.release()->GetAs<ResolvedColumnRef>());
      const Type* datatype = in_column_ref->type();
      // The first column group is used to determine the datatype order and
      // number of columns for all other column groups.
      if (is_first_column_group) {
        value_column_types->push_back(std::move(datatype));
      } else if (!datatype->Equals(value_column_types->at(column_index))) {
        ZETASQL_RET_CHECK_LT(column_index, value_column_types->size());
        return MakeSqlErrorAt(in_column)
               << "The datatype of column does not match with other datatypes "
                  "in the IN clause. Expected "
               << Type::TypeKindToString(
                      value_column_types->at(column_index)->kind(),
                      product_mode())
               << ", Found "
               << Type::TypeKindToString(datatype->kind(), product_mode());
      }
      column_index++;
      if (!input_columns.contains(in_column_ref->column())) {
        return MakeSqlErrorAt(in_column)
               << "Correlated column references in UNPIVOT IN clause is not "
                  "allowed";
      }
      if (input_columns_seen.contains(in_column_ref->column())) {
        // TODO: For queries where same input table column is
        // mapped to different columns in the unpivot input subquery, we might
        // want to allow those columns in the UNPIVOT IN clause.
        // Example, SELECT * FROM (SELECT x as a, x as b
        //            FROM t) UNPIVOT (y for z IN ( a , b ));
        return MakeSqlErrorAt(in_column)
               << "Column names in UNPIVOT IN clause cannot be repeated";
      }
      input_columns_seen.insert(in_column_ref->column());
      in_clause_input_columns->insert(in_column_ref->column());
      in_column_group.push_back(std::move(in_column_ref));
    }
    std::unique_ptr<const ResolvedUnpivotArg> col_set =
        MakeResolvedUnpivotArg(std::move(in_column_group));
    resolved_in_items->push_back(std::move(col_set));

    // Each column group can have an explicitly provided alias (after AS).
    // If the alias is not provided, a string label is auto-generated by
    // concatenating column names from the column group. The datatypes of all
    // labels must be the same and hence they must all be provided in the case
    // of non-string(integer) labels.
    ZETASQL_ASSIGN_OR_RETURN(Value label,
                     GetLabelForUnpivotInColumnList(in_column_list));
    if (label_type_kind == TYPE_UNKNOWN) {
      label_type_kind = label.type_kind();
    } else if (label_type_kind != label.type_kind()) {
      return MakeSqlErrorAt(in_column_list)
             << "All UNPIVOT labels must be the same type; when non-string "
                "labels are present then all labels must be explicitly "
                "provided";
    }
    resolved_label_list->push_back(MakeResolvedLiteralWithoutLocation(label));
    is_first_column_group = false;
  }
  *label_type = types::TypeFromSimpleTypeKind(label_type_kind);
  return absl::OkStatus();
}

absl::StatusOr<Value> Resolver::GetLabelForUnpivotInColumnList(
    const ASTUnpivotInItem* in_column_list) {
  if (in_column_list->alias() == nullptr ||
      in_column_list->alias()->label() == nullptr) {
    std::string column_group_label = "";
    for (const ASTPathExpression* in_col :
         in_column_list->unpivot_columns()->path_expression_list()) {
      if (column_group_label.empty()) {
        column_group_label = in_col->last_name()->GetAsString();
      } else {
        absl::StrAppend(&column_group_label, "_",
                        in_col->last_name()->GetAsString());
      }
    }
    return Value::String(column_group_label);
  }

  if (in_column_list->alias()->label()->node_kind() == AST_STRING_LITERAL) {
    return Value::String(in_column_list->alias()
                             ->label()
                             ->GetAsOrNull<ASTStringLiteral>()
                             ->string_value());
  } else if (in_column_list->alias()->label()->node_kind() == AST_INT_LITERAL) {
    Value label;
    if (!Value::ParseInteger(in_column_list->alias()
                                 ->label()
                                 ->GetAsOrNull<ASTIntLiteral>()
                                 ->image(),
                             &label)) {
      return MakeSqlErrorAt(in_column_list->alias()->label())
             << "Invalid integer label for UNPIVOT clause";
    }
    return label;
  }
  // The parser only accepts integer and string literal so we would
  // never get here, but added for sanity check.
  return MakeSqlErrorAt(in_column_list)
         << "UNPIVOT column labels must be string or integer";
}

absl::Status Resolver::ResolveUnpivotClause(
    std::unique_ptr<const ResolvedScan> input_scan,
    std::shared_ptr<const NameList> input_name_list,
    const NameScope* previous_scope, const ASTUnpivotClause* ast_unpivot_clause,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  if (!language().LanguageFeatureEnabled(FEATURE_V_1_3_UNPIVOT)) {
    return MakeSqlErrorAt(ast_unpivot_clause) << "UNPIVOT is not supported";
  }

  if (input_name_list->HasValueTableColumns()) {
    return MakeSqlErrorAt(ast_unpivot_clause)
           << "UNPIVOT is not allowed on value tables";
  }

  auto unpivot_scope =
      std::make_unique<NameScope>(previous_scope, input_name_list);

  // Resolve IN columns.
  std::vector<std::unique_ptr<const ResolvedUnpivotArg>> resolved_in_items;
  absl::flat_hash_set<ResolvedColumn> in_clause_input_columns;
  std::vector<const Type*> value_column_types;
  std::vector<std::unique_ptr<const ResolvedLiteral>> resolved_label_list;
  const Type* label_type = nullptr;
  ZETASQL_RETURN_IF_ERROR(ResolveUnpivotInClause(
      ast_unpivot_clause->unpivot_in_items(), &resolved_in_items,
      &input_scan->column_list(), &in_clause_input_columns, &value_column_types,
      &label_type, &resolved_label_list, ast_unpivot_clause,
      unpivot_scope.get()));

  // Resolve unpivot output value columns.
  std::vector<ResolvedColumn> unpivot_value_columns;
  ZETASQL_RETURN_IF_ERROR(ResolveUnpivotOutputValueColumns(
      ast_unpivot_clause->unpivot_output_value_columns(),
      &unpivot_value_columns, value_column_types, unpivot_scope.get()));

  // Resolve unpivot output name column.
  if (ast_unpivot_clause->unpivot_output_name_column()->num_names() > 1) {
    return MakeSqlErrorAt(ast_unpivot_clause->unpivot_output_name_column())
           << "Only name of the new column is accepted as label column in "
              "UNPIVOT. Qualified names are not allowed";
  }
  IdString column_name = ast_unpivot_clause->unpivot_output_name_column()
                             ->first_name()
                             ->GetAsIdString();
  ResolvedColumn unpivot_label_column(
      AllocateColumnId(), /*table_name=*/kUnpivotColumnId, column_name,
      AnnotatedType(label_type, /*annotation_map=*/nullptr));

  // Create a list of output columns. It would contain:
  // Input columns that are not present in the IN clause, in the order as they
  // appear in the <input_scan> + unpivot_value_columns + unpivot_label_column
  std::vector<ResolvedColumn> output_column_list;
  auto final_name_list = std::make_shared<NameList>();

  // Columns from the input table that are not unpivoted in IN clause are
  // projected in the output as a new computed column.
  std::vector<std::unique_ptr<const ResolvedComputedColumn>>
      projected_input_column_list;
  for (int i = 0; i < input_name_list->num_columns(); ++i) {
    const NamedColumn& named_col = input_name_list->column(i);
    const ResolvedColumn& col = named_col.column();
    if (!in_clause_input_columns.contains(col)) {
      ResolvedColumn output_col(AllocateColumnId(),
                                /*table_name=*/kUnpivotColumnId,
                                named_col.name(), col.type());
      output_column_list.push_back(output_col);
      projected_input_column_list.push_back(
          MakeResolvedComputedColumn(output_col, MakeColumnRef(col)));
      ZETASQL_RETURN_IF_ERROR(final_name_list->AddColumn(named_col.name(), output_col,
                                                 /*is_explicit=*/true));
    }
  }
  for (int i = 0; i < unpivot_value_columns.size(); i++) {
    output_column_list.push_back(unpivot_value_columns[i]);
    ZETASQL_RETURN_IF_ERROR(final_name_list->AddColumn(
        unpivot_value_columns[i].name_id(), unpivot_value_columns[i], true));
  }
  output_column_list.push_back(unpivot_label_column);
  ZETASQL_RETURN_IF_ERROR(final_name_list->AddColumn(unpivot_label_column.name_id(),
                                             unpivot_label_column, true));

  *output_name_list = final_name_list;

  if (ast_unpivot_clause->output_alias() != nullptr) {
    ZETASQL_ASSIGN_OR_RETURN(
        *output_name_list,
        NameList::AddRangeVariableInWrappingNameList(
            ast_unpivot_clause->output_alias()->GetAsIdString(),
            ast_unpivot_clause->output_alias(), *output_name_list));
  }

  bool include_nulls = false;
  if (ast_unpivot_clause->null_filter() == ASTUnpivotClause::kInclude) {
    include_nulls = true;
  }

  *output = MakeResolvedUnpivotScan(
      output_column_list, std::move(input_scan),
      std::move(unpivot_value_columns), std::move(unpivot_label_column),
      std::move(resolved_label_list), std::move(resolved_in_items),
      std::move(projected_input_column_list), include_nulls);
  analyzer_output_properties_.MarkRelevant(REWRITE_UNPIVOT);
  return absl::OkStatus();
}

static bool ShouldResolveSuffixAsArrayScan(
    const LanguageOptions& options, std::unique_ptr<PathExpressionSpan>& suffix,
    const ASTPathExpression* path_expr) {
  return options.LanguageFeatureEnabled(
             FEATURE_V_1_4_SINGLE_TABLE_NAME_ARRAY_PATH) &&
         suffix != nullptr && suffix->num_names() > 1 &&
         suffix->num_names() <= path_expr->num_names();
}

// This is a self-contained table expression.  It can be an UNNEST, but
// only as a leaf - not one that has to wrap another scan and flatten it.
absl::Status Resolver::ResolveTablePathExpression(
    const ASTTablePathExpression* table_ref, const NameScope* external_scope,
    const NameScope* local_scope, std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  const ASTForSystemTime* for_system_time = table_ref->for_system_time();
  if (for_system_time != nullptr &&
      !language().LanguageFeatureEnabled(FEATURE_V_1_1_FOR_SYSTEM_TIME_AS_OF)) {
    return MakeSqlErrorAt(for_system_time)
           << "FOR SYSTEM_TIME AS OF is not supported";
  }

  if (ShouldResolveAsArrayScan(table_ref, local_scope)) {
    if (for_system_time != nullptr) {
      return MakeSqlErrorAt(for_system_time)
             << "FOR SYSTEM_TIME AS OF is not allowed with array scans";
    }
    if (table_ref->pivot_clause() != nullptr) {
      return MakeSqlErrorAt(table_ref->pivot_clause())
             << "PIVOT is not allowed with array scans";
    }

    if (table_ref->unpivot_clause() != nullptr) {
      return MakeSqlErrorAt(table_ref->unpivot_clause())
             << "UNPIVOT is not allowed with array scans";
    }

    // When <table_ref> contains explicit UNNEST, do not supply <path_expr>.
    // Otherwise pass in the full path expression.
    std::unique_ptr<const ResolvedScan> no_lhs_scan;
    std::optional<PathExpressionSpan> path_expr =
        table_ref->path_expr() != nullptr
            ? std::make_optional(PathExpressionSpan(*table_ref->path_expr()))
            : std::nullopt;
    return ResolveArrayScan(table_ref, path_expr,
                            /*on_clause=*/nullptr, /*using_clause=*/nullptr,
                            /*ast_join=*/nullptr, /*is_outer_scan=*/false,
                            /*include_lhs_name_list=*/false,
                            /*is_single_table_array_path=*/false,
                            /*resolved_input_scan=*/&no_lhs_scan,
                            /*name_list_input=*/nullptr, local_scope, output,
                            output_name_list);
  }

  if (table_ref->with_offset() != nullptr) {
    return MakeSqlErrorAt(table_ref)
           << "WITH OFFSET can only be used with array scans";
  }

  const ASTPathExpression* path_expr = table_ref->path_expr();
  ZETASQL_RET_CHECK(path_expr != nullptr);

  IdString alias;
  bool has_explicit_alias;
  const ASTNode* alias_location;
  if (table_ref->alias() != nullptr) {
    alias = table_ref->alias()->GetAsIdString();
    alias_location = table_ref->alias();
    has_explicit_alias = true;
  } else {
    alias = GetAliasForExpression(path_expr);
    alias_location = table_ref;
    has_explicit_alias = false;
  }
  ZETASQL_RET_CHECK(!alias.empty());

  std::shared_ptr<const NameList> name_list;
  std::unique_ptr<const ResolvedScan> this_scan;
  if (named_subquery_map_.contains(path_expr->ToIdStringVector())) {
    if (for_system_time != nullptr) {
      return MakeSqlErrorAt(for_system_time) << "FOR SYSTEM_TIME AS OF cannot "
                                                "be used with tables defined "
                                                "in WITH clause";
    }
    ZETASQL_RETURN_IF_ERROR(ResolveNamedSubqueryRef(path_expr, table_ref->hint(),
                                            &this_scan, &name_list));

    if (name_list->is_value_table()) {
      auto new_name_list = std::make_shared<NameList>();
      ZETASQL_RETURN_IF_ERROR(ConvertValueTableNameListToNameListWithValueTable(
          table_ref, alias, name_list, &new_name_list));
      name_list = new_name_list;
    } else {
      // Add a range variable for the with_ref scan.
      ZETASQL_ASSIGN_OR_RETURN(name_list, NameList::AddRangeVariableInWrappingNameList(
                                      alias, alias_location, name_list));
    }
  } else if (path_expr->num_names() == 1 &&
             function_argument_info_ != nullptr &&
             function_argument_info_->FindTableArg(
                 path_expr->first_name()->GetAsIdString()) != nullptr) {
    if (for_system_time != nullptr) {
      return MakeSqlErrorAt(for_system_time)
             << "FOR SYSTEM_TIME AS OF cannot be used with TABLE parameter "
             << path_expr->first_name()->GetAsIdString() << " to FUNCTION";
    }
    ZETASQL_RETURN_IF_ERROR(ResolvePathExpressionAsFunctionTableArgument(
        path_expr, table_ref->hint(), alias, alias_location, &this_scan,
        &name_list));
  } else {
    // The (possibly multi-part) table name did not match the WITH clause or a
    // table-valued argument (which only support single-part names), so try to
    // resolve this name as a Table from the Catalog.
    // If the prefix of the <path_expr> is resolved as the table name,
    // <suffix> is the suffix part of the names that start with the
    // last identifier in the table name.
    // For example, if we found table name "catalog.table" in <path_expr>
    // "catalog.table.array", then <suffix> will be set to "table.array".
    std::unique_ptr<const ResolvedTableScan> table_scan;
    std::unique_ptr<PathExpressionSpan> suffix;
    std::unique_ptr<PathExpressionSpan>* remaining_names =
        language().LanguageFeatureEnabled(
            FEATURE_V_1_4_SINGLE_TABLE_NAME_ARRAY_PATH)
            ? &suffix
            : nullptr;

    ZETASQL_RETURN_IF_ERROR(ResolvePathExpressionAsTableScan(
        path_expr, alias, has_explicit_alias, alias_location, table_ref->hint(),
        for_system_time, local_scope, remaining_names, &table_scan, &name_list,
        resolved_columns_from_table_scans_));
    this_scan = std::move(table_scan);

    if (ShouldResolveSuffixAsArrayScan(language(), suffix, path_expr)) {
      std::unique_ptr<const NameScope> rhs_scope =
          std::make_unique<NameScope>(*name_list);
      const NameScope* rhs_array_scan_scope = rhs_scope.get();
      auto empty_name_list = std::make_shared<NameList>();
      return ResolveArrayScan(table_ref, std::make_optional(*suffix),
                              /*on_clause=*/nullptr, /*using_clause=*/nullptr,
                              /*ast_join=*/nullptr, /*is_outer_scan=*/false,
                              /*include_lhs_name_list=*/false,
                              /*is_single_table_array_path=*/true, &this_scan,
                              empty_name_list, rhs_array_scan_scope, output,
                              output_name_list);
    }
  }
  ZETASQL_RET_CHECK(this_scan != nullptr);
  ZETASQL_RET_CHECK(name_list != nullptr);

  if (table_ref->pivot_clause() != nullptr) {
    ZETASQL_RET_CHECK_EQ(for_system_time, nullptr)
        << "Parser should not allow PIVOT and FOR SYSTEM TIME AS OF to coexist";
    ZETASQL_RETURN_IF_ERROR(
        ResolvePivotClause(std::move(this_scan), name_list, external_scope,
                           /*input_is_subquery=*/false,
                           table_ref->pivot_clause(), &this_scan, &name_list));
  }
  if (table_ref->unpivot_clause() != nullptr) {
    ZETASQL_RET_CHECK_EQ(for_system_time, nullptr)
        << "Parser should not allow UNPIVOT and FOR SYSTEM TIME AS OF to "
           "coexist";
    ZETASQL_RETURN_IF_ERROR(ResolveUnpivotClause(
        std::move(this_scan), name_list, external_scope,
        table_ref->unpivot_clause(), &this_scan, &name_list));
  }
  if (table_ref->sample_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveTablesampleClause(table_ref->sample_clause(),
                                             &name_list, &this_scan));
  }

  *output_name_list = name_list;
  *output = std::move(this_scan);
  return absl::OkStatus();
}

absl::Status Resolver::ResolvePathExpressionAsFunctionTableArgument(
    const ASTPathExpression* path_expr, const ASTHint* hint, IdString alias,
    const ASTNode* ast_location, std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  std::shared_ptr<const NameList> name_list;
  std::unique_ptr<const ResolvedScan> this_scan;
  ZETASQL_RET_CHECK_NE(function_argument_info_, nullptr);
  // The path refers to a relation argument in a CREATE TABLE FUNCTION
  // statement. Create and return a resolved relation argument reference.
  const FunctionArgumentInfo::ArgumentDetails* details =
      function_argument_info_->FindTableArg(
          path_expr->first_name()->GetAsIdString());
  ZETASQL_RET_CHECK_NE(details, nullptr);
  // We do not expect to analyze a template function body until the function is
  // invoked and we have concrete argument types. If a template type is found
  // that could mean engine code is using an API incorrectly.
  ZETASQL_RET_CHECK(!details->arg_type.IsTemplated())
      << "Function bodies cannot be resolved with templated argument types";
  const TVFRelation& tvf_relation =
      details->arg_type.options().relation_input_schema();
  std::unique_ptr<NameList> new_name_list(new NameList);
  std::vector<ResolvedColumn> resolved_columns;
  if (tvf_relation.is_value_table()) {
    ZETASQL_RET_CHECK_EQ(1, tvf_relation.num_columns());
    resolved_columns.push_back(ResolvedColumn(
        AllocateColumnId(), path_expr->first_name()->GetAsIdString(),
        kValueColumnId, tvf_relation.column(0).type));
    ZETASQL_RETURN_IF_ERROR(new_name_list->AddValueTableColumn(
        alias, resolved_columns[0], path_expr));
    ZETASQL_RETURN_IF_ERROR(new_name_list->SetIsValueTable());
    name_list = std::move(new_name_list);
  } else {
    resolved_columns.reserve(tvf_relation.num_columns());
    for (const TVFRelation::Column& column : tvf_relation.columns()) {
      resolved_columns.push_back(ResolvedColumn(
          AllocateColumnId(),
          id_string_pool_->Make(path_expr->first_name()->GetAsString()),
          id_string_pool_->Make(column.name), column.type));
      ZETASQL_RETURN_IF_ERROR(new_name_list->AddColumn(
          resolved_columns.back().name_id(), resolved_columns.back(),
          /*is_explicit=*/true));
    }
    name_list = std::move(new_name_list);
    // Add a range variable for the TVF relation argument scan.
    ZETASQL_ASSIGN_OR_RETURN(name_list, NameList::AddRangeVariableInWrappingNameList(
                                    alias, ast_location, name_list));
  }
  auto relation_argument_scan = MakeResolvedRelationArgumentScan(
      resolved_columns, path_expr->first_name()->GetAsString(),
      tvf_relation.is_value_table());
  if (hint != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(hint, relation_argument_scan.get()));
  }
  MaybeRecordParseLocation(path_expr, relation_argument_scan.get());
  this_scan = std::move(relation_argument_scan);

  *output_name_list = name_list;
  *output = std::move(this_scan);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveTableSubquery(
    const ASTTableSubquery* table_ref, const NameScope* scope,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  // Table subqueries cannot have correlated references to tables earlier in
  // the same FROM clause, but should still be able to see correlated names
  // from outer queries.  This is handled by the caller, who passes us the
  // external name scope excluding any local aliases.

  IdString alias;
  if (table_ref->alias() != nullptr) {
    alias = table_ref->alias()->GetAsIdString();
  } else {
    alias = AllocateSubqueryName();
  }
  ZETASQL_RET_CHECK(!alias.empty());

  std::unique_ptr<const ResolvedScan> resolved_subquery;
  std::shared_ptr<const NameList> subquery_name_list;
  ZETASQL_RETURN_IF_ERROR(ResolveQuery(table_ref->subquery(), scope, alias,
                               /*is_outer_query=*/false, &resolved_subquery,
                               &subquery_name_list));
  ZETASQL_RET_CHECK(nullptr != subquery_name_list);

  // A table subquery never preserves order, so we clear is_ordered on the
  // final scan of the subquery, even if it was a ResolvedOrderByScan.
  const_cast<ResolvedScan*>(resolved_subquery.get())->set_is_ordered(false);

  if (table_ref->alias() == nullptr) {
    // For a table subquery without an alias, the NameList from the subquery
    // should propagate unchanged, including range variables, pseudo-columns
    // and is_value_table.
    if (subquery_name_list->is_value_table() &&
        !subquery_name_list->HasValueTableColumns()) {
      // This handles the case where the input is marked with is_value_table,
      // but it doesn't have a value table column.  This happens for UNION
      // of value tables, for one example.  Calling this function
      // builds a NameList containing a value table column.
      auto new_name_list = std::make_shared<NameList>();
      ZETASQL_RETURN_IF_ERROR(ConvertValueTableNameListToNameListWithValueTable(
          table_ref, alias, subquery_name_list, &new_name_list));
      *output_name_list = new_name_list;
    } else {
      *output_name_list = subquery_name_list;
    }
  } else {
    ZETASQL_RETURN_IF_ERROR(UpdateNameListForTableAlias(
        table_ref, alias, subquery_name_list, output_name_list));
  }

  if (table_ref->pivot_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolvePivotClause(
        std::move(resolved_subquery), *output_name_list, scope,
        /*input_is_subquery=*/true, table_ref->pivot_clause(),
        &resolved_subquery, output_name_list));
  }

  if (table_ref->unpivot_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveUnpivotClause(
        std::move(resolved_subquery), *output_name_list, scope,
        table_ref->unpivot_clause(), &resolved_subquery, output_name_list));
  }
  if (table_ref->sample_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveTablesampleClause(
        table_ref->sample_clause(), output_name_list, &resolved_subquery));
  }

  *output = std::move(resolved_subquery);
  return absl::OkStatus();
}

// Validates the parameter to PERCENT. It can either be a literal or a
// parameter. In either case, its type must be a double or an int64_t.
static absl::Status CheckPercentIsValid(
    const ASTNode* ast_location,
    const std::unique_ptr<const ResolvedExpr>& expr) {
  if ((expr->node_kind() != RESOLVED_PARAMETER &&
       expr->node_kind() != RESOLVED_LITERAL) ||
      (!expr->type()->IsInt64() && !expr->type()->IsDouble())) {
    return MakeSqlErrorAt(ast_location)
           << "PERCENT expects either a double or an integer literal or "
              "parameter";
  }

  if (expr->node_kind() == RESOLVED_LITERAL) {
    // If a literal, we can also validate its value.
    const Value value = expr->GetAs<ResolvedLiteral>()->value();
    bool is_valid = false;
    if (value.type()->IsInt64()) {
      is_valid = (!value.is_null() && value.int64_value() >= 0 &&
                  value.int64_value() <= 100);
    } else {
      ZETASQL_RET_CHECK(value.type()->IsDouble());
      is_valid = (!value.is_null() && value.double_value() >= 0.0 &&
                  value.double_value() <= 100.0);
    }
    if (!is_valid) {
      return MakeSqlErrorAt(ast_location)
             << "PERCENT value must be in the range [0, 100]";
    }
  }
  return absl::OkStatus();
}

// Note that this takes a NameList rather than a NameScope.  That means it
// won't be able to resolve correlated references.  The only places names
// are resolved is in PARTITION BY, which is somewhat obscure.
// This was probably an accidental omission, but seems unlikely to show up much.
absl::Status Resolver::ResolveTablesampleClause(
    const ASTSampleClause* sample_clause,
    std::shared_ptr<const NameList>* current_name_list,
    std::unique_ptr<const ResolvedScan>* current_scan) {
  if (!language().LanguageFeatureEnabled(FEATURE_TABLESAMPLE)) {
    return MakeSqlErrorAt(sample_clause) << "TABLESAMPLE not supported";
  }

  ZETASQL_RET_CHECK(sample_clause->sample_method() != nullptr);
  const ASTIdentifier* method = sample_clause->sample_method();

  ZETASQL_RET_CHECK(sample_clause->sample_size() != nullptr);
  std::unique_ptr<const ResolvedExpr> resolved_size;
  const ASTExpression* size = sample_clause->sample_size()->size();
  static constexpr char kTablesampleClause[] = "TABLESAMPLE clause";
  ZETASQL_RETURN_IF_ERROR(ResolveScalarExpr(size, empty_name_scope_.get(),
                                    kTablesampleClause, &resolved_size));

  const int unit = sample_clause->sample_size()->unit();
  ZETASQL_RET_CHECK(unit == ASTSampleSize::ROWS || unit == ASTSampleSize::PERCENT)
      << unit;
  const ResolvedSampleScan::SampleUnit resolved_unit =
      (unit == ASTSampleSize::ROWS) ? ResolvedSampleScan::ROWS
                                    : ResolvedSampleScan::PERCENT;
  if (resolved_unit == ResolvedSampleScan::ROWS) {
    ZETASQL_RETURN_IF_ERROR(ValidateParameterOrLiteralAndCoerceToInt64IfNeeded(
        /*clause_name=*/"ROWS", size, &resolved_size));
  } else {
    ZETASQL_RETURN_IF_ERROR(CheckPercentIsValid(size, resolved_size));
  }

  std::vector<std::unique_ptr<const ResolvedExpr>> partition_by_list;
  const NameScope name_scope(**current_name_list);
  QueryResolutionInfo query_info(this);
  if (sample_clause->sample_size()->partition_by() != nullptr) {
    if (!language().LanguageFeatureEnabled(
            FEATURE_STRATIFIED_RESERVOIR_TABLESAMPLE)) {
      return MakeSqlErrorAt(sample_clause->sample_size()->partition_by())
             << "TABLESAMPLE does not support the PARTITION BY clause";
    }
    const std::string method = sample_clause->sample_method()->GetAsString();
    if (!zetasql_base::CaseEqual(method, "reservoir")) {
      return MakeSqlErrorAt(sample_clause->sample_size()->partition_by())
             << "The TABLESAMPLE " << method
             << " method does not support PARTITION BY. "
             << "Remove PARTITION BY, or use the TABLESAMPLE RESERVOIR method";
    }
    if (resolved_unit != ResolvedSampleScan::ROWS) {
      return MakeSqlErrorAt(sample_clause->sample_size()->partition_by())
             << "TABLESAMPLE with PERCENT does not support PARTITION BY. "
             << "Remove PARTITION BY, or use TABLESAMPLE RESERVOIR with ROWS";
    }
    ZETASQL_RETURN_IF_ERROR(ResolveCreateTablePartitionByList(
        sample_clause->sample_size()
            ->partition_by()
            ->partitioning_expressions(),
        PartitioningKind::PARTITION_BY, name_scope, &query_info,
        &partition_by_list));
  }

  std::unique_ptr<const ResolvedExpr> resolved_repeatable_argument;
  if (sample_clause->sample_suffix() != nullptr &&
      sample_clause->sample_suffix()->repeat() != nullptr) {
    ZETASQL_RET_CHECK(sample_clause->sample_suffix()->repeat()->argument() != nullptr);
    const ASTExpression* repeatable_argument =
        sample_clause->sample_suffix()->repeat()->argument();
    ZETASQL_RETURN_IF_ERROR(
        ResolveScalarExpr(repeatable_argument, empty_name_scope_.get(),
                          kTablesampleClause, &resolved_repeatable_argument));
    ZETASQL_RETURN_IF_ERROR(ValidateParameterOrLiteralAndCoerceToInt64IfNeeded(
        /*clause_name=*/"REPEATABLE", repeatable_argument,
        &resolved_repeatable_argument));
  }

  // Resolve WITH WEIGHT if present.
  std::unique_ptr<ResolvedColumnHolder> weight_column;
  ResolvedColumnList output_column_list = (*current_scan)->column_list();
  if (sample_clause->sample_suffix() != nullptr &&
      sample_clause->sample_suffix()->weight() != nullptr) {
    // If the alias is NULL, we get "weight" as an implicit alias.
    const ASTAlias* with_weight_alias =
        sample_clause->sample_suffix()->weight()->alias();
    const IdString weight_alias =
        (with_weight_alias == nullptr ? kWeightAlias
                                      : with_weight_alias->GetAsIdString());

    const ResolvedColumn column(AllocateColumnId(),
                                /*table_name=*/kWeightId, /*name=*/weight_alias,
                                type_factory_->get_double());
    weight_column = MakeResolvedColumnHolder(column);
    output_column_list.push_back(weight_column->column());
    std::shared_ptr<NameList> name_list(new NameList);
    ZETASQL_RETURN_IF_ERROR(name_list->MergeFrom(**current_name_list, sample_clause));
    ZETASQL_RETURN_IF_ERROR(name_list->AddValueTableColumn(
        weight_alias, weight_column->column(),
        with_weight_alias != nullptr
            ? absl::implicit_cast<const ASTNode*>(with_weight_alias)
            : sample_clause->sample_suffix()->weight()));
    *current_name_list = name_list;
  }

  *current_scan = MakeResolvedSampleScan(
      output_column_list, std::move(*current_scan),
      absl::AsciiStrToLower(method->GetAsString()), std::move(resolved_size),
      resolved_unit, std::move(resolved_repeatable_argument),
      std::move(weight_column), std::move(partition_by_list));
  return absl::OkStatus();
}

// There's not much resolving left to do here.  We already know we have
// an identifier that matches a WITH subquery alias, so we build the
// ResolvedWithRefScan.
absl::Status Resolver::ResolveNamedSubqueryRef(
    const ASTPathExpression* table_path, const ASTHint* hint,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  ZETASQL_RET_CHECK(table_path != nullptr);
  auto it = named_subquery_map_.find(table_path->ToIdStringVector());
  ZETASQL_RET_CHECK(it != named_subquery_map_.end() && !it->second.empty());
  const NamedSubquery* named_subquery = it->second.back().get();
  if (named_subquery == nullptr) {
    // This is possible only if we have a recursive reference from within the
    // non-recursive term of a recursive UNION. This is an error.
    return MakeSqlErrorAt(table_path)
           << "Recursive reference is not allowed in non-recursive UNION term";
  }

  // For each new column produced in the WithRefScan, we want to name it
  // using the WITH alias, not the original column name.  e.g. In
  //   WITH Q AS (SELECT Key K FROM KeyValue)
  //   SELECT * FROM Q;
  // we want to call the new column Q.K, not Q.Key.  Since the column_list
  // may not map 1:1 with select-list column names, we need to build a map.
  std::map<ResolvedColumn, IdString> with_column_to_alias;
  for (const NamedColumn& named_column : named_subquery->name_list->columns()) {
    zetasql_base::InsertIfNotPresent(&with_column_to_alias, named_column.column(),
                            named_column.name());
  }

  // Make a new ResolvedColumn for each column from the WITH scan.
  // This is necessary so that if the WITH subquery is referenced twice,
  // we get distinct column names for each scan.
  ResolvedColumnList column_list;
  std::map<ResolvedColumn, ResolvedColumn> old_column_to_new_column;
  for (int i = 0; i < named_subquery->column_list.size(); ++i) {
    const ResolvedColumn& column = named_subquery->column_list[i];

    // Get the alias for the column produced by the WITH reference,
    // using the first alias for that column in the WITH subquery.
    // Every column in the column_list should correspond to at least column
    // in the WITH subquery's NameList.
    IdString new_column_alias;
    const IdString* found = zetasql_base::FindOrNull(with_column_to_alias, column);
    ZETASQL_RET_CHECK(found != nullptr) << column.DebugString();
    new_column_alias = *found;

    column_list.emplace_back(
        ResolvedColumn(AllocateColumnId(), named_subquery->unique_alias,
                       new_column_alias, column.annotated_type()));
    // Build mapping from WITH subquery column to the newly created column
    // for the WITH reference.
    old_column_to_new_column[column] = column_list.back();
    // We can't prune any columns from the ResolvedWithRefScan because they
    // need to match 1:1 with the column_list on the with subquery.
    RecordColumnAccess(column_list.back());
  }

  // Make a new NameList pointing at the new ResolvedColumns.
  std::shared_ptr<NameList> name_list(new NameList);
  for (const NamedColumn& named_column : named_subquery->name_list->columns()) {
    const ResolvedColumn& old_column = named_column.column();
    auto found_column = old_column_to_new_column.find(old_column);
    ZETASQL_RET_CHECK(found_column != old_column_to_new_column.end());
    const ResolvedColumn& new_column = found_column->second;

    ZETASQL_RETURN_IF_ERROR(name_list->AddColumnMaybeValueTable(
        named_column.name(), new_column, named_column.is_explicit(), table_path,
        named_subquery->name_list->is_value_table()));
  }
  if (named_subquery->name_list->is_value_table()) {
    ZETASQL_RETURN_IF_ERROR(name_list->SetIsValueTable());
  }

  std::unique_ptr<ResolvedScan> scan;
  if (named_subquery->is_recursive) {
    std::unique_ptr<ResolvedRecursiveRefScan> recursive_scan =
        MakeResolvedRecursiveRefScan(column_list);

    // Remember information about the recursive scan, which will be needed for
    // downstream validation checks.
    recursive_ref_info_[recursive_scan.get()] = RecursiveRefScanInfo{
        table_path,
        named_subquery->unique_alias,
    };
    scan = std::move(recursive_scan);
  } else {
    scan = MakeResolvedWithRefScan(column_list,
                                   named_subquery->unique_alias.ToString());
  }
  ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(hint, scan.get()));

  *output = std::move(scan);
  *output_name_list = name_list;
  return absl::OkStatus();
}

absl::Status Resolver::ResolveColumnInUsing(
    const ASTIdentifier* ast_identifier, const NameList& name_list,
    absl::string_view side_name, IdString key_name,
    ResolvedColumn* found_column,
    std::unique_ptr<const ResolvedExpr>* compute_expr_for_found_column) {
  compute_expr_for_found_column->reset();
  // <ast_identifier> and <found_column> are redundant but we pass the
  // string in to avoid doing extra string copy.
  ABSL_DCHECK_EQ(ast_identifier->GetAsIdString(), key_name);

  NameTarget found_name;
  if (!name_list.LookupName(key_name, &found_name)) {
    return MakeSqlErrorAt(ast_identifier)
           << "Column " << key_name << " in USING clause not found on "
           << side_name << " side of join";
  }
  if (in_strict_mode() && found_name.IsImplicit()) {
    return MakeSqlErrorAt(ast_identifier)
           << "Column name " << ToIdentifierLiteral(key_name)
           << " cannot be used without a qualifier in strict name resolution"
           << " mode. Use JOIN ON with a qualified name instead";
  }
  switch (found_name.kind()) {
    case NameTarget::ACCESS_ERROR:
      // An ACCESS_ERROR should not be possible in this context, since
      // ACCESS_ERROR NameTargets only exist during post-GROUP BY processing,
      // and the USING clause is evaluated pre-GROUP BY.
      ZETASQL_RET_CHECK_FAIL() << "Accessing column " << key_name
                       << " in USING clause is invalid";
    case NameTarget::AMBIGUOUS:
      return MakeSqlErrorAt(ast_identifier)
             << "Column " << key_name << " in USING clause is ambiguous on "
             << side_name << " side of join";
    case NameTarget::RANGE_VARIABLE:
      if (found_name.scan_columns()->is_value_table()) {
        ZETASQL_RET_CHECK_EQ(found_name.scan_columns()->num_columns(), 1);
        *found_column = found_name.scan_columns()->column(0).column();
        break;
      } else {
        return MakeSqlErrorAt(ast_identifier)
               << "Name " << key_name
               << " in USING clause is a table alias, not a column name, on "
               << side_name << " side of join";
      }
    case NameTarget::FIELD_OF: {
      // We have an implicit field access.  Make the ResolvedColumnRef for
      // the column and then resolve the field access on top.
      std::unique_ptr<const ResolvedExpr> resolved_get_field;
      // We don't auto-flatten for USING. It could make for matches that look
      // the same but come from different structure which would be weird.
      ZETASQL_RETURN_IF_ERROR(ResolveFieldAccess(
          MakeColumnRef(found_name.column_containing_field()),
          ast_identifier->GetParseLocationRange(), ast_identifier,
          /*flatten_state=*/nullptr, &resolved_get_field));

      // Then create a new ResolvedColumn to store this result.
      *found_column = ResolvedColumn(
          AllocateColumnId(), MakeIdString(absl::StrCat("$join_", side_name)),
          key_name, resolved_get_field->annotated_type());

      *compute_expr_for_found_column = std::move(resolved_get_field);
      break;
    }
    case NameTarget::IMPLICIT_COLUMN:
    case NameTarget::EXPLICIT_COLUMN:
      *found_column = found_name.column();
      break;
  }
  return absl::OkStatus();
}

// static
absl::Status Resolver::MaybeAddJoinHintKeyword(const ASTJoin* ast_join,
                                               ResolvedScan* resolved_scan) {
  if (ast_join->join_hint() != ASTJoin::NO_JOIN_HINT) {
    // Convert HASH and LOOKUP to HASH_JOIN and LOOKUP_JOIN, respectively.
    absl::string_view hint;
    switch (ast_join->join_hint()) {
      case ASTJoin::HASH:
        hint = "HASH_JOIN";
        break;
      case ASTJoin::LOOKUP:
        hint = "LOOKUP_JOIN";
        break;
      case ASTJoin::NO_JOIN_HINT:
        ZETASQL_RET_CHECK_FAIL() << "Can't get here";
    }
    // Join hint keywords don't directly correspond to query text so we don't
    // record the parse location.
    resolved_scan->add_hint_list(MakeResolvedOption(
        /*qualifier=*/"", "join_type",
        MakeResolvedLiteralWithoutLocation(Value::String(hint))));
  }
  return absl::OkStatus();
}

absl::Status Resolver::ResolveUsing(
    const ASTUsingClause* using_clause, const NameList& name_list_lhs,
    const NameList& name_list_rhs, const ResolvedJoinScan::JoinType join_type,
    bool is_array_scan,
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>*
        lhs_computed_columns,
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>*
        rhs_computed_columns,
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>*
        computed_columns,
    NameList* output_name_list,
    std::unique_ptr<const ResolvedExpr>* join_condition) {
  ZETASQL_RET_CHECK(using_clause != nullptr);
  ZETASQL_RET_CHECK(computed_columns != nullptr);
  ZETASQL_RET_CHECK(output_name_list != nullptr);
  ZETASQL_RET_CHECK(join_condition != nullptr);

  IdStringSetCase column_names_emitted_by_using;

  std::vector<std::unique_ptr<const ResolvedExpr>> join_key_exprs;

  for (const ASTIdentifier* using_key : using_clause->keys()) {
    const IdString key_name = using_key->GetAsIdString();

    ResolvedColumn lhs_column, rhs_column;
    std::unique_ptr<const ResolvedExpr> lhs_compute_expr, rhs_compute_expr;
    ZETASQL_RETURN_IF_ERROR(ResolveColumnInUsing(using_key, name_list_lhs, "left",
                                         key_name, &lhs_column,
                                         &lhs_compute_expr));
    // NOTE: For ArrayScan, there is no rhs_scan we can push a Project into, so
    // we'll never materialize rhs_column. Instead, we'll use rhs_compute_expr
    // directly.
    ZETASQL_RETURN_IF_ERROR(ResolveColumnInUsing(using_key, name_list_rhs, "right",
                                         key_name, &rhs_column,
                                         &rhs_compute_expr));

    std::unique_ptr<const ResolvedExpr> lhs_expr = MakeColumnRef(lhs_column);
    std::unique_ptr<const ResolvedExpr> rhs_expr =
        is_array_scan && rhs_compute_expr != nullptr
            ? std::move(rhs_compute_expr)
            : MakeColumnRef(rhs_column);

    std::unique_ptr<const ResolvedExpr> join_key_expr;
    absl::Status status = MakeEqualityComparison(
        using_key, std::move(lhs_expr), std::move(rhs_expr), &join_key_expr);
    if (absl::IsInvalidArgument(status)) {
      // We assume there are only two major reasons when INVALID_ARGUMENT is
      // returned by MakeEqualityComparison:
      // 1) The column in USING from the table on both sides of join has the
      // same type, which does not support equality comparison;
      // 2) The column in USING from the table on both sides of join has
      // different types and are incompatible.
      // In particular, looking up catalog for equality operator should never
      // return INVALID_ARGUMENT.
      if (lhs_column.type()->Equivalent(rhs_column.type())) {
        return MakeSqlErrorAt(using_key)
               << "Column '" << ToIdentifierLiteral(key_name)
               << "' in USING has types that do not support equality "
                  "comparison: "
               << lhs_column.type()->ShortTypeName(product_mode());
      }
      return MakeSqlErrorAt(using_key)
             << "Column '" << ToIdentifierLiteral(key_name)
             << "' in USING has incompatible types that cannot be directly "
                "compared using equality on either side of the join: "
             << lhs_column.type()->ShortTypeName(product_mode()) << " and "
             << rhs_column.type()->ShortTypeName(product_mode());
    }
    // Propagate all other errors.
    ZETASQL_RETURN_IF_ERROR(status);
    join_key_exprs.push_back(std::move(join_key_expr));

    // The column name from inside USING should be visible as a column
    // exactly once, from the non-NULL side of the join.
    // As per specification, the output for the using column is:
    // 1) The lhs for INNER or LEFT JOIN
    // 2) The rhs for RIGHT JOIN
    // 3) A COALESCE(lhs, rhs) expression for FULL JOIN (whose result
    //    type is the supertype of lhs/rhs)
    if (zetasql_base::InsertIfNotPresent(&column_names_emitted_by_using, key_name)) {
      const IdString key_name = using_key->GetAsIdString();
      switch (join_type) {
        case ResolvedJoinScan::LEFT:
        case ResolvedJoinScan::INNER:
          // is_explicit=true because we always have a provided alias in
          // JOIN USING.
          ZETASQL_RETURN_IF_ERROR(output_name_list->AddColumn(key_name, lhs_column,
                                                      /*is_explicit=*/true));
          break;
        case ResolvedJoinScan::RIGHT:
          ZETASQL_RETURN_IF_ERROR(output_name_list->AddColumn(key_name, rhs_column,
                                                      /*is_explicit=*/true));
          break;
        case ResolvedJoinScan::FULL: {
          std::unique_ptr<const ResolvedExpr> coalesce_expr;
          ZETASQL_RETURN_IF_ERROR(MakeCoalesceExpr(using_key, {lhs_column, rhs_column},
                                           &coalesce_expr));
          const ResolvedColumn coalesce_column(AllocateColumnId(), kFullJoinId,
                                               key_name,
                                               coalesce_expr->annotated_type());
          computed_columns->push_back(MakeResolvedComputedColumn(
              coalesce_column, std::move(coalesce_expr)));
          ZETASQL_RETURN_IF_ERROR(output_name_list->AddColumn(key_name, coalesce_column,
                                                      /*is_explicit=*/true));
          // Mark the <coalesce_column> as referenced so that it does not get
          // pruned if column pruning is enabled in the AnalyzerOptions.
          // Pruning this column causes problems for the SQLBuilder and other
          // visitors that do not visit expressions computed in a ResolvedNode
          // unless that node actually projects the expressions.
          // TODO: Consider enhancing column pruning, so that when
          // pruning a computed column we determine if we can also prune the
          // related expression.
          RecordColumnAccess(coalesce_column);
          break;
        }
      }
    }

    if (lhs_compute_expr != nullptr) {
      lhs_computed_columns->push_back(
          MakeResolvedComputedColumn(lhs_column, std::move(lhs_compute_expr)));
    }
    if (rhs_compute_expr != nullptr) {
      rhs_computed_columns->push_back(
          MakeResolvedComputedColumn(rhs_column, std::move(rhs_compute_expr)));
    }
  }

  NameList::MergeOptions merge_options{.excluded_field_names =
                                           &column_names_emitted_by_using};

  ZETASQL_RETURN_IF_ERROR(
      output_name_list->MergeFrom(name_list_lhs, using_clause, merge_options));
  ZETASQL_RETURN_IF_ERROR(
      output_name_list->MergeFrom(name_list_rhs, using_clause, merge_options));

  return MakeAndExpr(using_clause, std::move(join_key_exprs), join_condition);
}

// Inside this method, `external_scope` is the scope including names that
// are visible coming from outside the join.
// We build additional NameScopes inside the method where we need to resolve
// expressions using names that may come from the lhs or rhs of this join.
absl::Status Resolver::ResolveJoin(
    const ASTJoin* join, const NameScope* external_scope,
    const NameScope* local_scope, std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  std::unique_ptr<const ResolvedScan> resolved_lhs;
  std::shared_ptr<const NameList> name_list_lhs;
  ZETASQL_RETURN_IF_ERROR(ResolveTableExpression(
      join->lhs(), external_scope, local_scope, &resolved_lhs, &name_list_lhs));

  auto scope_for_lhs_storage = std::make_unique<NameScope>(*name_list_lhs);
  const NameScope* scope_for_lhs = scope_for_lhs_storage.get();

  return ResolveJoinRhs(join, external_scope, scope_for_lhs, name_list_lhs,
                        std::move(resolved_lhs), output, output_name_list);
}

absl::Status Resolver::ResolveJoinRhs(
    const ASTJoin* join, const NameScope* external_scope,
    const NameScope* scope_for_lhs,
    const std::shared_ptr<const NameList>& name_list_lhs,
    std::unique_ptr<const ResolvedScan> resolved_lhs,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  bool disable_outer_join_array =
      language().LanguageFeatureEnabled(FEATURE_DISABLE_OUTER_JOIN_ARRAY);
  bool rhs_is_table_path_expr =
      join->rhs()->node_kind() == AST_TABLE_PATH_EXPRESSION;
  bool is_right_or_full_join =
      join->join_type() == ASTJoin::RIGHT || join->join_type() == ASTJoin::FULL;
  bool rhs_is_unnest_expr = false;
  bool is_correlated_array_join = false;
  bool rhs_is_correlated_array_in_subquery = false;
  std::string error_label;

  if (rhs_is_table_path_expr) {
    const ASTTablePathExpression* table_ref =
        join->rhs()->GetAsOrDie<ASTTablePathExpression>();
    const ASTPathExpression* rhs_path_expr = table_ref->path_expr();
    rhs_is_unnest_expr = rhs_path_expr == nullptr;
    bool rhs_is_dotted_path_expr =
        rhs_path_expr != nullptr && rhs_path_expr->num_names() > 1;
    is_correlated_array_join =
        rhs_is_dotted_path_expr &&
        IsPathExpressionStartingFromScope(*rhs_path_expr, *scope_for_lhs);
    rhs_is_correlated_array_in_subquery =
        rhs_is_dotted_path_expr &&
        IsPathExpressionStartingFromScope(*rhs_path_expr, *external_scope);

    if (rhs_is_unnest_expr) {
      error_label = "UNNEST expression";
    } else {
      error_label = rhs_path_expr->ToIdentifierPathString();
    }

    // TODO: Update the error message after the rollout of
    // outer join independent array is done.
    // Showing legacy error message for backward compatibility.
    if (disable_outer_join_array &&
        (rhs_is_unnest_expr || is_correlated_array_join ||
         rhs_is_correlated_array_in_subquery)) {
      if (join->join_type() == ASTJoin::RIGHT) {
        return MakeSqlErrorAt(join->rhs())
               << "Array scan is not allowed with RIGHT JOIN: " << error_label;
      } else if (join->join_type() == ASTJoin::FULL) {
        return MakeSqlErrorAt(join->rhs())
               << "Array scan is not allowed with FULL JOIN: " << error_label;
      }
      if (table_ref->for_system_time() != nullptr) {
        return MakeSqlErrorAt(table_ref->for_system_time())
               << "FOR SYSTEM TIME is not allowed with array scan";
      }
    }
    // For backward compatibility, we decided not to update the error message of
    // NATURAL JOIN for the array reference case as of April 2023. But it is a
    // nice improvement to have in the future.
    if (join->natural() && (rhs_is_unnest_expr || is_correlated_array_join ||
                            rhs_is_correlated_array_in_subquery)) {
      return MakeSqlErrorAt(join->rhs())
             << "Array scan is not allowed with NATURAL JOIN: " << error_label;
    }
  }

  // Join trees are normally left deep and normally evaluated left to right.
  // There are two ways to introduce a right-deep shape in ZetaSQL:
  // 1) Use parenthesis to evaluate a join on the right first.
  // 2) Use consecutive ON or USING clauses.
  // In such cases, we need different scoping rules for the right deep part.
  //
  // From Date & Darwen, p143:
  //   ( ( T1 JOIN T2 ON cond1 )
  //     JOIN
  //     ( T3 JOIN T4 ON cond2 )
  //     ON cond3 )
  // * cond1 can see T1, T2
  // * cond2 can see T3, T4
  // * cond3 can see T1, T2, T3, T4
  // * The select-list can see T1, T2, T3, T4
  //
  // The same structure can be written without parens as:
  //   T1 JOIN T2 ON cond1 JOIN T3 JOIN T4 ON cond2 ON cond3
  //
  // There is a further refinement we don't currently support because we
  // don't allow aliases on parenthesized join.
  //   ( ( T1 JOIN T2 ON cond1 ) AS TA
  //     JOIN
  //     ( T3 JOIN T4 ON cond2 ) AS TB
  //     ON cond3 )
  // * Now, cond3 can see only TA, TB, and not T1, T2, T3, T4.
  // * The select-list can see only TA, TB, and not T1, T2, T3, T4.
  auto external_and_lhs_scope =
      std::make_unique<NameScope>(external_scope, name_list_lhs);
  const NameScope* rhs_from_scope;
  if (is_right_or_full_join ||
      // When the RHS is parenthsized
      join->rhs()->node_kind() == AST_PARENTHESIZED_JOIN ||
      // When the RHS is right-deep due to consecutive ON clauses.
      join->rhs()->node_kind() == AST_JOIN) {
    // A clean scope only includes external names (and none of the names
    // introduced locally in the same FROM clause). There are two cases where
    // we need a "clean" scope on the right hand side of the JOIN:
    //
    // 1. The rhs is not a parenthesized join (e.g. table names, path
    //    expressions, table subqueries) and the current JOIN type is RIGHT JOIN
    //    or FULL JOIN.
    //
    // 2. The right hand side is a join introduced by parenthesis or a tight
    //    binding ON or USING clause. See the example above the conditional.
    //
    // Otherwise, the lhs name scope is still needed because it is allowed to
    // have names correlated to the lhs of the JOIN.
    rhs_from_scope = external_scope;
  } else {
    rhs_from_scope = external_and_lhs_scope.get();
  }

  // Peek at rhs_node to see if we should try to resolve it as an array scan.
  // If the first identifier can be resolved inside <rhs_from_scope>, then try
  // to resolve this join as an array scan.
  if (rhs_is_table_path_expr) {
    const ASTTablePathExpression* table_ref =
        join->rhs()->GetAsOrDie<ASTTablePathExpression>();
    const ASTPathExpression* rhs_path_expr = table_ref->path_expr();
    // We may have an unnest_expr instead of a path_expr here.
    // Single-word identifiers are always resolved as table names.
    if ((rhs_is_unnest_expr && !is_right_or_full_join) ||
        is_correlated_array_join ||
        (rhs_is_correlated_array_in_subquery && !is_right_or_full_join)) {
      // Make sure this join is valid for an array scan.
      bool is_left_outer = false;
      switch (join->join_type()) {
        case ASTJoin::DEFAULT_JOIN_TYPE:
        case ASTJoin::CROSS:
        case ASTJoin::INNER:
        case ASTJoin::COMMA:
          break;  // These are all inner joins.
        case ASTJoin::LEFT:
          is_left_outer = true;
          break;
        case ASTJoin::RIGHT:
          return MakeSqlErrorAt(join->rhs())
                 << "Correlated array references are not allowed with RIGHT "
                    "JOIN: "
                 << error_label;
        case ASTJoin::FULL:
          return MakeSqlErrorAt(join->rhs())
                 << "Correlated array references are not allowed with FULL "
                    "JOIN: "
                 << error_label;
      }
      if (table_ref->for_system_time() != nullptr) {
        return MakeSqlErrorAt(table_ref->for_system_time())
               << "FOR SYSTEM TIME is not allowed with correlated array "
                  "reference";
      }

      std::optional<PathExpressionSpan> path_expr =
          table_ref->path_expr() != nullptr
              ? std::make_optional(PathExpressionSpan(*rhs_path_expr))
              : std::nullopt;
      return ResolveArrayScan(table_ref, path_expr, join->on_clause(),
                              join->using_clause(), join, is_left_outer,
                              /*include_lhs_name_list=*/true,
                              /*is_single_table_array_path=*/false,
                              &resolved_lhs, name_list_lhs, rhs_from_scope,
                              output, output_name_list);
    }
  }

  // Now we're in the normal table-scan case.
  std::unique_ptr<const ResolvedScan> resolved_rhs;
  std::shared_ptr<const NameList> name_list_rhs;
  ZETASQL_RETURN_IF_ERROR(ResolveTableExpression(join->rhs(), external_scope,
                                         rhs_from_scope, &resolved_rhs,
                                         &name_list_rhs));

  // True iff this join type accepts (and requires) an ON or USING clause.
  bool expect_join_condition;
  const char* join_type_name = "";  // For error messages.
  ResolvedJoinScan::JoinType resolved_join_type;
  bool has_using = false;
  switch (join->join_type()) {
    case ASTJoin::COMMA:
      // This is the only case without a "JOIN" keyword.
      // This join_type_name never gets used currently because none of the
      // error cases below apply for comma joins.
      join_type_name = "comma join";
      expect_join_condition = false;
      resolved_join_type = ResolvedJoinScan::INNER;
      break;
    case ASTJoin::CROSS:
      join_type_name = "CROSS JOIN";
      expect_join_condition = false;
      resolved_join_type = ResolvedJoinScan::INNER;
      break;
    case ASTJoin::DEFAULT_JOIN_TYPE:  // No join_type keyword - same as INNER.
    case ASTJoin::INNER:
      join_type_name = "INNER JOIN";
      expect_join_condition = true;
      resolved_join_type = ResolvedJoinScan::INNER;
      break;
    case ASTJoin::LEFT:
      join_type_name = "LEFT JOIN";
      expect_join_condition = true;
      resolved_join_type = ResolvedJoinScan::LEFT;
      break;
    case ASTJoin::RIGHT:
      join_type_name = "RIGHT JOIN";
      expect_join_condition = true;
      resolved_join_type = ResolvedJoinScan::RIGHT;
      break;
    case ASTJoin::FULL:
      join_type_name = "FULL JOIN";
      expect_join_condition = true;
      resolved_join_type = ResolvedJoinScan::FULL;
      break;
  }

  const char* natural_str = "";  // For error messages.
  if (join->natural()) {
    if (!expect_join_condition) {
      return MakeSqlErrorAtLocalNode(join->join_location())
             << "NATURAL cannot be used with " << join_type_name;
    }
    expect_join_condition = false;
    natural_str = "NATURAL ";

    return MakeSqlErrorAtLocalNode(join->join_location())
           << "Natural join not supported";
  }

  // This stores the extra casted (for LEFT, RIGHT, INNER JOIN) and
  // coalesced columns (for FULL JOIN) that may be required for USING.
  // These columns are computed after the join.
  std::vector<std::unique_ptr<const ResolvedComputedColumn>> computed_columns;

  std::shared_ptr<NameList> name_list(new NameList);
  std::unique_ptr<const ResolvedExpr> join_condition;

  if (join->using_clause() != nullptr) {
    ZETASQL_RET_CHECK(join->on_clause() == nullptr);  // Can't have both.
    if (!expect_join_condition) {
      return MakeSqlErrorAt(join->using_clause())
             << "USING clause cannot be used with " << natural_str
             << join_type_name;
    }
    has_using = true;

    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        lhs_computed_columns;
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        rhs_computed_columns;

    ZETASQL_RETURN_IF_ERROR(ResolveUsing(join->using_clause(), *name_list_lhs,
                                 *name_list_rhs, resolved_join_type,
                                 /*is_array_scan=*/false, &lhs_computed_columns,
                                 &rhs_computed_columns, &computed_columns,
                                 name_list.get(), &join_condition));

    // Add a Project for any columns we need to computed before the join.
    ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
        std::move(lhs_computed_columns), &resolved_lhs));
    ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
        std::move(rhs_computed_columns), &resolved_rhs));
  } else {
    ZETASQL_RETURN_IF_ERROR(
        name_list->MergeFrom(*name_list_lhs, join->lhs()->alias_location()));
    ZETASQL_RETURN_IF_ERROR(
        name_list->MergeFrom(*name_list_rhs, join->rhs()->alias_location()));

    static constexpr char kJoinOnClause[] = "JOIN ON clause";
    if (join->on_clause() != nullptr) {
      if (!expect_join_condition) {
        return MakeSqlErrorAt(join->on_clause())
               << "ON clause cannot be used with " << natural_str
               << join_type_name;
      }

      const std::unique_ptr<const NameScope> on_scope(
          new NameScope(external_scope, name_list));

      ZETASQL_RETURN_IF_ERROR(ResolveScalarExpr(join->on_clause()->expression(),
                                        on_scope.get(), kJoinOnClause,
                                        &join_condition));
      ZETASQL_RETURN_IF_ERROR(CoerceExprToBool(join->on_clause()->expression(),
                                       kJoinOnClause, &join_condition));
    } else {
      // No ON or USING clause.
      if (expect_join_condition) {
        return MakeSqlErrorAtLocalNode(join->join_location())
               << natural_str << join_type_name
               << " must have an immediately following ON or USING clause";
      }
    }
  }

  *output_name_list = name_list;

  return AddScansForJoin(join, std::move(resolved_lhs), std::move(resolved_rhs),
                         resolved_join_type, has_using,
                         std::move(join_condition), std::move(computed_columns),
                         output);
}

absl::Status Resolver::AddScansForJoin(
    const ASTJoin* join, std::unique_ptr<const ResolvedScan> resolved_lhs,
    std::unique_ptr<const ResolvedScan> resolved_rhs,
    ResolvedJoinScan::JoinType resolved_join_type, bool has_using,
    std::unique_ptr<const ResolvedExpr> join_condition,
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> computed_columns,
    std::unique_ptr<const ResolvedScan>* output_scan) {
  ResolvedColumnList concat_columns = ConcatColumnLists(
      resolved_lhs->column_list(), resolved_rhs->column_list());
  std::unique_ptr<ResolvedJoinScan> resolved_join = MakeResolvedJoinScan(
      concat_columns, resolved_join_type, std::move(resolved_lhs),
      std::move(resolved_rhs), std::move(join_condition), has_using);

  // If we have a join_type keyword hint (e.g. HASH JOIN or LOOKUP JOIN),
  // add it on the front of hint_list, before any long-form hints.
  ZETASQL_RETURN_IF_ERROR(MaybeAddJoinHintKeyword(join, resolved_join.get()));

  ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(join->hint(), resolved_join.get()));

  if (join->join_location() != nullptr) {
    MaybeRecordParseLocation(join->join_location()->GetParseLocationRange(),
                             resolved_join.get());
  }

  *output_scan = std::move(resolved_join);

  // If we created a CAST expression for RIGHT/INNER/LEFT JOIN or a
  // COALESCE for FULL JOIN with USING, we create a wrapper ProjectScan node to
  // produce additional columns for those expressions.
  ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(std::move(computed_columns),
                                                    output_scan));

  return absl::OkStatus();
}

absl::Status Resolver::ResolveParenthesizedJoin(
    const ASTParenthesizedJoin* parenthesized_join,
    const NameScope* external_scope, const NameScope* local_scope,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  std::unique_ptr<const ResolvedScan> resolved_join;
  ZETASQL_RETURN_IF_ERROR(ResolveJoin(parenthesized_join->join(), external_scope,
                              local_scope, &resolved_join, output_name_list));

  if (parenthesized_join->sample_clause()) {
    ZETASQL_RETURN_IF_ERROR(ResolveTablesampleClause(
        parenthesized_join->sample_clause(), output_name_list, &resolved_join));
  }

  *output = std::move(resolved_join);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveGroupRowsTVF(
    const ASTTVF* ast_tvf, std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* group_rows_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();
  if (name_lists_for_group_rows_.empty()) {
    return MakeSqlErrorAt(ast_tvf)
           << "GROUP_ROWS() can only be used inside WITH GROUP ROWS clause";
  }

  std::shared_ptr<const NameList> from_clause_name_list =
      name_lists_for_group_rows_.top().name_list;
  name_lists_for_group_rows_.top().group_rows_tvf_used = true;

  // Clone the FROM clause's name list. Also remember mapping and new columns in
  // column_list and in out_cols.
  ResolvedColumnList column_list;
  // For each cloned column, create a new computed column with a
  // column reference pointing to the column in the original table.
  // This is necessary so that if the GROUP_ROWS() is referenced twice,
  // we get distinct column ids for each scan.
  std::vector<std::unique_ptr<const ResolvedComputedColumn>> out_cols;
  absl::flat_hash_map<ResolvedColumn, ResolvedColumn> cloned_columns;
  auto clone_column = [this, &column_list, &out_cols, &cloned_columns](
                          const ResolvedColumn& from_clause_column) {
    ResolvedColumn group_rows_column(
        AllocateColumnId(), from_clause_column.table_name_id(),
        from_clause_column.name_id(), from_clause_column.type());
    cloned_columns[from_clause_column] = group_rows_column;
    column_list.emplace_back(group_rows_column);
    out_cols.push_back(MakeResolvedComputedColumn(
        group_rows_column, MakeColumnRef(from_clause_column)));
    return group_rows_column;
  };

  absl::string_view value_table_error =
      "Value tables are not allowed to pass through GROUP_ROWS() TVF";
  // Table value currently does not propagate. When it is encountered,
  // value_table_error is raised.
  ZETASQL_ASSIGN_OR_RETURN(auto cloned_name_list,
                   from_clause_name_list->CloneWithNewColumns(
                       ast_tvf, value_table_error, ast_tvf->alias(),
                       clone_column, id_string_pool_));

  ZETASQL_RET_CHECK_EQ(cloned_name_list->num_columns(),
               from_clause_name_list->num_columns());

  std::string alias;
  *group_rows_name_list = std::move(cloned_name_list);
  if (ast_tvf->alias() != nullptr) {
    alias = ast_tvf->alias()->GetAsString();
  }

  // Resolve the query hint, if present.
  std::vector<std::unique_ptr<const ResolvedOption>> hints;
  if (ast_tvf->hint() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveHintAndAppend(ast_tvf->hint(), &hints));
  }

  std::unique_ptr<ResolvedGroupRowsScan> group_rows_scan =
      MakeResolvedGroupRowsScan(column_list, std::move(out_cols), alias);
  group_rows_scan->set_hint_list(std::move(hints));

  *output = std::move(group_rows_scan);
  return absl::OkStatus();
}

namespace {
// Vertifies that argument of TVF call inside <resolved_tvf_args> has no
// collation, i.e. each argument is not a scalar argument of collated type or a
// table argument with collated columns. An error will be thrown if the
// verification fails. The input <ast_locations> is expected to match 1:1 to the
// arguments in the <resolved_tvf_args>, and it is mainly used for error
// message.
absl::Status CheckTVFArgumentHasNoCollation(
    std::vector<ResolvedTVFArg>& resolved_tvf_args,
    const std::vector<const ASTNode*> ast_locations) {
  ZETASQL_RET_CHECK_EQ(ast_locations.size(), resolved_tvf_args.size());
  for (int i = 0; i < resolved_tvf_args.size(); ++i) {
    if (resolved_tvf_args[i].IsExpr()) {
      ZETASQL_ASSIGN_OR_RETURN(const ResolvedExpr* const expr,
                       resolved_tvf_args[i].GetExpr());
      if (CollationAnnotation::ExistsIn(expr->type_annotation_map())) {
        ZETASQL_RET_CHECK(ast_locations[i] != nullptr);
        return MakeSqlErrorAt(ast_locations[i])
               << "Collation "
               << expr->type_annotation_map()->DebugString(
                      CollationAnnotation::GetId())
               << " on argument of TVF call is not allowed";
      }
    } else if (resolved_tvf_args[i].IsScan()) {
      ZETASQL_ASSIGN_OR_RETURN(std::shared_ptr<const NameList> name_list,
                       resolved_tvf_args[i].GetNameList());
      const std::vector<ResolvedColumn> column_list =
          name_list->GetResolvedColumns();
      for (const ResolvedColumn& col : column_list) {
        if (CollationAnnotation::ExistsIn(col.type_annotation_map())) {
          std::string column_str = name_list->is_value_table()
                                       ? "value-table column"
                                       : "column " + col.name();
          ZETASQL_RET_CHECK(ast_locations[i] != nullptr);
          return MakeSqlErrorAt(ast_locations[i])
                 << "Collation "
                 << col.type_annotation_map()->DebugString(
                        CollationAnnotation::GetId())
                 << " on " << column_str
                 << " of argument of TVF call is not allowed";
        }
      }
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<const TableValuedFunction*> FindTVFOrMakeNiceError(
    absl::string_view tvf_name_string, const ASTTVF* ast_tvf,
    const AnalyzerOptions& analyzer_options, Catalog* catalog) {
  const TableValuedFunction* tvf_catalog_entry = nullptr;
  const absl::Status find_status = catalog->FindTableValuedFunction(
      ast_tvf->name()->ToIdentifierVector(), &tvf_catalog_entry,
      analyzer_options.find_options());
  if (find_status.code() == absl::StatusCode::kNotFound) {
    std::string error_message;
    absl::StrAppend(&error_message,
                    "Table-valued function not found: ", tvf_name_string);

    const std::string tvf_suggestion = catalog->SuggestTableValuedFunction(
        ast_tvf->name()->ToIdentifierVector());
    if (!tvf_suggestion.empty()) {
      absl::StrAppend(&error_message, "; Did you mean ", tvf_suggestion, "?");
    }

    return MakeSqlErrorAt(ast_tvf) << error_message;
  } else if (!find_status.ok()) {
    // The FindTableValuedFunction() call can return an invalid argument error,
    // for example, when looking up LazyResolutionTableFunctions (which are
    // resolved upon lookup).
    //
    // Rather than directly return the <find_status>, we update the location
    // of the error to indicate the function call in this statement.  We also
    // preserve the ErrorSource payload from <find_status>, since that
    // indicates source errors for this error.
    return WrapNestedErrorStatus(
        ast_tvf,
        absl::StrCat("Invalid table-valued function ", tvf_name_string),
        find_status, analyzer_options.error_message_mode());
  }
  ZETASQL_RET_CHECK_EQ(1, tvf_catalog_entry->NumSignatures())
      << "Each TVF has exactly one signature; overloading is not currently "
      << "allowed.";
  return tvf_catalog_entry;
}

}  // namespace

absl::Status Resolver::ResolveTVF(
    const ASTTVF* ast_tvf, const NameScope* external_scope,
    const NameScope* local_scope,
    std::vector<ResolvedTVFArg>* resolved_tvf_args,
    std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  if (language().LanguageFeatureEnabled(FEATURE_V_1_3_WITH_GROUP_ROWS)) {
    std::vector<std::string> fn_name = ast_tvf->name()->ToIdentifierVector();
    if (ast_tvf->name()->num_names() == 1 &&
        zetasql_base::CaseEqual(
            ast_tvf->name()->first_name()->GetAsIdString().ToStringView(),
            "GROUP_ROWS") &&
        ast_tvf->argument_entries().empty()) {
      return ResolveGroupRowsTVF(ast_tvf, output, output_name_list);
    }
  }

  // Check the language options to make sure TVFs are supported on this server.
  if (!language().LanguageFeatureEnabled(FEATURE_TABLE_VALUED_FUNCTIONS)) {
    return MakeSqlErrorAt(ast_tvf)
           << "Table-valued functions are not supported";
  }

  // Lookup into the catalog to get the TVF definition.
  const std::string tvf_name_string = ast_tvf->name()->ToIdentifierPathString();
  const IdString tvf_name_idstring = MakeIdString(tvf_name_string);
  ZETASQL_ASSIGN_OR_RETURN(const TableValuedFunction* tvf_catalog_entry,
                   FindTVFOrMakeNiceError(tvf_name_string, ast_tvf,
                                          analyzer_options_, catalog_));

  std::unique_ptr<FunctionSignature> result_signature;
  std::vector<TVFInputArgumentType> tvf_input_arguments;
  ZETASQL_RETURN_IF_ERROR(PrepareTVFInputArguments(
      tvf_name_string, ast_tvf, tvf_catalog_entry, external_scope, local_scope,
      &result_signature, resolved_tvf_args, &tvf_input_arguments));

  ZETASQL_RET_CHECK_EQ(resolved_tvf_args->size(), tvf_input_arguments.size());

  // Call the TableValuedFunction::Resolve method to get the output schema.
  // Use a new empty cycle detector, or the cycle detector from an enclosing
  // Resolver if we are analyzing one or more templated function calls.
  std::shared_ptr<TVFSignature> tvf_signature;
  absl::Status resolve_status;
  if (analyzer_options_.find_options().cycle_detector() == nullptr) {
    // AnalyzerOptions is a very large object, and this stack frame is already
    // huge. Allocate it on the heap to save some stack space.
    struct AnalyzerAndCycleDetector {
      CycleDetector owned_cycle_detector;
      AnalyzerOptions analyzer_options;
    };
    auto cycle = std::make_unique<AnalyzerAndCycleDetector>(
        AnalyzerAndCycleDetector{.analyzer_options = analyzer_options_});
    cycle->analyzer_options.mutable_find_options()->set_cycle_detector(
        &cycle->owned_cycle_detector);
    resolve_status = tvf_catalog_entry->Resolve(
        &cycle->analyzer_options, tvf_input_arguments, *result_signature,
        catalog_, type_factory_, &tvf_signature);
  } else {
    resolve_status = tvf_catalog_entry->Resolve(
        &analyzer_options_, tvf_input_arguments, *result_signature, catalog_,
        type_factory_, &tvf_signature);
  }

  if (!resolve_status.ok()) {
    // The Resolve method returned an error status that is already updated
    // based on the <analyzer_options> ErrorMessageMode.  Make a new
    // ErrorSource based on the <resolve_status>, and return a new error
    // status that indicates that the TVF call is invalid, while indicating
    // the TVF call location for the error.
    return WrapNestedErrorStatus(
        ast_tvf,
        absl::StrCat("Invalid table-valued function ", tvf_name_string),
        resolve_status, analyzer_options_.error_message_mode());
  }

  bool is_value_table = tvf_signature->result_schema().is_value_table();
  if (is_value_table) {
    ZETASQL_RETURN_IF_ERROR(
        CheckValidValueTableFromTVF(ast_tvf, tvf_catalog_entry->FullName(),
                                    tvf_signature->result_schema()));
  } else if (tvf_signature->result_schema().num_columns() == 0) {
    return MakeSqlErrorAt(ast_tvf)
           << "Table-valued functions must return at least one column, but "
           << "TVF " << tvf_catalog_entry->FullName() << " returned no columns";
  }

  // Fill the column and name list based on the output schema.
  // These columns match up 1:1 by position because we don't have a guarantee
  // of unique names. For value table, the output schema must have the value
  // column at index 0 and the rest columns if present must be pseudo columns.
  ResolvedColumnList column_list;
  std::shared_ptr<NameList> name_list(new NameList);
  column_list.reserve(tvf_signature->result_schema().num_columns());
  for (int i = 0; i < tvf_signature->result_schema().num_columns(); ++i) {
    const TVFRelation::Column& column =
        tvf_signature->result_schema().column(i);
    const IdString column_name = MakeIdString(
        !column.name.empty() ? column.name : absl::StrCat("$col", i));
    column_list.push_back(ResolvedColumn(AllocateColumnId(), tvf_name_idstring,
                                         column_name, column.annotated_type()));
    if (column.is_pseudo_column) {
      ZETASQL_RETURN_IF_ERROR(
          name_list->AddPseudoColumn(column_name, column_list.back(), ast_tvf));
    } else if (is_value_table) {
      ZETASQL_RET_CHECK_EQ(i, 0);  // Verified by CheckValidValueTableFromTVF
      // Defer AddValueTableColumn until after adding the pseudo-columns.
    } else {
      ZETASQL_RETURN_IF_ERROR(name_list->AddColumn(column_name, column_list.back(),
                                           /*is_explicit=*/true));
    }
  }
  if (is_value_table) {
    IdString alias = ast_tvf->alias() != nullptr
                         ? ast_tvf->alias()->GetAsIdString()
                         : MakeIdString("$col0");
    // So far, we've accumulated the pseudo-columns only.  Now add the
    // value table column, and pass in the list of pseudo-columns so they
    // can be attached to the range variable for the value table.
    ZETASQL_RETURN_IF_ERROR(name_list->AddValueTableColumn(
        alias, column_list[0], ast_tvf, /*excluded_field_names =*/{},
        name_list));
    ZETASQL_RETURN_IF_ERROR(name_list->SetIsValueTable());
  }
  *output_name_list = name_list;

  // If the TVF call has an alias, add a range variable to the name list so that
  // the enclosing query can refer to that alias.
  if (ast_tvf->alias() != nullptr && !is_value_table) {
    ZETASQL_ASSIGN_OR_RETURN(*output_name_list,
                     NameList::AddRangeVariableInWrappingNameList(
                         ast_tvf->alias()->GetAsIdString(), ast_tvf->alias(),
                         *output_name_list));
  }

  // Resolve the query hint, if present.
  std::vector<std::unique_ptr<const ResolvedOption>> hints;
  if (ast_tvf->hint() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveHintAndAppend(ast_tvf->hint(), &hints));
  }

  // Create the resolved TVF scan.
  std::vector<const ResolvedTVFArgument*> final_resolved_tvf_args;
  for (ResolvedTVFArg& arg : *resolved_tvf_args) {
    if (arg.IsExpr()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> expr,
                       arg.MoveExpr());
      final_resolved_tvf_args.push_back(
          MakeResolvedTVFArgument(std::move(expr), /*scan=*/nullptr,
                                  /*model=*/nullptr, /*connection=*/nullptr,
                                  /*descriptor_arg=*/nullptr,
                                  /*argument_column_list=*/{})
              .release());
    } else if (arg.IsScan()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedScan> scan,
                       arg.MoveScan());
      ZETASQL_ASSIGN_OR_RETURN(std::shared_ptr<const NameList> name_list,
                       arg.GetNameList());
      final_resolved_tvf_args.push_back(
          MakeResolvedTVFArgument(/*expr=*/nullptr, std::move(scan),
                                  /*model=*/nullptr, /*connection=*/nullptr,
                                  /*descriptor_arg=*/nullptr,
                                  name_list->GetResolvedColumns())
              .release());
    } else if (arg.IsConnection()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedConnection> connection,
                       arg.MoveConnection());
      final_resolved_tvf_args.push_back(
          MakeResolvedTVFArgument(/*expr=*/nullptr, /*scan=*/nullptr,
                                  /*model=*/nullptr, std::move(connection),
                                  /*descriptor_arg=*/nullptr,
                                  /*argument_column_list=*/{})
              .release());
    } else if (arg.IsDescriptor()) {
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedDescriptor> descriptor,
                       arg.MoveDescriptor());
      final_resolved_tvf_args.push_back(
          MakeResolvedTVFArgument(/*expr=*/nullptr, /*scan=*/nullptr,
                                  /*model=*/nullptr, /*connection=*/nullptr,
                                  /*descriptor_arg=*/std::move(descriptor),
                                  /*argument_column_list=*/{})
              .release());
    } else {
      ZETASQL_RET_CHECK(arg.IsModel());
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedModel> model,
                       arg.MoveModel());
      final_resolved_tvf_args.push_back(
          MakeResolvedTVFArgument(/*expr=*/nullptr, /*scan=*/nullptr,
                                  std::move(model), /*connection=*/nullptr,
                                  /*descriptor_arg=*/nullptr,
                                  /*argument_column_list=*/{})
              .release());
    }
  }
  std::string alias;
  if (ast_tvf->alias() != nullptr) {
    alias = ast_tvf->alias()->GetAsString();
  }

  // When this is true, we are setting <result_signature> into the
  // ResolvedTVFScan. This will happen when there are omitted arguments
  // preceding any provided arguments in the call and the engine might need the
  // result signature to figure out which arguments are provided.
  bool provide_result_signature = false;
  bool saw_omitted_arguments = false;
  for (const FunctionArgumentType& arg : result_signature->arguments()) {
    if (arg.optional() && arg.num_occurrences() == 0) {
      saw_omitted_arguments = true;
    } else if (saw_omitted_arguments) {
      provide_result_signature = true;
      break;
    }
  }

  std::vector<int> column_index_list(
      tvf_signature->result_schema().columns().size());
  // Fill column_index_list with 0, 1, 2, ..., column_list.size()-1.
  std::iota(column_index_list.begin(), column_index_list.end(), 0);

  if (tvf_catalog_entry->Is<SQLTableValuedFunction>() ||
      tvf_catalog_entry->Is<TemplatedSQLTVF>()) {
    analyzer_output_properties_.MarkRelevant(
        ResolvedASTRewrite::REWRITE_INLINE_SQL_TVFS);
  }

  auto tvf_scan = MakeResolvedTVFScan(
      column_list, tvf_catalog_entry, tvf_signature,
      std::move(final_resolved_tvf_args), column_index_list, alias,
      provide_result_signature ? std::move(result_signature) : nullptr);
  // WARNING: <result_signature> is destroyed at this point.

  tvf_scan->set_hint_list(std::move(hints));

  MaybeRecordTVFCallParseLocation(ast_tvf, tvf_scan.get());
  *output = std::move(tvf_scan);

  // Resolve the PIVOT clause, if present.
  if (ast_tvf->pivot_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolvePivotClause(
        std::move(*output), *output_name_list, external_scope,
        /*input_is_subquery=*/false, ast_tvf->pivot_clause(), output,
        output_name_list));
  }

  if (ast_tvf->unpivot_clause() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(ResolveUnpivotClause(
        std::move(*output), *output_name_list, external_scope,
        ast_tvf->unpivot_clause(), output, output_name_list));
  }
  // Resolve the TABLESAMPLE clause, if present.
  if (ast_tvf->sample() != nullptr) {
    if (!language().LanguageFeatureEnabled(
            FEATURE_TABLESAMPLE_FROM_TABLE_VALUED_FUNCTIONS)) {
      return MakeSqlErrorAt(ast_tvf->sample())
             << "TABLESAMPLE from table-valued function calls is not supported";
    }
    ZETASQL_RETURN_IF_ERROR(
        ResolveTablesampleClause(ast_tvf->sample(), output_name_list, output));
  }
  return absl::OkStatus();
}

absl::StatusOr<int> Resolver::MatchTVFSignature(
    const ASTTVF* ast_tvf, const TableValuedFunction* tvf_catalog_entry,
    const NameScope* external_scope, const NameScope* local_scope,
    const FunctionResolver& function_resolver,
    std::unique_ptr<FunctionSignature>* result_signature,
    std::vector<const ASTNode*>* arg_locations,
    std::vector<ResolvedTVFArg>* resolved_tvf_args,
    std::vector<NamedArgumentInfo>* named_arguments,
    SignatureMatchResult* signature_match_result) {
  ZETASQL_RET_CHECK_EQ(arg_locations->size(), resolved_tvf_args->size());

  // Get the TVF signature. Each TVF has exactly one signature; overloading is
  // not currently allowed.
  ZETASQL_RET_CHECK_EQ(1, tvf_catalog_entry->NumSignatures())
      << tvf_catalog_entry->DebugString();
  const int64_t signature_idx = 0;
  const FunctionSignature& function_signature =
      *tvf_catalog_entry->GetSignature(signature_idx);

  const int num_initial_args = static_cast<int>(resolved_tvf_args->size());
  const int num_ast_args = static_cast<int>(ast_tvf->argument_entries().size());
  const int num_tvf_args = num_initial_args + num_ast_args;

  if (num_initial_args > 0) {
    // Make sure the signature has enough table arguments as positional
    // arguments at the front to support these initial table args.
    bool signature_accepts_initial_args = true;
    bool named_only = false;
    if (function_signature.arguments().size() < num_initial_args) {
      signature_accepts_initial_args = false;
    } else {
      for (int sig_idx = 0; sig_idx < num_initial_args; ++sig_idx) {
        ZETASQL_RET_CHECK((*resolved_tvf_args)[sig_idx].IsScan());
        const FunctionArgumentType& arg_type =
            function_signature.argument(sig_idx);
        if (!arg_type.IsRelation()) {
          signature_accepts_initial_args = false;
          break;
        }
        if (arg_type.options().named_argument_kind() == kNamedOnly) {
          signature_accepts_initial_args = false;
          named_only = true;
          break;
        }
      }
    }
    if (!signature_accepts_initial_args) {
      return MakeSqlErrorAt(ast_tvf)
             << "Table-valued function cannot be called "
                "because its signature does not start with "
             << (num_initial_args == 1 ? "a" : absl::StrCat(num_initial_args))
             << (named_only ? " positional" : "") << " TABLE argument"
             << (num_initial_args > 1 ? "s" : "") << "; Supported signature: "
             << tvf_catalog_entry->GetSupportedSignaturesUserFacingText(
                    language(), /*print_template_and_name_details=*/true);
    }
  }

  // Check whether descriptors are present in this TVF call.
  bool descriptor_arg_present = false;
  for (const ASTTVFArgument* ast_tvf_arg : ast_tvf->argument_entries()) {
    if (ast_tvf_arg->descriptor() != nullptr) {
      descriptor_arg_present = true;
      break;
    }
  }

  // Resolve the TVF arguments. Each one becomes either an expression, a scan,
  // an ML model, a connection or a descriptor object. We allow correlation
  // references to the enclosing query if this TVF call is inside a scalar
  // subquery expression, but we do not allow references to columns in
  // previous tables in the same FROM clause as the TVF. For this reason, we
  // use 'external_scope' for the ExprResolutionInfo object here when
  // resolving expressions in the TVF call.
  absl::flat_hash_map<int, std::unique_ptr<const NameScope>>
      sig_idx_to_name_scope_map;
  arg_locations->reserve(num_tvf_args);
  resolved_tvf_args->reserve(num_tvf_args);
  for (int ast_idx = 0; ast_idx < num_ast_args; ++ast_idx) {
    int sig_idx = num_initial_args + ast_idx;  // Position in FunctionSignature
    const ASTExpression* ast_expr =
        ast_tvf->argument_entries()[ast_idx]->expr();
    if (ast_expr == nullptr) {
      arg_locations->push_back(ast_tvf->argument_entries()[ast_idx]);
    } else {
      arg_locations->push_back(ast_expr);
      if (ast_expr->node_kind() == AST_NAMED_ARGUMENT) {
        // Make sure the language feature is enabled.
        if (!language().LanguageFeatureEnabled(FEATURE_NAMED_ARGUMENTS)) {
          return MakeSqlErrorAt(ast_expr)
                 << "Named arguments are not supported";
        }
        // Add the named argument to the map.
        const ASTNamedArgument* named_arg = ast_expr->GetAs<ASTNamedArgument>();
        if (IsNamedLambda(named_arg)) {
          return MakeSqlErrorAt(named_arg->expr())
                 << "Lambda arguments are not implemented for TVF";
        }
        named_arguments->emplace_back(named_arg->name()->GetAsIdString(),
                                      sig_idx, named_arg);
        const absl::string_view arg_name =
            named_arg->name()->GetAsIdString().ToStringView();

        sig_idx = -1;
        for (int j = 0; j < function_signature.arguments().size(); ++j) {
          const FunctionArgumentType& arg_type = function_signature.argument(j);
          if (arg_type.has_argument_name() &&
              zetasql_base::CaseEqual(arg_type.argument_name(), arg_name)) {
            sig_idx = j;
            break;
          }
        }
        if (sig_idx == -1) {
          return MakeSqlErrorAt(ast_expr)
                 << "Named argument " << arg_name
                 << " not found in signature for call to function "
                 << tvf_catalog_entry->FullName();
        }
      }
    }
    // `sig_idx` is the position of the arg in the signature.
    auto tvf_arg_or_status = ResolveTVFArg(
        ast_tvf->argument_entries()[ast_idx], external_scope, local_scope,
        sig_idx < function_signature.arguments().size()
            ? &function_signature.argument(sig_idx)
            : nullptr,
        tvf_catalog_entry, sig_idx,
        descriptor_arg_present ? &sig_idx_to_name_scope_map : nullptr);
    ZETASQL_RETURN_IF_ERROR(tvf_arg_or_status.status());
    resolved_tvf_args->push_back(std::move(tvf_arg_or_status).value());
  }
  ZETASQL_RET_CHECK_EQ(arg_locations->size(), num_tvf_args);
  ZETASQL_RET_CHECK_EQ(resolved_tvf_args->size(), num_tvf_args);

  // We perform a second resolution pass for descriptors whose columns must be
  // resolved against a related table argument. The second pass is necessary
  // because column resolution is done with respect to the table's NameScope,
  // which requires that the related table argument is already resolved
  // (descriptor columns can reference table arguments that appear after them
  // in the function call).
  if (descriptor_arg_present) {
    // Store the NameLists for the initial table args so descriptor matching
    // can see them.
    for (int sig_idx = 0; sig_idx < num_initial_args; ++sig_idx) {
      ZETASQL_RET_CHECK(!sig_idx_to_name_scope_map.contains(sig_idx));
      ZETASQL_ASSIGN_OR_RETURN(auto name_list,
                       (*resolved_tvf_args)[sig_idx].GetNameList());
      sig_idx_to_name_scope_map[sig_idx] =
          std::make_unique<NameScope>(external_scope, name_list);
    }

    for (int ast_idx = 0; ast_idx < num_ast_args; ast_idx++) {
      const int sig_idx = ast_idx + num_initial_args;
      ResolvedTVFArg* resolved_tvf_arg = &(*resolved_tvf_args)[sig_idx];
      if (resolved_tvf_arg->IsDescriptor()) {
        const ASTTVFArgument* ast_tvf_arg =
            ast_tvf->argument_entries()[ast_idx];
        const FunctionArgumentType* function_argument =
            sig_idx < function_signature.arguments().size()
                ? &function_signature.argument(sig_idx)
                : nullptr;
        if (function_argument != nullptr) {
          std::optional<int> table_offset =
              function_argument->GetDescriptorResolutionTableOffset();
          if (table_offset.has_value()) {
            ZETASQL_RET_CHECK_GE(table_offset.value(), 0);
            if (!sig_idx_to_name_scope_map.contains(table_offset.value())) {
              return MakeSqlErrorAt(ast_tvf_arg)
                     << "DESCRIPTOR specifies resolving names from non-table "
                        "argument "
                     << table_offset.value();
            }

            std::unique_ptr<const ResolvedDescriptor> resolved_descriptor =
                resolved_tvf_arg->MoveDescriptor().value();
            ZETASQL_RETURN_IF_ERROR(FinishResolvingDescriptor(
                ast_tvf_arg, sig_idx_to_name_scope_map[table_offset.value()],
                table_offset.value(), &resolved_descriptor));
            resolved_tvf_arg->SetDescriptor(std::move(resolved_descriptor));
          }
        }
      }
    }
  }

  // Check if the function call contains any named arguments.
  const std::string tvf_name_string = ast_tvf->name()->ToIdentifierPathString();
  int repetitions = 0;
  int optionals = 0;
  ZETASQL_RET_CHECK_LE(arg_locations->size(), std::numeric_limits<int32_t>::max());

  std::vector<InputArgumentType> input_arg_types;
  input_arg_types.reserve(num_tvf_args);
  for (int i = 0; i < num_tvf_args; ++i) {
    auto input_arg_type_or_status = GetTVFArgType(resolved_tvf_args->at(i));
    ZETASQL_RETURN_IF_ERROR(input_arg_type_or_status.status());
    input_arg_types.push_back(std::move(input_arg_type_or_status).value());
  }

  signature_match_result->set_allow_mismatch_message(true);

  if (!SignatureArgumentCountMatches(
          function_signature, static_cast<int>(arg_locations->size()),
          &repetitions, &optionals, signature_match_result)) {
    return GenerateTVFNotMatchError(
        ast_tvf, *arg_locations, *signature_match_result, *tvf_catalog_entry,
        tvf_name_string, input_arg_types, signature_idx);
  }

  std::vector<ArgIndexEntry> arg_index_mapping;
  // TVFs can only have one signature for now, so either this call
  // returns an error, or the arguments match the signature.
  ZETASQL_ASSIGN_OR_RETURN(
      std::string mismatch_message,
      function_resolver.GetFunctionArgumentIndexMappingPerSignature(
          tvf_name_string, function_signature, ast_tvf, *arg_locations,
          *named_arguments, repetitions,
          /*always_include_omitted_named_arguments_in_index_mapping=*/false,
          analyzer_options_.show_function_signature_mismatch_details(),
          &arg_index_mapping));
  if (!mismatch_message.empty()) {
    if (signature_match_result->allow_mismatch_message()) {
      signature_match_result->set_mismatch_message(mismatch_message);
    }
    return GenerateTVFNotMatchError(
        ast_tvf, *arg_locations, *signature_match_result, *tvf_catalog_entry,
        tvf_name_string, input_arg_types, signature_idx);
  }
  ZETASQL_RETURN_IF_ERROR(
      FunctionResolver::
          ReorderInputArgumentTypesPerIndexMappingAndInjectDefaultValues(
              function_signature, arg_index_mapping, &input_arg_types));

  // Check if the TVF arguments match its signature. If not, return an error.
  ZETASQL_ASSIGN_OR_RETURN(
      const bool matches,
      function_resolver.SignatureMatches(
          *arg_locations, input_arg_types, function_signature,
          /*allow_argument_coercion=*/true, /*name_scope=*/nullptr,
          result_signature, signature_match_result, &arg_index_mapping,
          /*arg_overrides=*/nullptr));

  if (!matches) {
    return GenerateTVFNotMatchError(
        ast_tvf, *arg_locations, *signature_match_result, *tvf_catalog_entry,
        tvf_name_string, input_arg_types, signature_idx);
  }

  ZETASQL_RETURN_IF_ERROR(FunctionResolver::ReorderArgumentExpressionsPerIndexMapping(
      tvf_name_string, function_signature, arg_index_mapping, ast_tvf,
      input_arg_types, arg_locations, /*resolved_args=*/nullptr,
      resolved_tvf_args));

  return signature_idx;
}

absl::Status Resolver::PrepareTVFInputArguments(
    absl::string_view tvf_name_string, const ASTTVF* ast_tvf,
    const TableValuedFunction* tvf_catalog_entry,
    const NameScope* external_scope, const NameScope* local_scope,
    std::unique_ptr<FunctionSignature>* result_signature,
    std::vector<ResolvedTVFArg>* resolved_tvf_args,
    std::vector<TVFInputArgumentType>* tvf_input_arguments) {
  // `resolved_tvf_args` can be non-empty to provide initial args.
  ZETASQL_RET_CHECK(tvf_input_arguments->empty());

  // <arg_locations> and <resolved_tvf_args> reflect the concrete function
  // call arguments in <result_signature> and match 1:1 to them. For initial
  // arguments, we don't have an argument location, so just use the location
  // of the TVF call.
  std::vector<const ASTNode*> arg_locations(resolved_tvf_args->size(), ast_tvf);
  std::vector<NamedArgumentInfo> named_arguments;

  SignatureMatchResult signature_match_result;
  FunctionResolver function_resolver(catalog_, type_factory_, this);
  ZETASQL_ASSIGN_OR_RETURN(
      const int matching_signature_idx,
      MatchTVFSignature(ast_tvf, tvf_catalog_entry, external_scope, local_scope,
                        function_resolver, result_signature, &arg_locations,
                        resolved_tvf_args, &named_arguments,
                        &signature_match_result));
  ZETASQL_RET_CHECK_EQ(arg_locations.size(), resolved_tvf_args->size());
  const FunctionSignature* function_signature =
      tvf_catalog_entry->GetSignature(matching_signature_idx);
  ZETASQL_RETURN_IF_ERROR(AddAdditionalDeprecationWarningsForCalledFunction(
      ast_tvf, *function_signature, tvf_catalog_entry->FullName(),
      /*is_tvf=*/true));

  ZETASQL_RETURN_IF_ERROR(
      CheckTVFArgumentHasNoCollation(*resolved_tvf_args, arg_locations));
  // Add casts or coerce literals for TVF arguments.
  ZETASQL_RET_CHECK((*result_signature)->IsConcrete()) << ast_tvf->DebugString();

  const auto BadArgErrorPrefix = [&result_signature, &named_arguments,
                                  tvf_catalog_entry](int idx) {
    const FunctionArgumentType& argument =
        (*result_signature)->ConcreteArgument(idx);
    if (argument.has_argument_name()) {
      // Check whether function call was using named argument or positional
      // argument, and if it was named - use the name in the error message.
      for (const auto& named_arg : named_arguments) {
        if (zetasql_base::CaseEqual(named_arg.name().ToString(),
                                   argument.argument_name())) {
          return absl::StrCat("Argument '", argument.argument_name(),
                              "' to table-valued function ",
                              tvf_catalog_entry->FullName());
        }
      }
    }
    if ((*result_signature)->NumConcreteArguments() == 1) {
      return absl::StrCat("The argument to table-valued function ",
                          tvf_catalog_entry->FullName());
    } else {
      return absl::StrCat("Argument ", idx + 1, " to table-valued function ",
                          tvf_catalog_entry->FullName());
    }
  };

  for (int arg_idx = 0; arg_idx < resolved_tvf_args->size(); ++arg_idx) {
    ResolvedTVFArg* resolved_tvf_arg = &(*resolved_tvf_args)[arg_idx];
    if (resolved_tvf_arg->IsExpr()) {
      ZETASQL_RET_CHECK_LT(arg_idx, (*result_signature)->NumConcreteArguments());

      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> expr,
                       resolved_tvf_arg->MoveExpr());
      ZETASQL_RETURN_IF_ERROR(function_resolver.CheckArgumentConstraints(
          ast_tvf, tvf_catalog_entry->FullName(), /*is_tvf=*/true,
          arg_locations[arg_idx], arg_idx,
          (*result_signature)->ConcreteArgument(arg_idx), expr.get(),
          BadArgErrorPrefix));

      const Type* target_type =
          (*result_signature)->ConcreteArgumentType(arg_idx);
      const ASTNode* ast = arg_locations[arg_idx];
      if (ast->node_kind() == AST_NAMED_ARGUMENT) {
        ast = ast->GetAs<ASTNamedArgument>()->expr();
        ZETASQL_RET_CHECK(ast != nullptr);
        // Has validated in `MatchTVFSignature` that named arguments are not
        // named lambdas.
        ZETASQL_RET_CHECK(!ast->Is<ASTLambda>());
      }
      ZETASQL_RETURN_IF_ERROR(
          CoerceExprToType(ast, target_type, kExplicitCoercion, &expr));
      if (expr->node_kind() == RESOLVED_LITERAL) {
        const Value& value = expr->GetAs<ResolvedLiteral>()->value();
        ZETASQL_RETURN_IF_ERROR(function_resolver.CheckArgumentValueConstraints(
            arg_locations[arg_idx], arg_idx, value,
            (*result_signature)->ConcreteArgument(arg_idx), BadArgErrorPrefix));
      }
      resolved_tvf_arg->SetExpr(std::move(expr));
    } else if (resolved_tvf_arg->IsScan()) {
      bool must_add_projection = false;
      ZETASQL_RETURN_IF_ERROR(CheckIfMustCoerceOrRearrangeTVFRelationArgColumns(
          function_signature->argument(arg_idx), arg_idx,
          signature_match_result, *resolved_tvf_arg, &must_add_projection));
      if (must_add_projection) {
        ZETASQL_RETURN_IF_ERROR(CoerceOrRearrangeTVFRelationArgColumns(
            function_signature->argument(arg_idx), arg_idx,
            signature_match_result, ast_tvf, resolved_tvf_arg));
      }
      // We've now ensured the input scan has just the required columns.
      // Mark those columns accessed so they don't get pruned.
      // Use the NameList rather than the column_list to avoid including
      // pseudo-columns.
      ZETASQL_ASSIGN_OR_RETURN(auto name_list, resolved_tvf_arg->GetNameList());
      RecordColumnAccess(name_list->GetResolvedColumns());
    }
  }

  // Prepare the list of TVF input arguments for calling the
  // TableValuedFunction::Resolve method.
  tvf_input_arguments->reserve(resolved_tvf_args->size());
  for (const ResolvedTVFArg& resolved_tvf_arg : *resolved_tvf_args) {
    if (resolved_tvf_arg.IsExpr()) {
      ZETASQL_ASSIGN_OR_RETURN(const ResolvedExpr* const expr,
                       resolved_tvf_arg.GetExpr());
      if (expr->node_kind() == RESOLVED_LITERAL) {
        const Value& value = expr->GetAs<ResolvedLiteral>()->value();
        tvf_input_arguments->push_back(
            TVFInputArgumentType(InputArgumentType(value)));
      } else {
        tvf_input_arguments->push_back(
            TVFInputArgumentType(InputArgumentType(expr->type())));
      }
      tvf_input_arguments->back().set_scalar_expr(expr);
    } else if (resolved_tvf_arg.IsDescriptor()) {
      ZETASQL_ASSIGN_OR_RETURN(const ResolvedDescriptor* const descriptor,
                       resolved_tvf_arg.GetDescriptor());
      tvf_input_arguments->push_back(TVFInputArgumentType(
          TVFDescriptorArgument(descriptor->descriptor_column_name_list())));
    } else if (resolved_tvf_arg.IsConnection()) {
      ZETASQL_ASSIGN_OR_RETURN(const ResolvedConnection* const connection,
                       resolved_tvf_arg.GetConnection());
      tvf_input_arguments->push_back(TVFInputArgumentType(
          TVFConnectionArgument(connection->connection())));
    } else if (resolved_tvf_arg.IsModel()) {
      ZETASQL_ASSIGN_OR_RETURN(const ResolvedModel* const model,
                       resolved_tvf_arg.GetModel());
      tvf_input_arguments->push_back(
          TVFInputArgumentType(TVFModelArgument(model->model())));
    } else {
      ZETASQL_ASSIGN_OR_RETURN(std::shared_ptr<const NameList> name_list,
                       resolved_tvf_arg.GetNameList());
      const std::vector<ResolvedColumn> column_list =
          name_list->GetResolvedColumns();
      if (name_list->is_value_table()) {
        ZETASQL_RET_CHECK_EQ(1, name_list->num_columns()) << ast_tvf->DebugString();
        ZETASQL_RET_CHECK_EQ(1, column_list.size()) << ast_tvf->DebugString();
        tvf_input_arguments->push_back(TVFInputArgumentType(
            TVFRelation::ValueTable(column_list[0].type())));
      } else {
        TVFRelation::ColumnList tvf_relation_columns;
        tvf_relation_columns.reserve(column_list.size());
        ZETASQL_RET_CHECK_GE(column_list.size(), name_list->num_columns())
            << ast_tvf->DebugString();
        for (int j = 0; j < name_list->num_columns(); ++j) {
          tvf_relation_columns.emplace_back(
              name_list->column(j).name().ToString(), column_list[j].type());
        }
        tvf_input_arguments->push_back(
            TVFInputArgumentType(TVFRelation(tvf_relation_columns)));
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Resolver::GenerateTVFNotMatchError(
    const ASTTVF* ast_tvf, const std::vector<const ASTNode*>& arg_locations,
    const SignatureMatchResult& signature_match_result,
    const TableValuedFunction& tvf_catalog_entry, const std::string& tvf_name,
    const std::vector<InputArgumentType>& input_arg_types, int signature_idx) {
  const ASTNode* ast_location = ast_tvf;
  if (signature_match_result.bad_argument_index() != -1) {
    ZETASQL_RET_CHECK_LT(signature_match_result.bad_argument_index(),
                 arg_locations.size());
    ast_location = arg_locations[signature_match_result.bad_argument_index()];
  }
  return MakeSqlErrorAt(ast_location)
         << tvf_catalog_entry.GetTVFSignatureErrorMessage(
                tvf_name, input_arg_types, signature_idx,
                signature_match_result, language(),
                analyzer_options_.show_function_signature_mismatch_details());
}

absl::StatusOr<ResolvedTVFArg> Resolver::ResolveTVFArg(
    const ASTTVFArgument* ast_tvf_arg, const NameScope* external_scope,
    const NameScope* local_scope, const FunctionArgumentType* function_argument,
    const TableValuedFunction* tvf_catalog_entry, int sig_idx,
    absl::flat_hash_map<int, std::unique_ptr<const NameScope>>*
        sig_idx_to_name_scope_map) {
  const ASTExpression* ast_expr = ast_tvf_arg->expr();
  const ASTTableClause* ast_table_clause = ast_tvf_arg->table_clause();
  const ASTModelClause* ast_model_clause = ast_tvf_arg->model_clause();
  const ASTConnectionClause* ast_connection_clause =
      ast_tvf_arg->connection_clause();
  const ASTDescriptor* ast_descriptor = ast_tvf_arg->descriptor();
  ResolvedTVFArg resolved_tvf_arg;
  if (ast_table_clause != nullptr) {
    // Resolve the TVF argument as a relation including all original columns
    // from the named table.
    const Table* table = nullptr;
    if (ast_table_clause->tvf() != nullptr) {
      // The TABLE clause represents a TVF call with arguments. Resolve the
      // TVF inside. Then add an identity projection to match the plan shape
      // expected by engines.
      // When X is a value table, TABLE X produces a scan of it as a value
      // table. (The same is true when we have TABLE tvf(...), and the tvf
      // returns a value table.)
      std::vector<ResolvedTVFArg> tvf_args;
      std::unique_ptr<const ResolvedScan> scan;
      std::shared_ptr<const NameList> name_list;
      ZETASQL_RETURN_IF_ERROR(ResolveTVF(ast_table_clause->tvf(), external_scope,
                                 local_scope, &tvf_args, &scan, &name_list));
      resolved_tvf_arg.SetScan(std::move(scan), name_list);
    } else {
      // If the TVF argument is a TABLE clause, then the table name can be a
      // WITH clause entry, one of the table arguments to the TVF, or a table
      // from the <catalog_>.
      const ASTPathExpression* table_path = ast_table_clause->table_path();
      if (named_subquery_map_.contains(table_path->ToIdStringVector())) {
        std::unique_ptr<const ResolvedScan> scan;
        std::shared_ptr<const NameList> name_list;
        ZETASQL_RETURN_IF_ERROR(ResolveNamedSubqueryRef(table_path, /*hint=*/nullptr,
                                                &scan, &name_list));
        resolved_tvf_arg.SetScan(std::move(scan), name_list);
      } else if (table_path->num_names() == 1 &&
                 function_argument_info_ != nullptr &&
                 function_argument_info_->FindTableArg(
                     table_path->first_name()->GetAsIdString()) != nullptr &&
                 catalog_->FindTable(table_path->ToIdentifierVector(), &table,
                                     analyzer_options_.find_options())
                         .code() == absl::StatusCode::kNotFound) {
        std::unique_ptr<const ResolvedScan> scan;
        std::shared_ptr<const NameList> name_list;
        ZETASQL_RETURN_IF_ERROR(ResolvePathExpressionAsFunctionTableArgument(
            table_path, /*hint=*/nullptr, GetAliasForExpression(table_path),
            /*ast_location=*/ast_table_clause, &scan, &name_list));
        resolved_tvf_arg.SetScan(std::move(scan), std::move(name_list));
      } else {
        ZETASQL_RET_CHECK(ast_expr == nullptr);
        std::unique_ptr<const ResolvedTableScan> table_scan;
        std::shared_ptr<const NameList> name_list;
        ZETASQL_RETURN_IF_ERROR(ResolvePathExpressionAsTableScan(
            table_path, GetAliasForExpression(table_path),
            /*has_explicit_alias=*/false, /*alias_location=*/ast_table_clause,
            /*hints=*/nullptr, /*for_system_time=*/nullptr, external_scope,
            /*remaining_names=*/nullptr, &table_scan, &name_list,
            resolved_columns_from_table_scans_));
        resolved_tvf_arg.SetScan(std::move(table_scan), std::move(name_list));
      }
    }
    ZETASQL_ASSIGN_OR_RETURN(std::shared_ptr<const NameList> name_list,
                     resolved_tvf_arg.GetNameList());
    RecordColumnAccess(name_list->GetResolvedColumns());
  } else if (ast_expr != nullptr) {
    if (ast_expr->node_kind() == AST_NAMED_ARGUMENT) {
      if (IsNamedLambda(ast_expr)) {
        return MakeSqlErrorAt(ast_expr)
               << "Lambda arguments are not implemented for TVF";
      }
      // Set 'ast_expr' to the named argument value for further resolving.
      ast_expr = ast_expr->GetAs<ASTNamedArgument>()->expr();
    }
    if (function_argument &&
        (function_argument->IsRelation() || function_argument->IsModel() ||
         function_argument->IsConnection() ||
         function_argument->IsDescriptor())) {
      auto gen_arg_id = [function_argument](int sig_idx) {
        std::string arg_id = absl::StrCat(sig_idx + 1);
        if (function_argument->has_argument_name()) {
          absl::StrAppend(&arg_id, " ('", function_argument->argument_name(),
                          "')");
        }
        return arg_id;
      };
      if (function_argument->IsRelation()) {
        // Resolve the TVF argument as a relation. The argument should be
        // written in the TVF call as a table subquery. We parsed all
        // arguments as expressions, so the parse node should initially have
        // scalar subquery type. We check that the expression subquery
        // modifier type is NONE to exclude ARRAY and EXISTS subqueries.
        if (ast_expr->node_kind() != AST_EXPRESSION_SUBQUERY ||
            ast_expr->GetAsOrDie<ASTExpressionSubquery>()->modifier() !=
                ASTExpressionSubquery::NONE) {
          std::string error = absl::StrCat(
              "Table-valued function ", tvf_catalog_entry->FullName(),
              " argument ", gen_arg_id(sig_idx),
              " must be a relation (i.e. table subquery)");
          if (ast_expr->node_kind() == AST_PATH_EXPRESSION) {
            const std::string table_name =
                ast_expr->GetAsOrDie<ASTPathExpression>()
                    ->ToIdentifierPathString();
            // Return a specific error message helping the user figure out
            // what to change.
            absl::StrAppend(&error, "; if you meant to refer to table ",
                            table_name, " then add the TABLE keyword ",
                            "before the table name (i.e. TABLE ", table_name,
                            ")");
          }
          return MakeSqlErrorAt(ast_expr) << error;
        }
        std::unique_ptr<const ResolvedScan> scan;
        std::shared_ptr<const NameList> name_list;
        ZETASQL_RETURN_IF_ERROR(
            ResolveQuery(ast_expr->GetAsOrDie<ASTExpressionSubquery>()->query(),
                         external_scope, AllocateSubqueryName(),
                         /*is_outer_query=*/false, &scan, &name_list));

        // The <sig_idx_to_name_scope_map> is not nullptr means descriptors
        // appear in TVF thus there is a need to build NameScopes for the table
        // arguments.
        if (sig_idx_to_name_scope_map != nullptr) {
          sig_idx_to_name_scope_map->emplace(
              sig_idx, std::make_unique<NameScope>(external_scope, name_list));
        }
        resolved_tvf_arg.SetScan(std::move(scan), std::move(name_list));
      } else if (function_argument->IsConnection()) {
        return MakeSqlErrorAt(ast_expr)
               << "Table-valued function " << tvf_catalog_entry->FullName()
               << " argument " << gen_arg_id(sig_idx)
               << " must be a connection specified with the CONNECTION keyword";
      } else if (function_argument->IsDescriptor()) {
        return MakeSqlErrorAt(ast_expr)
               << "Table-valued function " << tvf_catalog_entry->FullName()
               << " argument " << gen_arg_id(sig_idx)
               << " must be a DESCRIPTOR";
      } else {
        ZETASQL_RET_CHECK(function_argument->IsModel());
        return MakeSqlErrorAt(ast_expr)
               << "Table-valued function " << tvf_catalog_entry->FullName()
               << " argument " << gen_arg_id(sig_idx)
               << " must be a model specified with the MODEL keyword";
      }
    } else {
      // Resolve the TVF argument as a scalar expression.
      std::unique_ptr<const ResolvedExpr> expr;
      ZETASQL_RETURN_IF_ERROR(
          ResolveScalarExpr(ast_expr, external_scope, "FROM clause", &expr));
      resolved_tvf_arg.SetExpr(std::move(expr));
    }
  } else if (ast_connection_clause != nullptr) {
    std::unique_ptr<const ResolvedConnection> resolved_connection;
    ZETASQL_RETURN_IF_ERROR(ResolveConnection(ast_connection_clause->connection_path(),
                                      &resolved_connection));
    resolved_tvf_arg.SetConnection(std::move(resolved_connection));
  } else if (ast_model_clause != nullptr) {
    std::unique_ptr<const ResolvedModel> resolved_model;
    ZETASQL_RETURN_IF_ERROR(
        ResolveModel(ast_model_clause->model_path(), &resolved_model));
    resolved_tvf_arg.SetModel(std::move(resolved_model));
  } else {
    ZETASQL_RET_CHECK(ast_descriptor != nullptr);
    std::unique_ptr<const ResolvedDescriptor> resolved_descriptor;
    const ASTDescriptorColumnList* column_list = ast_descriptor->columns();
    ZETASQL_RETURN_IF_ERROR(
        ResolveDescriptorFirstPass(column_list, &resolved_descriptor));
    resolved_tvf_arg.SetDescriptor(std::move(resolved_descriptor));
  }

  return resolved_tvf_arg;
}

absl::StatusOr<InputArgumentType> Resolver::GetTVFArgType(
    const ResolvedTVFArg& resolved_tvf_arg) {
  InputArgumentType input_arg_type;
  if (resolved_tvf_arg.IsExpr()) {
    ZETASQL_ASSIGN_OR_RETURN(const ResolvedExpr* const expr,
                     resolved_tvf_arg.GetExpr());
    // We should not force the type for a pure NULL expr, as the output
    // InputArgumentType is used for function signature matching and the final
    // argument type is not determined yet.
    input_arg_type = GetInputArgumentTypeForExpr(
        expr, /*pick_default_type_for_untyped_expr=*/false);
  } else if (resolved_tvf_arg.IsScan()) {
    ZETASQL_ASSIGN_OR_RETURN(std::shared_ptr<const NameList> name_list,
                     resolved_tvf_arg.GetNameList());
    if (name_list->is_value_table()) {
      input_arg_type = InputArgumentType::RelationInputArgumentType(
          TVFRelation::ValueTable(name_list->column(0).column().type()));
    } else {
      TVFRelation::ColumnList provided_input_relation_columns;
      provided_input_relation_columns.reserve(name_list->num_columns());
      // Loop over each explicit column returned from the relation argument.
      // Use the number of names in the relation argument's name list instead
      // of the scan's column_list().size() here since the latter includes
      // pseudo-columns, which we do not want to consider here.
      for (int j = 0; j < name_list->num_columns(); ++j) {
        const NamedColumn& named_column = name_list->column(j);
        provided_input_relation_columns.emplace_back(
            named_column.name().ToString(), named_column.column().type());
      }
      input_arg_type = InputArgumentType::RelationInputArgumentType(
          TVFRelation(provided_input_relation_columns));
    }
  } else if (resolved_tvf_arg.IsConnection()) {
    ZETASQL_ASSIGN_OR_RETURN(const ResolvedConnection* const connection,
                     resolved_tvf_arg.GetConnection());
    input_arg_type = InputArgumentType::ConnectionInputArgumentType(
        TVFConnectionArgument(connection->connection()));
  } else if (resolved_tvf_arg.IsModel()) {
    // We are processing a model argument.
    ZETASQL_ASSIGN_OR_RETURN(const ResolvedModel* const model,
                     resolved_tvf_arg.GetModel());
    input_arg_type = InputArgumentType::ModelInputArgumentType(
        TVFModelArgument(model->model()));
  } else {
    ZETASQL_RET_CHECK(resolved_tvf_arg.IsDescriptor());
    // We are processing a descriptor argument.
    input_arg_type = InputArgumentType::DescriptorInputArgumentType();
  }
  return input_arg_type;
}

absl::Status Resolver::CheckIfMustCoerceOrRearrangeTVFRelationArgColumns(
    const FunctionArgumentType& tvf_signature_arg, int arg_idx,
    const SignatureMatchResult& signature_match_result,
    const ResolvedTVFArg& resolved_tvf_arg, bool* add_projection) {
  // If the function signature did not include a required schema for this
  // particular relation argument, there is no need to add a projection.
  if (!tvf_signature_arg.options().has_relation_input_schema()) {
    *add_projection = false;
    return absl::OkStatus();
  }

  // Add a projection to add or drop columns as needed if the number of provided
  // columns was not equal to the number of required columns.
  const TVFRelation& required_schema =
      tvf_signature_arg.options().relation_input_schema();
  ZETASQL_ASSIGN_OR_RETURN(std::shared_ptr<const NameList> name_list,
                   resolved_tvf_arg.GetNameList());
  const int num_provided_columns = name_list->num_columns();
  if (required_schema.num_columns() != num_provided_columns) {
    *add_projection = true;
    return absl::OkStatus();
  }

  // Add a projection if the function signature-matching process indicates a
  // need to coerce the type of one of the function arguments.
  for (const auto& [key, to_type] :
       signature_match_result.tvf_relation_coercion_map()) {
    if (arg_idx == key.argument_index) {
      *add_projection = true;
      return absl::OkStatus();
    }
  }

  // If the required schema was a value table and no type coercion was
  // necessary, then there is no need to add a projection.
  if (required_schema.is_value_table()) {
    *add_projection = false;
    return absl::OkStatus();
  }

  // If the order of provided columns was not equal to the order of required
  // columns, add a projection to rearrange provided columns as needed.
  ZETASQL_RET_CHECK_EQ(required_schema.num_columns(), name_list->columns().size());
  for (int i = 0; i < num_provided_columns; ++i) {
    if (!zetasql_base::CaseEqual(required_schema.column(i).name,
                                name_list->column(i).name().ToString())) {
      *add_projection = true;
      return absl::OkStatus();
    }
  }

  *add_projection = false;
  return absl::OkStatus();
}

absl::Status Resolver::CoerceOrRearrangeTVFRelationArgColumns(
    const FunctionArgumentType& tvf_signature_arg, int arg_idx,
    const SignatureMatchResult& signature_match_result,
    const ASTNode* ast_location, ResolvedTVFArg* resolved_tvf_arg) {
  // Generate a name for a new projection to perform the TVF relation argument
  // coercion and create vectors to hold new columns for the projection.
  const IdString new_project_alias = AllocateSubqueryName();
  std::vector<ResolvedColumn> new_column_list;
  std::vector<std::unique_ptr<const ResolvedComputedColumn>>
      new_project_columns;
  std::unique_ptr<NameList> new_project_name_list(new NameList);

  // Reserve vectors.
  // Use the number of names in the relation argument's name list instead of the
  // scan's column_list().size() here since the latter includes pseudo-columns,
  // which we do not want to consider here.
  ZETASQL_ASSIGN_OR_RETURN(std::shared_ptr<const NameList> name_list,
                   resolved_tvf_arg->GetNameList());
  const int num_provided_columns = name_list->num_columns();
  new_column_list.reserve(num_provided_columns);
  new_project_columns.reserve(num_provided_columns);
  new_project_name_list->ReserveColumns(num_provided_columns);

  // Build a map from provided column name to the index of that column in the
  // list of provided columns.
  std::map<std::string, int, zetasql_base::CaseLess> col_name_to_idx;
  for (int col_idx = 0; col_idx < num_provided_columns; ++col_idx) {
    col_name_to_idx.emplace(name_list->column(col_idx).name().ToString(),
                            col_idx);
  }

  // Iterate through each required column of the relation from the function
  // signature and find the matching provided column with the same name. Also
  // create a cast expression if any provided column type is not equivalent to
  // the corresponding required input column type.
  const int num_required_columns = static_cast<int>(
      tvf_signature_arg.options().relation_input_schema().columns().size());
  for (int required_col_idx = 0; required_col_idx < num_required_columns;
       ++required_col_idx) {
    const std::string required_col_name = tvf_signature_arg.options()
                                              .relation_input_schema()
                                              .column(required_col_idx)
                                              .name;
    int provided_col_idx;
    if (tvf_signature_arg.options().relation_input_schema().is_value_table()) {
      provided_col_idx = 0;
    } else {
      const int* lookup = zetasql_base::FindOrNull(col_name_to_idx, required_col_name);
      ZETASQL_RET_CHECK(lookup != nullptr) << required_col_name;
      provided_col_idx = *lookup;
    }
    const Type* result_type =
        zetasql_base::FindPtrOrNull(signature_match_result.tvf_relation_coercion_map(),
                           {arg_idx, provided_col_idx});
    const ResolvedColumn& provided_input_column =
        name_list->column(provided_col_idx).column();
    if (result_type == nullptr) {
      new_column_list.push_back(provided_input_column);
    } else {
      new_column_list.emplace_back(AllocateColumnId(), new_project_alias,
                                   name_list->column(provided_col_idx).name(),
                                   result_type);
      std::unique_ptr<const ResolvedExpr> resolved_cast(
          MakeColumnRef(provided_input_column, /*is_correlated=*/false));
      ZETASQL_RETURN_IF_ERROR(ResolveCastWithResolvedArgument(
          ast_location, result_type, /*return_null_on_error=*/false,
          &resolved_cast));
      new_project_columns.push_back(MakeResolvedComputedColumn(
          new_column_list.back(), std::move(resolved_cast)));
    }
    // Add the new referenced column to the set of referenced columns so that it
    // is not pruned away later.
    RecordColumnAccess(new_column_list.back());
    ZETASQL_RETURN_IF_ERROR(new_project_name_list->AddColumnMaybeValueTable(
        name_list->column(provided_col_idx).name(), new_column_list.back(),
        /*is_explicit=*/true, ast_location,
        tvf_signature_arg.options().relation_input_schema().is_value_table()));
  }
  if (tvf_signature_arg.options().relation_input_schema().is_value_table()) {
    ZETASQL_RETURN_IF_ERROR(new_project_name_list->SetIsValueTable());
  }

  // Reset the scan and name list to the new projection.
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedScan> scan,
                   resolved_tvf_arg->MoveScan());
  resolved_tvf_arg->SetScan(
      MakeResolvedProjectScan(new_column_list, std::move(new_project_columns),
                              std::move(scan)),
      std::move(new_project_name_list));
  return absl::OkStatus();
}

absl::Status Resolver::ValidateUnnestAliases(
    const ASTTablePathExpression* table_ref) {
  ZETASQL_RET_CHECK_NE(table_ref, nullptr);
  const ASTUnnestExpression* unnest_expr = table_ref->unnest_expr();
  ZETASQL_RET_CHECK_NE(unnest_expr, nullptr);
  ZETASQL_RET_CHECK_GE(unnest_expr->expressions().size(), 1);
  const ASTExpressionWithOptAlias* first_arg = unnest_expr->expressions(0);

  if (!language().LanguageFeatureEnabled(FEATURE_V_1_4_MULTIWAY_UNNEST)) {
    if (unnest_expr->expressions().size() > 1) {
      return MakeSqlErrorAt(unnest_expr->expressions(1))
             << "The UNNEST operator supports exactly one argument";
    }
    if (first_arg->optional_alias() != nullptr) {
      return MakeSqlErrorAt(first_arg->optional_alias())
             << "Argument alias is not supported in the UNNEST operator";
    }
  }

  if (table_ref->alias() != nullptr) {
    // The unnest expression has legacy alias. No argument aliases are allowed
    // and there should be exactly one expression in UNNEST.
    if (unnest_expr->expressions().size() != 1) {
      return MakeSqlErrorAt(table_ref->alias())
             << "When 2 or more array arguments are supplied to UNNEST, "
                "aliases for the element columns must be specified following "
                "the argument inside the parenthesis";
    }
    if (first_arg->optional_alias() != nullptr) {
      return MakeSqlErrorAt(table_ref->alias())
             << "Alias outside UNNEST is not allowed when the argument inside "
                "the parenthesis has alias";
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<const ResolvedExpr>>
Resolver::ResolveArrayZipMode(const ASTUnnestExpression* unnest,
                              ExprResolutionInfo* info) {
  const EnumType* array_zip_mode_type = types::ArrayZipModeEnumType();
  const ASTNamedArgument* array_zip_mode = unnest->array_zip_mode();
  std::unique_ptr<const ResolvedExpr> resolved_mode;
  if (array_zip_mode == nullptr) {
    // Ok to not specify an array zip mode. Supply a default 'PAD' mode if it's
    // multiway UNNEST syntax.
    if (unnest->expressions().size() > 1) {
      // We should have already validated in `ValidateUnnestAliases` that when
      // unnest contains more than 1 argument, the language feature is on.
      ZETASQL_RET_CHECK(
          language().LanguageFeatureEnabled(FEATURE_V_1_4_MULTIWAY_UNNEST));
      return MakeResolvedLiteralWithoutLocation(
          Value::Enum(array_zip_mode_type, functions::ArrayZipEnums::PAD));
    }
    return resolved_mode;
  }
  if (!language().LanguageFeatureEnabled(FEATURE_V_1_4_MULTIWAY_UNNEST)) {
    return MakeSqlErrorAt(array_zip_mode) << "Argument `mode` is not supported";
  }

  static constexpr absl::string_view kArrayZipModeArgName = "mode";
  if (!zetasql_base::CaseEqual(array_zip_mode->name()->GetAsStringView(),
                              kArrayZipModeArgName)) {
    return MakeSqlErrorAt(array_zip_mode)
           << "Unsupported named argument `"
           << array_zip_mode->name()->GetAsStringView()
           << "` in UNNEST; use `mode` instead";
  }

  if (unnest->expressions().size() == 1) {
    return MakeSqlErrorAt(array_zip_mode)
           << "Argument `mode` is not allowed when UNNEST only has one array "
              "argument";
  }
  if (IsNamedLambda(array_zip_mode)) {
    return MakeSqlErrorAt(array_zip_mode) << "Argument `mode` cannot be lambda";
  }

  ZETASQL_RETURN_IF_ERROR(ResolveExpr(array_zip_mode->expr(), info, &resolved_mode));

  // See if we need to coerce the type.
  if (resolved_mode->type()->Equals(array_zip_mode_type)) {
    return resolved_mode;
  }
  auto make_error_msg = [&](absl::string_view target_type_name,
                            absl::string_view actual_type_name) {
    return absl::Substitute(
        "Named argument `mode` used in UNNEST should have type $0, but got "
        "type $1",
        target_type_name, actual_type_name);
  };
  AnnotatedType annotated_target_type = {array_zip_mode_type,
                                         /*annotation_map=*/nullptr};
  ZETASQL_RETURN_IF_ERROR(CoerceExprToType(array_zip_mode->expr(),
                                   annotated_target_type, kImplicitCoercion,
                                   make_error_msg, &resolved_mode))
      .With(LocationOverride(array_zip_mode->expr()));
  return resolved_mode;
}

// Returns a more precise parsed location for UNNEST argument alias when the
// node is a path expression.
static const ASTNode* GetInferredAliasLocation(
    const ASTExpressionWithOptAlias* argument) {
  if (!argument->expression()->Is<ASTPathExpression>()) {
    return argument;
  }
  return argument->expression()->GetAsOrDie<ASTPathExpression>()->last_name();
}

// The alias could be returned empty. It will be allocated post-traversal of
// the resolved expr.
Resolver::UnnestArrayColumnAlias Resolver::GetArrayElementColumnAlias(
    const ASTExpressionWithOptAlias* argument) {
  if (argument->optional_alias() != nullptr) {
    return {/*alias=*/argument->optional_alias()->GetAsIdString(),
            /*alias_location=*/argument->optional_alias()};
  }
  return {/*alias=*/GetAliasForExpression(argument->expression()),
          /*alias_location=*/GetInferredAliasLocation(argument)};
}

absl::Status Resolver::ResolveArrayArgumentForExplicitUnnest(
    const ASTExpressionWithOptAlias* argument,
    UnnestArrayColumnAlias& arg_alias, ExprResolutionInfo* info,
    std::vector<UnnestArrayColumnAlias>& output_alias_list,
    ResolvedColumnList& output_column_list,
    std::shared_ptr<NameList>& output_name_list,
    std::vector<std::unique_ptr<const ResolvedExpr>>& resolved_array_expr_list,
    std::vector<ResolvedColumn>& resolved_element_column_list) {
  std::unique_ptr<const ResolvedExpr> resolved_value_expr;
  const absl::Status resolve_expr_status =
      ResolveExpr(argument->expression(), info, &resolved_value_expr);

  // If resolving the expression failed, and it looked like a valid table
  // name, then give a more helpful error message.
  // TODO: b/315169608 - Find a better way to detect the desired error
  if (resolve_expr_status.code() == absl::StatusCode::kInvalidArgument &&
      absl::StartsWith(resolve_expr_status.message(), "Unrecognized name: ") &&
      argument->expression()->node_kind() == AST_PATH_EXPRESSION) {
    const ASTPathExpression* path_expr =
        argument->expression()->GetAsOrDie<ASTPathExpression>();
    const Table* table = nullptr;
    int num_names_consumed = 0;
    const absl::Status find_status = catalog_->FindTableWithPathPrefix(
        path_expr->ToIdentifierVector(), analyzer_options_.find_options(),
        &num_names_consumed, &table);

    if (find_status.ok()) {
      if (table != nullptr && num_names_consumed < path_expr->num_names()) {
        return MakeSqlErrorAt(path_expr)
               << "UNNEST cannot be applied on path expression with "
                  "non-correlated table name prefix "
               << table->FullName();
      }
      return MakeSqlErrorAt(path_expr)
             << "UNNEST cannot be applied on a table: "
             << path_expr->ToIdentifierPathString();
    }
    if (find_status.code() != absl::StatusCode::kNotFound) {
      ZETASQL_RETURN_IF_ERROR(find_status);
    }
  }
  ZETASQL_RETURN_IF_ERROR(resolve_expr_status);  // Return original error.
  ZETASQL_RET_CHECK(resolved_value_expr != nullptr);
  const Type* value_type = resolved_value_expr->type();
  ZETASQL_RET_CHECK(value_type != nullptr);
  if (!value_type->IsArray()) {
    return MakeSqlErrorAt(argument->expression())
           << "Values referenced in UNNEST must be arrays. "
           << "UNNEST contains expression of type "
           << value_type->ShortTypeName(product_mode());
  }

  // Compute alias if it's not provided in the user query or not inferrable from
  // the original path expression.
  if (arg_alias.alias.empty()) {
    arg_alias.alias = AllocateUnnestName();
  }

  const AnnotationMap* element_annotation = nullptr;
  if (resolved_value_expr->type_annotation_map() != nullptr) {
    element_annotation =
        resolved_value_expr->type_annotation_map()->AsArrayMap()->element();
  }
  const ResolvedColumn array_element_column(
      AllocateColumnId(), /*table_name=*/kArrayId, /*name=*/arg_alias.alias,
      AnnotatedType(value_type->AsArray()->element_type(), element_annotation));
  output_alias_list.emplace_back(arg_alias);
  output_column_list.emplace_back(array_element_column);
  resolved_array_expr_list.emplace_back(std::move(resolved_value_expr));
  resolved_element_column_list.emplace_back(array_element_column);

  absl::Status name_list_update_status = output_name_list->AddValueTableColumn(
      arg_alias.alias, array_element_column, arg_alias.alias_location);
  if (name_list_update_status.code() == absl::StatusCode::kInvalidArgument &&
      absl::StartsWith(name_list_update_status.message(), "Duplicate alias")) {
    return MakeSqlErrorAt(arg_alias.alias_location)
           << "Duplicate value table name `" << arg_alias.alias
           << "` found in UNNEST is not allowed";
  }
  return name_list_update_status;
}

absl::Status Resolver::ResolveUnnest(
    const ASTTablePathExpression* table_ref, ExprResolutionInfo* info,
    std::vector<UnnestArrayColumnAlias>& output_alias_list,
    ResolvedColumnList& output_column_list,
    std::shared_ptr<NameList>& output_name_list,
    std::vector<std::unique_ptr<const ResolvedExpr>>& resolved_array_expr_list,
    std::vector<ResolvedColumn>& resolved_element_column_list) {
  const ASTUnnestExpression* unnest = table_ref->unnest_expr();
  ZETASQL_RET_CHECK(unnest != nullptr);

  if (unnest->expressions().size() > 1) {
    // Mark multiway UNNEST rewriter.
    analyzer_output_properties_.MarkRelevant(REWRITE_MULTIWAY_UNNEST);
    for (const ASTExpressionWithOptAlias* argument : unnest->expressions()) {
      UnnestArrayColumnAlias arg_alias = GetArrayElementColumnAlias(argument);
      ZETASQL_RETURN_IF_ERROR(ResolveArrayArgumentForExplicitUnnest(
          argument, arg_alias, info, output_alias_list, output_column_list,
          output_name_list, resolved_array_expr_list,
          resolved_element_column_list));
    }
    return absl::OkStatus();
  }

  ZETASQL_RET_CHECK(unnest->expressions().size() == 1);
  UnnestArrayColumnAlias arg_alias;
  // For the singleton UNNEST case, we respect alias outside the explicit UNNEST
  // parenthesis for backward compatibility.
  if (table_ref->alias() != nullptr) {
    arg_alias.alias = table_ref->alias()->GetAsIdString();
    arg_alias.alias_location = table_ref->alias();
  } else {
    // Point alias location to UNNEST by default for backward compatibility.
    arg_alias.alias_location = table_ref;
    if (language().LanguageFeatureEnabled(
            FEATURE_V_1_4_SINGLETON_UNNEST_INFERS_ALIAS)) {
      arg_alias.alias =
          GetAliasForExpression(unnest->expressions(0)->expression());
      arg_alias.alias_location =
          GetInferredAliasLocation(unnest->expressions(0));
    }
    if (language().LanguageFeatureEnabled(FEATURE_V_1_4_MULTIWAY_UNNEST) &&
        unnest->expressions(0)->optional_alias() != nullptr) {
      arg_alias.alias =
          unnest->expressions(0)->optional_alias()->GetAsIdString();
      arg_alias.alias_location = unnest->expressions(0)->optional_alias();
    }
  }

  return ResolveArrayArgumentForExplicitUnnest(
      unnest->expressions(0), arg_alias, info, output_alias_list,
      output_column_list, output_name_list, resolved_array_expr_list,
      resolved_element_column_list);
}

absl::Status Resolver::ResolveArrayScan(
    const ASTTablePathExpression* table_ref,
    std::optional<PathExpressionSpan> path_expr, const ASTOnClause* on_clause,
    const ASTUsingClause* using_clause, const ASTJoin* ast_join,
    bool is_outer_scan, bool include_lhs_name_list,
    bool is_single_table_array_path,
    std::unique_ptr<const ResolvedScan>* resolved_input_scan,
    const std::shared_ptr<const NameList>& name_list_input,
    const NameScope* scope, std::unique_ptr<const ResolvedScan>* output,
    std::shared_ptr<const NameList>* output_name_list) {
  RETURN_ERROR_IF_OUT_OF_STACK_SPACE();

  // There might not be a lhs_scan, but the unique_ptr should be non-NULL.
  ZETASQL_RET_CHECK(resolved_input_scan != nullptr);
  ZETASQL_RET_CHECK_EQ(*resolved_input_scan == nullptr, name_list_input == nullptr);

  if (table_ref->sample_clause() != nullptr) {
    return MakeSqlErrorAt(table_ref)
           << "TABLESAMPLE cannot be used with arrays";
  }

  // We have either an array reference or UNNEST.
  // These variables get set in either branch below.
  std::unique_ptr<const ResolvedExpr> resolved_value_expr;
  const Type* value_type = nullptr;
  // `mode` argument will only be set and used in explicit UNNEST syntax.
  std::unique_ptr<const ResolvedExpr> resolved_zip_mode;
  ResolvedColumnList output_column_list;
  if (*resolved_input_scan != nullptr && include_lhs_name_list) {
    output_column_list = (*resolved_input_scan)->column_list();
  }

  // Build a name list for correlated names.
  // TODO: b/315045184 - clean up shared_ptr usage.
  std::shared_ptr<NameList> name_list_lhs(new NameList);
  if (name_list_input != nullptr) {
    ZETASQL_RETURN_IF_ERROR(name_list_lhs->MergeFrom(*name_list_input, table_ref));
  }

  // Array aliases are always treated as explicit range variables,
  // even if computed.
  // This allows
  //   SELECT t.key, array1, array2
  //   FROM Table t, t.array1, array1.array2;
  // `array1` and `array2` are also available implicitly as columns on the
  // preceding scan, but the array scan makes them implicit.
  std::shared_ptr<NameList> name_list(new NameList);
  // `name_list_rhs` is only used if USING clause is present.
  std::shared_ptr<NameList> name_list_rhs(new NameList);

  std::vector<std::unique_ptr<const ResolvedExpr>> resolved_array_expr_list;
  std::vector<ResolvedColumn> resolved_element_column_list;
  std::vector<UnnestArrayColumnAlias> output_alias_list;

  if (table_ref->path_expr() != nullptr) {
    ZETASQL_RET_CHECK(path_expr.has_value());

    // Single-word identifiers are always resolved as table names
    // and shouldn't have made it into ResolveArrayScan.
    ZETASQL_RET_CHECK_GE(path_expr->num_names(), 2);

    // Path expression in the FROM clause only resolves against non-aggregate
    // and non-analytic scope, so no aggregate function or window function is
    // allowed here.
    ExprResolutionInfo no_aggregation(scope, "FROM clause");
    FlattenState::Restorer restorer;
    if (language().LanguageFeatureEnabled(
            FEATURE_V_1_3_UNNEST_AND_FLATTEN_ARRAYS)) {
      no_aggregation.flatten_state.set_can_flatten(true, &restorer);
    }

    NameTarget first_target;
    ZETASQL_RET_CHECK(scope->LookupName(path_expr->GetFirstIdString(), &first_target));

    switch (first_target.kind()) {
      case NameTarget::EXPLICIT_COLUMN:
      case NameTarget::RANGE_VARIABLE: {
        // These are the allowed cases.
        break;
      }
      case NameTarget::IMPLICIT_COLUMN: {
        // We disallowed this because the results were very confusing.
        //   FROM TableName, ColumnName.array_value
        // is not allowed.
        // Explicit column names are allowed in order to support chained
        // references like
        //   FROM Table t, t.Column.arr1 a1, a1.arr2 a2
        //
        // If we decide to make this allowed in normal mode, we would still
        // need to give an error if in_strict_mode().
        return MakeSqlErrorAtPoint(path_expr->GetParseLocationRange().start())
               << "Aliases referenced in the from clause must refer to "
                  "preceding scans, and cannot refer to columns on those "
                  "scans. "
               << path_expr->GetFirstIdString()
               << " refers to a column and must be qualified with a table "
                  "name.";
      }
      case NameTarget::FIELD_OF: {
        return MakeSqlErrorAtPoint(path_expr->GetParseLocationRange().start())
               << "Aliases referenced in the from clause must refer to "
                  "preceding scans, and cannot refer to columns or fields on "
                  "those scans. "
               << path_expr->GetFirstIdString()
               << " refers to a field and must be qualified with a table name.";
      }
      case NameTarget::ACCESS_ERROR: {
        PathExpressionSpan path_expr_span(path_expr.value());
        ZETASQL_RETURN_IF_ERROR(ResolvePathExpressionAsExpression(
            path_expr_span, &no_aggregation, ResolvedStatement::READ,
            &resolved_value_expr));
        ZETASQL_RET_CHECK(path_expr_span.num_names() > 1);
        // This is the allowed case when the whole path can be resolved and
        // matched to a post-group by column, despite that the first name is
        // ACCESS_ERROR. For example:
        //
        // select tt.KitchenSink.repeated_int32_val,
        //        (select sum(e)
        //         from tt.KitchenSink.repeated_int32_val),  -- <- this ref
        // from TestTable tt
        // group by 1
        //
        // In this query, the reference to tt.KitchenSink.Repeated_int32_val
        // in the subquery is okay because tt.KitchenSink.Repeated_int32_val
        // is a group by key and thus a post-group by column. Any other
        // repeated field in KitchenSink would trigger the error above
        // because KitchenSink is not generally visible post GROUP BY.
        break;
      }
      case NameTarget::AMBIGUOUS: {
        // This can happen if the array name is ambiguous (resolves to a name
        // in more than one table previously in the FROM clause).
        return MakeSqlErrorAtPoint(path_expr->GetParseLocationRange().start())
               << path_expr->GetFirstIdString()
               << " ambiguously references multiple columns in previous FROM"
               << " clause tables";
      }
    }

    // Now we know we have an identifier path starting with a scan.
    // Resolve the remaining path to expand proto field accesses.
    PathExpressionSpan path_expr_span(path_expr.value());
    ZETASQL_RETURN_IF_ERROR(ResolvePathExpressionAsExpression(
        path_expr_span, &no_aggregation, ResolvedStatement::READ,
        &resolved_value_expr));

    ZETASQL_RET_CHECK(resolved_value_expr != nullptr);
    value_type = resolved_value_expr->type();
    ZETASQL_RET_CHECK(value_type != nullptr);
    if (!value_type->IsArray()) {
      return MakeSqlErrorAtPoint(path_expr->GetParseLocationRange().start())
             << "Values referenced in FROM clause must be arrays. "
             << path_expr->ToIdentifierPathString() << " has type "
             << value_type->ShortTypeName(product_mode());
    }

    IdString alias;
    const ASTNode* alias_location;
    if (table_ref->alias() != nullptr) {
      alias = table_ref->alias()->GetAsIdString();
      alias_location = table_ref->alias();
    } else {
      alias = GetAliasForExpression(table_ref->path_expr());
      alias_location = table_ref;
    }
    ZETASQL_RET_CHECK(!alias.empty());
    output_alias_list.emplace_back(
        UnnestArrayColumnAlias{alias, alias_location});

    const AnnotationMap* element_annotation = nullptr;
    if (resolved_value_expr->type_annotation_map() != nullptr) {
      element_annotation =
          resolved_value_expr->type_annotation_map()->AsArrayMap()->element();
    }
    const ResolvedColumn array_element_column(
        AllocateColumnId(), /*table_name=*/kArrayId, /*name=*/alias,
        AnnotatedType(value_type->AsArray()->element_type(),
                      element_annotation));

    ZETASQL_RETURN_IF_ERROR(name_list_rhs->AddValueTableColumn(
        alias, array_element_column, alias_location));
    output_column_list.emplace_back(array_element_column);
    resolved_array_expr_list.push_back(std::move(resolved_value_expr));
    resolved_element_column_list.push_back(array_element_column);
  } else {
    ZETASQL_RET_CHECK(table_ref->unnest_expr() != nullptr);
    ZETASQL_RETURN_IF_ERROR(ValidateUnnestAliases(table_ref));

    ExprResolutionInfo info(scope, "UNNEST");
    FlattenState::Restorer restorer;
    if (language().LanguageFeatureEnabled(
            FEATURE_V_1_3_UNNEST_AND_FLATTEN_ARRAYS)) {
      info.flatten_state.set_can_flatten(true, &restorer);
    }

    const ASTUnnestExpression* unnest = table_ref->unnest_expr();
    ZETASQL_RETURN_IF_ERROR(ResolveUnnest(
        table_ref, &info, output_alias_list, output_column_list, name_list_rhs,
        resolved_array_expr_list, resolved_element_column_list));
    ZETASQL_RET_CHECK_EQ(output_alias_list.size(), resolved_element_column_list.size());
    ZETASQL_RET_CHECK_EQ(resolved_array_expr_list.size(),
                 resolved_element_column_list.size());
    ZETASQL_ASSIGN_OR_RETURN(resolved_zip_mode, ResolveArrayZipMode(unnest, &info));
  }

  // Resolve WITH OFFSET if present.
  std::unique_ptr<ResolvedColumnHolder> array_position_column;
  if (table_ref->with_offset() != nullptr) {
    // If the alias is NULL, we get "offset" as an implicit alias.
    const ASTAlias* with_offset_alias = table_ref->with_offset()->alias();
    const IdString offset_alias =
        (with_offset_alias == nullptr ? kOffsetAlias
                                      : with_offset_alias->GetAsIdString());

    const ResolvedColumn column(AllocateColumnId(),
                                /*table_name=*/kArrayOffsetId,
                                /*name=*/offset_alias,
                                AnnotatedType(type_factory_->get_int64(),
                                              /*annotation_map=*/nullptr));
    array_position_column = MakeResolvedColumnHolder(column);
    output_column_list.push_back(array_position_column->column());

    // We add the offset column as a value table column so its name acts
    // like a range variable and we get an error if it conflicts with
    // other range variables in the same FROM clause.
    ZETASQL_RETURN_IF_ERROR(name_list_rhs->AddValueTableColumn(
        offset_alias, array_position_column->column(),
        with_offset_alias != nullptr
            ? absl::implicit_cast<const ASTNode*>(with_offset_alias)
            : table_ref->with_offset()));
  }

  std::unique_ptr<const ResolvedExpr> resolved_condition;
  std::vector<std::unique_ptr<const ResolvedComputedColumn>> computed_columns;

  if (using_clause != nullptr) {
    ZETASQL_RET_CHECK(on_clause == nullptr);  // Can't have both.

    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        lhs_computed_columns;
    std::vector<std::unique_ptr<const ResolvedComputedColumn>>
        rhs_computed_columns;

    ZETASQL_RETURN_IF_ERROR(ResolveUsing(
        using_clause, *name_list_lhs, *name_list_rhs,
        is_outer_scan ? ResolvedJoinScan::LEFT : ResolvedJoinScan::INNER,
        /*is_array_scan=*/true, &lhs_computed_columns, &rhs_computed_columns,
        &computed_columns, name_list.get(), &resolved_condition));

    // Add the USING columns to the <output_column_list>.  These will show
    // up in SELECT *, and can be referenced unqualified (without a table
    // alias qualifier).
    for (const std::unique_ptr<const ResolvedComputedColumn>& column :
         lhs_computed_columns) {
      output_column_list.push_back(column->column());
    }

    // Add a Project for any columns we need to computed before the join.
    ZETASQL_RETURN_IF_ERROR(MaybeAddProjectForComputedColumns(
        std::move(lhs_computed_columns), resolved_input_scan));
    // The <rhs_computed_columns> and <computed_columns> should be empty as
    // ArrayScan has no notion of right or full outer join.
    ZETASQL_RET_CHECK(rhs_computed_columns.empty());
    ZETASQL_RET_CHECK(computed_columns.empty());
  } else {
    ZETASQL_RETURN_IF_ERROR(name_list->MergeFrom(*name_list_lhs, table_ref));
    // We explicitly add the array element and offset columns to the name_list
    // instead of merging name_list_rhs to get the exact error location.
    for (int i = 0; i < resolved_element_column_list.size(); ++i) {
      ZETASQL_RETURN_IF_ERROR(name_list->AddValueTableColumn(
          output_alias_list[i].alias, resolved_element_column_list[i],
          output_alias_list[i].alias_location));
    }

    if (array_position_column != nullptr) {
      const ASTAlias* with_offset_alias = table_ref->with_offset()->alias();
      ZETASQL_RETURN_IF_ERROR(name_list->AddValueTableColumn(
          with_offset_alias == nullptr ? kOffsetAlias
                                       : with_offset_alias->GetAsIdString(),
          array_position_column->column(),
          with_offset_alias != nullptr
              ? absl::implicit_cast<const ASTNode*>(with_offset_alias)
              : table_ref->with_offset()));
    }

    if (on_clause != nullptr) {
      const std::unique_ptr<const NameScope> on_clause_scope(
          new NameScope(scope, name_list));
      ZETASQL_RETURN_IF_ERROR(ResolveScalarExpr(on_clause->expression(),
                                        on_clause_scope.get(), "ON clause",
                                        &resolved_condition));
      ZETASQL_RETURN_IF_ERROR(CoerceExprToBool(on_clause->expression(),
                                       "JOIN ON clause", &resolved_condition));
    }
  }

  std::unique_ptr<ResolvedArrayScan> resolved_array_scan =
      MakeResolvedArrayScan(output_column_list, std::move(*resolved_input_scan),
                            std::move(resolved_array_expr_list),
                            std::move(resolved_element_column_list),
                            std::move(array_position_column),
                            std::move(resolved_condition), is_outer_scan,
                            std::move(resolved_zip_mode));

  // We can have hints attached to either or both the JOIN keyword for this
  // array scan and the table_path_expr for the array itself.
  // Add hints from both places onto the ResolvedArrayScan node.
  if (ast_join != nullptr) {
    // If we have HASH JOIN or LOOKUP JOIN on an array join, we still add those
    // hints on the ArrayScan node even though they may have no meaning.
    ZETASQL_RETURN_IF_ERROR(
        MaybeAddJoinHintKeyword(ast_join, resolved_array_scan.get()));

    ZETASQL_RETURN_IF_ERROR(
        ResolveHintsForNode(ast_join->hint(), resolved_array_scan.get()));
  }
  if (table_ref->hint() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveHintsForNode(table_ref->hint(), resolved_array_scan.get()));
  }

  // For the unique shape of FROM table.array_path, attach informational node
  // source which has no functionality impact.
  if (is_single_table_array_path) {
    resolved_array_scan->set_node_source(kNodeSourceSingleTableArrayNamePath);
  }

  *output = std::move(resolved_array_scan);
  // If we created a CAST expression for JOIN with USING, we create a wrapper
  // ProjectScan node to produce additional columns for those expressions.
  ZETASQL_RETURN_IF_ERROR(
      MaybeAddProjectForComputedColumns(std::move(computed_columns), output));
  *output_name_list = name_list;
  return absl::OkStatus();
}

void Resolver::MaybeRecordParseLocation(const ASTNode* ast_location,
                                        ResolvedNode* resolved_node) const {
  if (analyzer_options_.parse_location_record_type() !=
          PARSE_LOCATION_RECORD_NONE &&
      ast_location != nullptr) {
    resolved_node->SetParseLocationRange(ast_location->GetParseLocationRange());
  }
}

void Resolver::MaybeRecordParseLocation(
    const ParseLocationRange& parse_location,
    ResolvedNode* resolved_node) const {
  if (analyzer_options_.parse_location_record_type() !=
          PARSE_LOCATION_RECORD_NONE &&
      parse_location.IsValid()) {
    resolved_node->SetParseLocationRange(parse_location);
  }
}

void Resolver::MaybeRecordExpressionSubqueryParseLocation(
    const ASTExpressionSubquery* ast_expr_subquery,
    ResolvedNode* resolved_node) const {
  switch (analyzer_options_.parse_location_record_type()) {
    case PARSE_LOCATION_RECORD_FULL_NODE_SCOPE:
      MaybeRecordParseLocation(ast_expr_subquery, resolved_node);
      break;
    case PARSE_LOCATION_RECORD_CODE_SEARCH:
      MaybeRecordParseLocation(ast_expr_subquery->query(), resolved_node);
      break;
    case PARSE_LOCATION_RECORD_NONE:  // NO-OP
      break;
  }
}

void Resolver::MaybeRecordTVFCallParseLocation(
    const ASTTVF* ast_location, ResolvedNode* resolved_node) const {
  switch (analyzer_options_.parse_location_record_type()) {
    case PARSE_LOCATION_RECORD_FULL_NODE_SCOPE:
      MaybeRecordParseLocation(ast_location, resolved_node);
      break;
    case PARSE_LOCATION_RECORD_CODE_SEARCH:
      MaybeRecordParseLocation(ast_location->name(), resolved_node);
      break;
    case PARSE_LOCATION_RECORD_NONE:  // NO-OP
      break;
  }
}

void Resolver::MaybeRecordFunctionCallParseLocation(
    const ASTFunctionCall* ast_location, ResolvedNode* resolved_node) const {
  if (ast_location == nullptr) {
    return;
  }
  switch (analyzer_options_.parse_location_record_type()) {
    case PARSE_LOCATION_RECORD_FULL_NODE_SCOPE:
      MaybeRecordParseLocation(ast_location, resolved_node);
      break;
    case PARSE_LOCATION_RECORD_CODE_SEARCH:
      MaybeRecordParseLocation(ast_location->function(), resolved_node);
      break;
    case PARSE_LOCATION_RECORD_NONE:  // NO-OP
      break;
  }
}

void Resolver::MaybeRecordFieldAccessParseLocation(
    const ParseLocationRange& parse_location, const ASTIdentifier* ast_field,
    ResolvedExpr* resolved_field_access_expr) const {
  switch (analyzer_options_.parse_location_record_type()) {
    case PARSE_LOCATION_RECORD_CODE_SEARCH:
      MaybeRecordParseLocation(ast_field, resolved_field_access_expr);
      break;
    case PARSE_LOCATION_RECORD_FULL_NODE_SCOPE: {
      if (ast_field == nullptr) {
        MaybeRecordParseLocation(parse_location, resolved_field_access_expr);
      } else if (!parse_location.IsValid()) {
        MaybeRecordParseLocation(ast_field, resolved_field_access_expr);
      } else {
        ParseLocationRange range;
        range.set_start(parse_location.start());
        range.set_end(ast_field->GetParseLocationRange().end());
        resolved_field_access_expr->SetParseLocationRange(range);
      }
      break;
    }
    case PARSE_LOCATION_RECORD_NONE:  // NO-OP
      break;
  }
}

void Resolver::RecordArgumentParseLocationsIfPresent(
    const ASTFunctionParameter& function_argument,
    FunctionArgumentTypeOptions* options) const {
  if (analyzer_options_.parse_location_record_type() ==
      PARSE_LOCATION_RECORD_NONE) {
    return;
  }

  if (function_argument.name() != nullptr) {
    options->set_argument_name_parse_location(
        function_argument.name()->GetParseLocationRange());
  }
  if (function_argument.type() != nullptr) {
    options->set_argument_type_parse_location(
        function_argument.type()->GetParseLocationRange());
  } else if (function_argument.templated_parameter_type() != nullptr) {
    options->set_argument_type_parse_location(
        function_argument.templated_parameter_type()->GetParseLocationRange());
  } else if (function_argument.tvf_schema() != nullptr) {
    options->set_argument_type_parse_location(
        function_argument.tvf_schema()->GetParseLocationRange());
  }
}

void Resolver::RecordTVFRelationColumnParseLocationsIfPresent(
    const ASTTVFSchemaColumn& tvf_schema_column, TVFRelation::Column* column) {
  if (analyzer_options_.parse_location_record_type() ==
      PARSE_LOCATION_RECORD_NONE) {
    return;
  }
  // Column name is an optional field.
  if (tvf_schema_column.name() != nullptr) {
    column->name_parse_location_range =
        tvf_schema_column.name()->GetParseLocationRange();
  }
  column->type_parse_location_range =
      tvf_schema_column.type()->GetParseLocationRange();
}

absl::Status Resolver::ResolveModel(
    const ASTPathExpression* path_expr,
    std::unique_ptr<const ResolvedModel>* resolved_model) {
  const Model* model = nullptr;
  const absl::Status find_status =
      catalog_->FindModel(path_expr->ToIdentifierVector(), &model,
                          analyzer_options_.find_options());

  if (find_status.code() == absl::StatusCode::kNotFound) {
    return MakeSqlErrorAt(path_expr)
           << "Model not found: " << path_expr->ToIdentifierPathString();
  }
  ZETASQL_RETURN_IF_ERROR(find_status);

  *resolved_model = MakeResolvedModel(model);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveDescriptorFirstPass(
    const ASTDescriptorColumnList* column_list,
    std::unique_ptr<const ResolvedDescriptor>* resolved_descriptor) {
  std::vector<std::string> descriptor_column_name_list;
  for (const ASTDescriptorColumn* const descriptor_column :
       column_list->descriptor_column_list()) {
    descriptor_column_name_list.push_back(
        descriptor_column->name()->GetAsString());
  }
  *resolved_descriptor = MakeResolvedDescriptor(std::vector<ResolvedColumn>(),
                                                descriptor_column_name_list);
  return ::absl::OkStatus();
}

absl::Status Resolver::FinishResolvingDescriptor(
    const ASTTVFArgument* ast_tvf_argument,
    const std::unique_ptr<const NameScope>& name_scope, int sig_idx,
    std::unique_ptr<const ResolvedDescriptor>* resolved_descriptor) {
  std::vector<ResolvedColumn> descriptor_column_list;
  std::vector<std::string> descriptor_column_name_list(
      resolved_descriptor->get()->descriptor_column_name_list());

  // resolve descriptor names from input table.
  for (int i = 0;
       i < resolved_descriptor->get()->descriptor_column_name_list().size();
       i++) {
    const std::string& name =
        resolved_descriptor->get()->descriptor_column_name_list()[i];
    NameTarget target;

    if (!name_scope->LookupName(id_string_pool_->Make(name), &target)) {
      return MakeSqlErrorAt(ast_tvf_argument->descriptor()
                                ->columns()
                                ->descriptor_column_list()
                                .at(i))
             << "DESCRIPTOR specifies " << name
             << ", which does not exist in the table passed as argument "
             << sig_idx + 1;
    } else if (target.IsAmbiguous()) {
      return MakeSqlErrorAt(ast_tvf_argument->descriptor()
                                ->columns()
                                ->descriptor_column_list()
                                .at(i))
             << "DESCRIPTOR specifies " << name
             << ", which is ambiguous in the table passed as argument "
             << sig_idx + 1;
    } else {
      descriptor_column_list.push_back(target.column());
    }
  }

  *resolved_descriptor = MakeResolvedDescriptor(descriptor_column_list,
                                                descriptor_column_name_list);
  return ::absl::OkStatus();
}

absl::Status Resolver::ResolveConnection(
    const ASTExpression* path_expr_or_default,
    std::unique_ptr<const ResolvedConnection>* resolved_connection) {
  if (path_expr_or_default->Is<ASTPathExpression>()) {
    return ResolveConnectionPath(
        path_expr_or_default->GetAsOrDie<ASTPathExpression>(),
        resolved_connection);
  }
  ZETASQL_RET_CHECK(path_expr_or_default->Is<ASTDefaultLiteral>());
  return MakeSqlErrorAt(path_expr_or_default)
         << "CONNECTION DEFAULT is not supported";
}

absl::Status Resolver::ResolveConnectionPath(
    const ASTPathExpression* path_expr,
    std::unique_ptr<const ResolvedConnection>* resolved_connection) {
  const Connection* connection = nullptr;
  const absl::Status find_status =
      catalog_->FindConnection(path_expr->ToIdentifierVector(), &connection,
                               analyzer_options_.find_options());

  if (find_status.code() == absl::StatusCode::kNotFound) {
    return MakeSqlErrorAt(path_expr)
           << "Connection not found: " << path_expr->ToIdentifierPathString();
  }
  ZETASQL_RETURN_IF_ERROR(find_status);

  *resolved_connection = MakeResolvedConnection(connection);
  return absl::OkStatus();
}

bool Resolver::IsPathExpressionStartingFromNamedSubquery(
    const ASTPathExpression& path_expr) const {
  std::vector<IdString> path;
  path.reserve(path_expr.num_names() - 1);
  for (int i = 0; i < path_expr.num_names() - 1; ++i) {
    path.push_back(path_expr.names().at(i)->GetAsIdString());
    if (named_subquery_map_.contains(path)) {
      return true;
    }
  }
  return false;
}

static bool IsColumnOfTableArgument(const ASTPathExpression& path_expr,
                                    const FunctionArgumentInfo* arguments) {
  if (arguments == nullptr) {
    return false;  // No arguments are in scope.
  }
  if (path_expr.num_names() < 2) {
    return false;
  }
  const FunctionArgumentInfo::ArgumentDetails* details =
      arguments->FindTableArg(path_expr.first_name()->GetAsIdString());
  if (details == nullptr || details->arg_type.IsTemplated()) {
    return false;
  }
  const TVFRelation& table =
      details->arg_type.options().relation_input_schema();
  for (int i = 0; i < table.num_columns(); ++i) {
    const TVFSchemaColumn& column = table.column(i);
    if (zetasql_base::CaseEqual(
            path_expr.name(1)->GetAsIdString().ToStringView(), column.name)) {
      return true;
    }
  }
  return false;
}

absl::Status Resolver::ResolvePathExpressionAsTableScan(
    const ASTPathExpression* path_expr, IdString alias, bool has_explicit_alias,
    const ASTNode* alias_location, const ASTHint* hints,
    const ASTForSystemTime* for_system_time, const NameScope* scope,
    std::unique_ptr<PathExpressionSpan>* remaining_names,
    std::unique_ptr<const ResolvedTableScan>* output,
    std::shared_ptr<const NameList>* output_name_list,
    ResolvedColumnToCatalogColumnHashMap&
        out_resolved_columns_to_catalog_columns_for_scan) {
  ZETASQL_RET_CHECK(output != nullptr);
  ZETASQL_RET_CHECK(output_name_list != nullptr);
  ZETASQL_RET_CHECK(path_expr != nullptr);
  ZETASQL_RET_CHECK(!alias.empty());
  ZETASQL_RET_CHECK(alias_location != nullptr);

  if (analyzing_partition_by_clause_name_ != nullptr) {
    return MakeSqlErrorAt(path_expr)
           << analyzing_partition_by_clause_name_
           << " expression cannot contain a table scan";
  }

  // Check if the table exists.
  const Table* table = nullptr;
  int num_names_consumed = 0;
  const absl::Status find_status =
      remaining_names != nullptr
          ? catalog_->FindTableWithPathPrefix(path_expr->ToIdentifierVector(),
                                              analyzer_options_.find_options(),
                                              &num_names_consumed, &table)
          : catalog_->FindTable(path_expr->ToIdentifierVector(), &table,
                                analyzer_options_.find_options());
  if (find_status.code() == absl::StatusCode::kNotFound) {
    if (const TableValuedFunction* tvf_catalog_entry = nullptr;
        analyzer_options()
            .replace_table_not_found_error_with_tvf_error_if_applicable() &&
        catalog_
            ->FindTableValuedFunction(path_expr->ToIdentifierVector(),
                                      &tvf_catalog_entry,
                                      analyzer_options().find_options())
            .ok()) {
      if (tvf_catalog_entry->NumSignatures() > 0 &&
          !tvf_catalog_entry->GetSignature(0)->arguments().empty()) {
        return MakeSqlErrorAt(path_expr)
               << "Table-valued function must be called with an argument list: "
                  "`"
               << tvf_catalog_entry->FullName() << "(...)`";
      } else {
        return MakeSqlErrorAt(path_expr)
               << "Table-valued function with no parameters must be called "
                  "with an empty argument list: `"
               << tvf_catalog_entry->FullName() << "()`";
      }
    }
    std::string error_message;
    absl::StrAppend(&error_message,
                    "Table not found: ", path_expr->ToIdentifierPathString());

    // We didn't find the name when trying to resolve it as a table.
    // If it looks like it might have been intended as a name from the scope,
    // give a more helpful error.
    if (IsPathExpressionStartingFromScope(*path_expr, *scope)) {
      absl::StrAppend(
          &error_message,
          " (Unqualified identifiers in a FROM clause are always resolved "
          "as tables. Identifier ",
          ToIdentifierLiteral(path_expr->first_name()->GetAsIdString()),
          " is in scope but unqualified names cannot be resolved here.)");
    } else if (IsPathExpressionStartingFromNamedSubquery(*path_expr)) {
      absl::StrAppend(
          &error_message, "; Table name ", path_expr->ToIdentifierPathString(),
          " starts with a WITH clause alias and references a column from that",
          " table, which is invalid in the FROM clause");
    } else if (IsColumnOfTableArgument(*path_expr, function_argument_info_)) {
      absl::StrAppend(
          &error_message, "; Table name ", path_expr->ToIdentifierPathString(),
          " starts with a TVF table-valued argument name and references a ",
          "column from that table, which is invalid in the FROM clause");
    } else {
      const std::string table_suggestion =
          catalog_->SuggestTable(path_expr->ToIdentifierVector());
      if (!table_suggestion.empty()) {
        absl::StrAppend(&error_message, "; Did you mean ", table_suggestion,
                        "?");
      }
    }
    return MakeSqlErrorAt(path_expr) << error_message;
  }
  ZETASQL_RETURN_IF_ERROR(find_status);

  ZETASQL_RET_CHECK(table != nullptr);
  const IdString table_name = MakeIdString(table->Name());

  bool is_partial_name_path = remaining_names != nullptr &&
                              path_expr->num_names() != num_names_consumed;
  if (is_partial_name_path) {
    ZETASQL_RET_CHECK_GT(num_names_consumed, 0);
    alias = path_expr->name(num_names_consumed - 1)->GetAsIdString();
    ZETASQL_ASSIGN_OR_RETURN(auto suffix, PathExpressionSpan(*path_expr)
                                      .subspan(num_names_consumed - 1,
                                               path_expr->num_names()));
    *remaining_names = std::make_unique<PathExpressionSpan>(suffix);
  }

  const bool is_value_table = table->IsValueTable();
  if (is_value_table) {
    ZETASQL_RETURN_IF_ERROR(CheckValidValueTable(*path_expr, *table));
  }

  ResolvedColumnList column_list;
  std::shared_ptr<NameList> name_list(new NameList);
  for (int i = 0; i < table->NumColumns(); ++i) {
    const Column* column = table->GetColumn(i);
    IdString column_name = MakeIdString(column->Name());
    if (column_name.empty()) {
      column_name = MakeIdString(absl::StrCat("$col", i + 1));
    }

    column_list.emplace_back(ResolvedColumn(
        AllocateColumnId(), table_name, column_name,
        AnnotatedType(column->GetType(), column->GetTypeAnnotationMap())));
    // Save the Catalog column for this ResolvedColumn so it can later be used
    // for checking column properties like Column::IsWritableColumn().
    out_resolved_columns_to_catalog_columns_for_scan[column_list.back()] =
        column;
    if (column->IsPseudoColumn()) {
      ZETASQL_RETURN_IF_ERROR(name_list->AddPseudoColumn(
          column_name, column_list.back(), path_expr));
    } else if (is_value_table) {
      ZETASQL_RET_CHECK_EQ(i, 0);  // Verified in CheckValidValueTable.
      // Defer AddValueTableColumn until after adding the pseudo-columns.
    } else {
      // is_explicit=false because we're adding all columns of a table.
      ZETASQL_RETURN_IF_ERROR(name_list->AddColumn(column_name, column_list.back(),
                                           /*is_explicit=*/false));
    }
  }
  if (is_value_table) {
    // So far, we've accumulated the pseudo-columns only.  Now add the
    // value table column, and pass in the list of pseudo-columns so they
    // can be attached to the range variable for the value table.
    ZETASQL_RET_CHECK_EQ(name_list->num_columns(), 0);
    ZETASQL_RETURN_IF_ERROR(
        name_list->AddValueTableColumn(alias, column_list[0], path_expr,
                                       /*excluded_field_names=*/{}, name_list));
    ZETASQL_RETURN_IF_ERROR(name_list->SetIsValueTable());
  }

  std::unique_ptr<const ResolvedExpr> for_system_time_expr;
  if (for_system_time != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        ResolveForSystemTimeExpr(for_system_time, &for_system_time_expr));
  }

  std::vector<int> column_index_list(column_list.size());
  // Fill column_index_list with 0, 1, 2, ..., column_list.size()-1.
  std::iota(column_index_list.begin(), column_index_list.end(), 0);

  std::unique_ptr<ResolvedTableScan> table_scan =
      MakeResolvedTableScan(column_list, table, std::move(for_system_time_expr),
                            has_explicit_alias ? alias.ToString() : "");
  table_scan->set_column_index_list(column_index_list);
  ZETASQL_RETURN_IF_ERROR(ResolveHintsForNode(hints, table_scan.get()));
  if (table->Is<SQLView>()) {
    analyzer_output_properties_.MarkRelevant(REWRITE_INLINE_SQL_VIEWS);
  }

  // The number of columns should equal the number of regular columns plus
  // the number of pseudo-columns in name_list, but we don't maintain the
  // count of pseudo-columns so we can't check that exactly.
  ZETASQL_RET_CHECK_GE(table_scan->column_list_size(), name_list->num_columns());

  *output_name_list = name_list;
  // Add a range variable for the whole scan unless this is a value table. For
  // value tables, the column's name already serves that purpose.
  if (!is_value_table) {
    ZETASQL_ASSIGN_OR_RETURN(*output_name_list,
                     NameList::AddRangeVariableInWrappingNameList(
                         alias, alias_location, *output_name_list));
  }
  MaybeRecordParseLocation(path_expr, table_scan.get());
  *output = std::move(table_scan);
  return absl::OkStatus();
}

absl::Status Resolver::ResolveForSystemTimeExpr(
    const ASTForSystemTime* for_system_time,
    std::unique_ptr<const ResolvedExpr>* resolved) {
  // Resolve against an empty NameScope, because column references don't
  // make sense in this clause (it needs to be constant).
  ZETASQL_RETURN_IF_ERROR(ResolveScalarExpr(for_system_time->expression(),
                                    empty_name_scope_.get(),
                                    "FOR SYSTEM_TIME AS OF", resolved));

  // Try to coerce STRING literals to TIMESTAMP, but ignore error if it
  // didn't work - proper error will be raised below.
  if (((*resolved)->node_kind() == RESOLVED_LITERAL &&
       (*resolved)->type()->IsString())) {
    CoerceExprToType(for_system_time, type_factory_->get_timestamp(),
                     kExplicitCoercion, resolved)
        .IgnoreError();  // TODO
  }

  if (!(*resolved)->type()->IsTimestamp()) {
    return MakeSqlErrorAt(for_system_time->expression())
           << "FOR SYSTEM_TIME AS OF must be of type TIMESTAMP but was of "
              "type "
           << (*resolved)->type()->ShortTypeName(product_mode());
  }
  return absl::OkStatus();
}

absl::Status Resolver::CoerceQueryStatementResultToTypes(
    const ASTNode* ast_node, absl::Span<const Type* const> types,
    std::unique_ptr<const ResolvedScan>* scan,
    std::shared_ptr<const NameList>* output_name_list) {
  const std::vector<NamedColumn>& column_list = (*output_name_list)->columns();
  if (types.size() != column_list.size()) {
    return MakeSqlErrorAt(ast_node)
           << "Query has unexpected number of output columns, " << "expected "
           << types.size() << ", but had " << column_list.size();
  }
  ZETASQL_RET_CHECK((*scan)->node_kind() == RESOLVED_PROJECT_SCAN);
  ResolvedColumnList casted_column_list;
  std::vector<std::unique_ptr<const ResolvedComputedColumn>> casted_exprs;
  auto name_list = std::make_shared<NameList>();
  for (int i = 0; i < types.size(); ++i) {
    const Type* result_type = column_list[i].column().type();
    const Type* target_type = types[i];
    if (result_type->Equals(target_type)) {
      casted_column_list.emplace_back(column_list[i].column());
      ZETASQL_RETURN_IF_ERROR(name_list->AddColumnMaybeValueTable(
          column_list[i].name(), column_list[i].column(),
          column_list[i].is_explicit(), ast_node,
          (*output_name_list)->is_value_table()));

    } else {
      // Extract and coerce an expression out of each column.
      //
      // When the result type of the query's output column does
      // not match the target type, then we try to coerce the output
      // column to the target type.  We use assignment coercion
      // rules for determining if coercion is allowed.  We also use
      // the projected expression when present (as opposed to the
      // projected column), since that allows us to do extra coercion
      // for literals such as:
      //
      // target_type: {DATE, TIMESTAMP}
      // query: SELECT '2011-01-01' as d, '2011-01-01 12:34:56' as t;
      //
      // target_type: {ENUM}
      // query: SELECT CAST(0 AS INT32) as e;

      ZETASQL_ASSIGN_OR_RETURN(const ResolvedExpr* column_expr,
                       GetColumnExpr((*scan)->GetAs<ResolvedProjectScan>(),
                                     column_list[i].column()));
      std::unique_ptr<const ResolvedExpr> column_ref;
      if (column_expr == nullptr) {
        column_ref = MakeColumnRef(column_list[i].column());
        column_expr = column_ref.get();
      }
      // Disallow untyped parameters for now, we can't mutably change them and
      // adding in a duplicate parameter into the tree with a different type
      // triggers an error.
      if (column_expr->node_kind() == RESOLVED_PARAMETER &&
          column_expr->GetAs<ResolvedParameter>()->is_untyped()) {
        return MakeSqlErrorAt(ast_node)
               << "Untyped parameter cannot be coerced to an output target "
               << "type for a query";
      }
      SignatureMatchResult unused;
      if (!coercer_.AssignableTo(
              GetInputArgumentTypeForExpr(
                  column_expr,
                  /*pick_default_type_for_untyped_expr=*/false),
              target_type,
              /* is_explicit = */ false, &unused)) {
        return MakeSqlErrorAt(ast_node)
               << "Query column " << (i + 1) << " has type "
               << result_type->ShortTypeName(product_mode())
               << " which cannot be coerced to target type "
               << target_type->ShortTypeName(product_mode());
      }
      std::unique_ptr<const ResolvedExpr> casted_expr =
          MakeColumnRef(column_list[i].column());
      const ASTNode* ast_location =
          GetASTNodeForColumn(ast_node, i, static_cast<int>(types.size()));
      ZETASQL_RETURN_IF_ERROR(function_resolver_->AddCastOrConvertLiteral(
          ast_location, target_type, /*format=*/nullptr,
          /*time_zone=*/nullptr, TypeParameters(), &**scan,
          /* set_has_explicit_type =*/false,
          /* return_null_on_error =*/false, &casted_expr));
      const ResolvedColumn casted_column(AllocateColumnId(), kCastedColumnId,
                                         column_list[i].name(), target_type);
      RecordColumnAccess(casted_column);
      casted_column_list.emplace_back(casted_column);
      casted_exprs.push_back(
          MakeResolvedComputedColumn(casted_column, std::move(casted_expr)));
      ZETASQL_RETURN_IF_ERROR(name_list->AddColumnMaybeValueTable(
          column_list[i].name(), casted_column,
          (*output_name_list)->column(i).is_explicit(), ast_node,
          (*output_name_list)->is_value_table()));
    }
  }
  if (!casted_exprs.empty()) {
    *scan = MakeResolvedProjectScan(casted_column_list, std::move(casted_exprs),
                                    std::move(*scan));
    ZETASQL_RET_CHECK_EQ((*output_name_list)->num_columns(), name_list->num_columns());
    if ((*output_name_list)->is_value_table()) {
      ZETASQL_RETURN_IF_ERROR(name_list->SetIsValueTable());
    }
    *output_name_list = name_list;
  }

  return absl::OkStatus();
}

}  // namespace zetasql
