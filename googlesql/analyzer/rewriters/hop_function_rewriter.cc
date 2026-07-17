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

#include "googlesql/analyzer/rewriters/hop_function_rewriter.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/analyzer/substitute.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output_properties.h"
#include "googlesql/public/builtin_function.pb.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/rewriter_interface.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/column_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/resolved_ast/rewrite_utils.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace googlesql {
namespace {

struct HopFunctionArguments {
  const ResolvedTVFScan* original_node = nullptr;
  std::unique_ptr<ResolvedScan> input_relation;
  const ResolvedColumn* timestamp_column = nullptr;
  const ResolvedExpr* window_size_expr = nullptr;
  const ResolvedExpr* step_size_expr = nullptr;
  const ResolvedExpr* origin_expr = nullptr;
};

// TODO: Migrate from ResolvedASTDeepCopyVisitor to
// ResolvedASTRewriteVisitor.
class HopRewriteVisitor : public ResolvedASTDeepCopyVisitor {
 public:
  HopRewriteVisitor(const AnalyzerOptions& analyzer_options, Catalog& catalog,
                    TypeFactory& type_factory, ColumnFactory& column_factory)
      : analyzer_options_(analyzer_options),
        catalog_(catalog),
        type_factory_(type_factory),
        column_factory_(column_factory),
        fn_builder_(analyzer_options, catalog, type_factory),
        product_mode_(analyzer_options.language().product_mode()) {}

 private:
  absl::Status VisitResolvedTVFScan(const ResolvedTVFScan* node) override {
    if (node->tvf()->IsGoogleSQLBuiltin() &&
        node->function_call_signature() != nullptr &&
        node->function_call_signature()->context_id() == googlesql::FN_HOP) {
      return RewriteHop(node);
    }
    return ResolvedASTDeepCopyVisitor::VisitResolvedTVFScan(node);
  }

  // ==========================================================================
  // HOP HELPERS
  // ==========================================================================

  absl::Status RewriteHop(const ResolvedTVFScan* node) {
    // Step 1: Validate that it's the specific function we're looking for.
    GOOGLESQL_RETURN_IF_ERROR(
        ValidateTvfIsBuiltinAndSignatureMatches(node, "HOP", FN_HOP));

    // Step 2: Extract and validate arguments.
    GOOGLESQL_ASSIGN_OR_RETURN(auto args, ExtractHopArguments(node));

    // Step 3: Build the parameters CTE.
    GOOGLESQL_ASSIGN_OR_RETURN(
        auto params_cte,
        BuildHopParametersCTE(args->window_size_expr, args->step_size_expr,
                              args->origin_expr));

    absl::string_view cte_name = params_cte->with_query_name();
    const ResolvedColumnList& cte_cols =
        params_cte->with_subquery()->column_list();
    // Step 4: Build the main query to project the final output.
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedScan> main_scan,
                     BuildHopFinalProjection(*args, cte_cols, cte_name,
                                             std::move(args->input_relation)));

    GOOGLESQL_ASSIGN_OR_RETURN(auto final_plan,
                     ResolvedWithScanBuilder()
                         .set_column_list(node->column_list())
                         .add_with_entry_list(std::move(params_cte))
                         .set_query(std::move(main_scan))
                         .set_recursive(false)
                         .BuildMutable());

    PushNodeToStack(std::move(final_plan));
    return absl::OkStatus();
  }

  absl::StatusOr<std::unique_ptr<HopFunctionArguments>> ExtractHopArguments(
      const ResolvedTVFScan* node) {
    // The HOP signature handled by this rewriter requires exactly 5 arguments:
    // input table, timestamp column, window_size, step_size, and origin
    // (analyzer injects default origin if omitted).
    GOOGLESQL_RET_CHECK_EQ(node->argument_list_size(), 5);

    auto args = std::make_unique<HopFunctionArguments>();
    args->original_node = node;

    // Argument 0: Input relation.
    const ResolvedTVFArgument* input_arg = node->argument_list(0);
    GOOGLESQL_RET_CHECK(input_arg != nullptr);
    GOOGLESQL_RET_CHECK(input_arg->scan() != nullptr);
    GOOGLESQL_ASSIGN_OR_RETURN(args->input_relation,
                     ProcessNode<ResolvedScan>(input_arg->scan()));

    // Argument 1: Timestamp Column.
    const ResolvedTVFArgument* ts_arg = node->argument_list(1);
    GOOGLESQL_RET_CHECK(ts_arg != nullptr);
    const ResolvedExpr* ts_col_expr = ts_arg->expr();
    GOOGLESQL_RET_CHECK(ts_col_expr != nullptr);
    GOOGLESQL_RET_CHECK(ts_col_expr->node_kind() == RESOLVED_LITERAL);
    const ResolvedLiteral* ts_col_literal =
        ts_col_expr->GetAs<ResolvedLiteral>();
    const Value& ts_col_val = ts_col_literal->value();
    GOOGLESQL_RET_CHECK(!ts_col_val.is_null());
    GOOGLESQL_RET_CHECK(ts_col_val.type()->IsString());
    const std::string ts_col_name = ts_col_val.string_value();

    // FindAndValidateTimestampColumnInScan performs user-facing SQL validation.
    GOOGLESQL_ASSIGN_OR_RETURN(
        args->timestamp_column,
        FindAndValidateTimestampColumnInScan(
            *ts_arg, ts_col_name, *args->input_relation, "HOP", product_mode_));

    // Argument 2: Window size.
    const ResolvedTVFArgument* window_arg = node->argument_list(2);
    GOOGLESQL_RET_CHECK(window_arg != nullptr);
    args->window_size_expr = window_arg->expr();
    GOOGLESQL_RET_CHECK(args->window_size_expr != nullptr);

    // Argument 3: Step size.
    const ResolvedTVFArgument* step_arg = node->argument_list(3);
    GOOGLESQL_RET_CHECK(step_arg != nullptr);
    args->step_size_expr = step_arg->expr();
    GOOGLESQL_RET_CHECK(args->step_size_expr != nullptr);

    // Argument 4: Origin.
    const ResolvedTVFArgument* origin_arg = node->argument_list(4);
    GOOGLESQL_RET_CHECK(origin_arg != nullptr);
    args->origin_expr = origin_arg->expr();
    GOOGLESQL_RET_CHECK(args->origin_expr != nullptr);

    // Ensure arguments don't contain correlations to outer scopes. The rewriter
    // puts the argument expressions into a CTE, and GoogleSQL currently does
    // not support correlated references in a CTE. See b/244184304 and
    // (broken link).
    GOOGLESQL_RETURN_IF_ERROR(ValidateArgumentsDoNotContainCorrelation(
        node, "HOP",
        {args->window_size_expr, args->step_size_expr, args->origin_expr}));

    return args;
  }

  // Builds a Common Table Expression (CTE) named '_hop_params' to
  // pre-calculate and validate the HOP function arguments (window_size,
  // step_size, origin). This ensures that the expressions for these parameters
  // are evaluated only once. It also pre-calculates step_size in nanoseconds
  // for use in GENERATE_TIMESTAMP_ARRAY.
  //
  // The generated SQL structure for the CTE is:
  //
  // WITH _hop_params AS (
  //   SELECT
  //     IF(precomputed_raw.window_size IS NOT NULL,
  //        precomputed_raw.window_size,
  //        ERROR("HOP window_size argument cannot be NULL")
  //       ) AS window_size,
  //     IF(precomputed_raw.step_size IS NOT NULL,
  //        IF(precomputed_raw.step_size > INTERVAL 0 DAY,
  //           IF(precomputed_raw.step_size <= precomputed_raw.window_size,
  //              precomputed_raw.step_size,
  //              ERROR("Invalid HOP window_size and step_size arguments, "
  //                    "step_size has to be smaller than or equal to "
  //                    "window_size")),
  //           ERROR("HOP step_size must be positive")),
  //        ERROR("HOP step_size argument cannot be NULL")
  //       ) AS step_size,
  //     IntervalToNanos(precomputed_raw.step_size) AS step_nanos,
  //     IF(precomputed_raw.origin IS NOT NULL,
  //        precomputed_raw.origin,
  //        ERROR("HOP origin argument cannot be NULL")
  //       ) AS origin
  //   FROM (
  //     SELECT
  //       <window_size_expr> AS window_size,
  //       <step_size_expr> AS step_size,
  //       <origin_expr> AS origin
  //   ) AS precomputed_raw
  // )
  //
  // See (broken link) for more details.
  absl::StatusOr<std::unique_ptr<const ResolvedWithEntry>>
  BuildHopParametersCTE(const ResolvedExpr* window_size_expr,
                        const ResolvedExpr* step_size_expr,
                        const ResolvedExpr* origin_expr) {
    // -- Part 1: Construct the raw parameters:
    // SELECT
    //   <window_size_expr> AS window_size,
    //   <step_size_expr> AS step_size,
    //   <origin_expr> AS origin
    //   ) AS precomputed_raw.
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> compute_exprs;
    ResolvedColumnList compute_cols;

    ResolvedColumn raw_window_col = column_factory_.MakeCol(
        "$precomputed_raw", "window_size", window_size_expr->type());
    compute_cols.push_back(raw_window_col);
    GOOGLESQL_ASSIGN_OR_RETURN(auto window_expr_copy,
                     ProcessNode<ResolvedExpr>(window_size_expr));
    compute_exprs.push_back(MakeResolvedComputedColumn(
        raw_window_col, std::move(window_expr_copy)));

    ResolvedColumn raw_step_col = column_factory_.MakeCol(
        "$precomputed_raw", "step_size", step_size_expr->type());
    compute_cols.push_back(raw_step_col);
    GOOGLESQL_ASSIGN_OR_RETURN(auto step_expr_copy,
                     ProcessNode<ResolvedExpr>(step_size_expr));
    compute_exprs.push_back(
        MakeResolvedComputedColumn(raw_step_col, std::move(step_expr_copy)));

    ResolvedColumn raw_origin_col = column_factory_.MakeCol(
        "$precomputed_raw", "origin", origin_expr->type());
    compute_cols.push_back(raw_origin_col);
    GOOGLESQL_ASSIGN_OR_RETURN(auto origin_expr_copy,
                     ProcessNode<ResolvedExpr>(origin_expr));
    compute_exprs.push_back(MakeResolvedComputedColumn(
        raw_origin_col, std::move(origin_expr_copy)));

    // -- Part 2: Validate arguments and project final output.
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> final_exprs;
    ResolvedColumnList final_cols;

    // window_size (Validation).
    ResolvedColumn hop_window_col = column_factory_.MakeCol(
        "$hop_params", "window_size", raw_window_col.type());
    final_cols.push_back(hop_window_col);
    {
      auto raw_ws_ref =
          MakeResolvedColumnRef(raw_window_col.type(), raw_window_col, false);
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto validated_expr,
          AnalyzeSubstitute(analyzer_options_, catalog_, type_factory_,
                            R"sql(
              IF(raw_ws IS NOT NULL,
                 raw_ws,
                 ERROR("HOP window_size argument cannot be NULL"))
              )sql",
                            {{"raw_ws", raw_ws_ref.get()}}),
          _.With(ExpectAnalyzeSubstituteSuccess));
      final_exprs.push_back(MakeResolvedComputedColumn(
          hop_window_col, std::move(validated_expr)));
    }

    // step_size (Validation chain).
    ResolvedColumn hop_step_col = column_factory_.MakeCol(
        "$hop_params", "step_size", raw_step_col.type());
    final_cols.push_back(hop_step_col);
    {
      auto raw_ss_ref =
          MakeResolvedColumnRef(raw_step_col.type(), raw_step_col, false);
      auto raw_ws_ref =
          MakeResolvedColumnRef(raw_window_col.type(), raw_window_col, false);

      GOOGLESQL_ASSIGN_OR_RETURN(
          auto final_val,
          AnalyzeSubstitute(
              analyzer_options_, catalog_, type_factory_,
              R"sql(
              IF(raw_ss IS NOT NULL,
                 IF(raw_ss > INTERVAL 0 DAY,
                    IF(raw_ss <= raw_ws,
                       raw_ss,
                       ERROR("Invalid HOP window_size and step_size arguments, step_size has to be smaller than or equal to window_size")),
                    ERROR("HOP step_size must be positive")),
                 ERROR("HOP step_size argument cannot be NULL"))
              )sql",
              {{"raw_ss", raw_ss_ref.get()}, {"raw_ws", raw_ws_ref.get()}}),
          _.With(ExpectAnalyzeSubstituteSuccess));

      final_exprs.push_back(
          MakeResolvedComputedColumn(hop_step_col, std::move(final_val)));
    }

    // Calculate step_nanos unconditionally.
    ResolvedColumn hop_step_nanos_col = column_factory_.MakeCol(
        "$hop_params", "step_nanos", types::Int64Type());
    final_cols.push_back(hop_step_nanos_col);
    {
      auto step_col_ref_for_nanos =
          MakeResolvedColumnRef(raw_step_col.type(), raw_step_col, false);
      GOOGLESQL_ASSIGN_OR_RETURN(auto total_nanos_expr,
                       fn_builder_.IntervalToNanos(
                           std::move(step_col_ref_for_nanos), "HOP"));
      final_exprs.push_back(MakeResolvedComputedColumn(
          hop_step_nanos_col, std::move(total_nanos_expr)));
    }

    // origin (Validation).
    ResolvedColumn hop_origin_col =
        column_factory_.MakeCol("$hop_params", "origin", raw_origin_col.type());
    final_cols.push_back(hop_origin_col);
    {
      auto raw_origin_ref =
          MakeResolvedColumnRef(raw_origin_col.type(), raw_origin_col, false);
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto validated_expr,
          AnalyzeSubstitute(analyzer_options_, catalog_, type_factory_,
                            R"sql(
              IF(raw_origin IS NOT NULL,
                 raw_origin,
                 ERROR("HOP origin argument cannot be NULL"))
              )sql",
                            {{"raw_origin", raw_origin_ref.get()}}),
          _.With(ExpectAnalyzeSubstituteSuccess));
      final_exprs.push_back(MakeResolvedComputedColumn(
          hop_origin_col, std::move(validated_expr)));
    }

    return ResolvedWithEntryBuilder()
        // TODO: b/535152130 - The injected CTE name "_hop_params" can collide
        // with user-defined CTEs. CTE names in a ResolvedAST must be globally
        // unique.
        .set_with_query_name("_hop_params")
        .set_with_subquery(
            ResolvedProjectScanBuilder()
                .set_column_list(final_cols)
                .set_expr_list(std::move(final_exprs))
                .set_input_scan(
                    ResolvedProjectScanBuilder()
                        .set_column_list(compute_cols)
                        .set_expr_list(std::move(compute_exprs))
                        .set_input_scan(MakeResolvedSingleRowScan())))
        .Build();
  }

  // Builds the final projection for the HOP rewrite. This SELECT statement
  // uses the validated parameters from the '_hop_params' CTE. For each row
  // in the input table, it generates an array of possible window start times
  // and then unnests this array, effectively left outer joining the input row
  // with each window it belongs to. If the timestamp is NULL, the row is
  // preserved but the window columns will be NULL.
  //
  // The generated Resolved AST structure is something like:
  //
  // SELECT
  //   t.*,
  //   generated_windows AS window_start,
  //   TIMESTAMP_ADD(
  //     generated_windows,
  //     (SELECT window_size FROM _hop_params)
  //   ) AS window_end
  // FROM
  //   <InputTable> AS t
  //   LEFT JOIN UNNEST(
  //       -- Calculate window_from using GENERATE_TIMESTAMP_ARRAY
  //       GENERATE_TIMESTAMP_ARRAY(
  //         -- starting point
  //         TIMESTAMP_ADD(
  //           TIMESTAMP_BUCKET(
  //             TIMESTAMP_SUB(t.ts, (SELECT window_size FROM _hop_params)),
  //             (SELECT step_size FROM _hop_params),
  //             (SELECT origin FROM _hop_params)
  //           ),
  //           (SELECT step_size FROM _hop_params)
  //         ),
  //         -- end point
  //         TIMESTAMP_BUCKET(
  //           t.ts,
  //           (SELECT step_size FROM _hop_params),
  //           (SELECT origin FROM _hop_params)
  //         ),
  //         -- step expression (pre-calculated integer)
  //         INTERVAL (SELECT step_nanos FROM _hop_params) NANOSECOND
  //       )
  //     )
  //   ) AS generated_windows
  //
  // Note: Scalar subqueries are used for convenience to reference the
  // single-row _hop_params CTE values as scalar expressions without
  // having to join the CTE.
  // See (broken link) for more details.
  absl::StatusOr<std::unique_ptr<ResolvedScan>> BuildHopFinalProjection(
      const HopFunctionArguments& args, const ResolvedColumnList& cte_cols,
      absl::string_view cte_name,
      std::unique_ptr<ResolvedScan> input_relation) {
    auto get_window_interval_expr = [&]() {
      return CreateCteColumnSubquery(column_factory_, cte_name, cte_cols, 0);
    };
    auto get_step_interval_expr = [&]() {
      return CreateCteColumnSubquery(column_factory_, cte_name, cte_cols, 1);
    };
    auto get_step_nanos_expr = [&]() {
      return CreateCteColumnSubquery(column_factory_, cte_name, cte_cols, 2);
    };
    auto get_origin_expr = [&]() {
      return CreateCteColumnSubquery(column_factory_, cte_name, cte_cols, 3);
    };

    // Creates the starting point expression for GENERATE_TIMESTAMP_ARRAY:
    // TIMESTAMP_ADD(
    //   TIMESTAMP_BUCKET(
    //     TIMESTAMP_SUB(t.ts, <window_size>),
    //     <step_size>,
    //     <origin>
    //   ),
    //   <step_size>
    // )
    auto make_start_gen_array_expr =
        [&]() -> absl::StatusOr<std::unique_ptr<const ResolvedExpr>> {
      auto ts_col_ref = MakeResolvedColumnRef(args.timestamp_column->type(),
                                              *args.timestamp_column, false);
      GOOGLESQL_ASSIGN_OR_RETURN(auto window_interval_expr, get_window_interval_expr());
      GOOGLESQL_ASSIGN_OR_RETURN(auto ts_minus_window,
                       fn_builder_.Subtract(std::move(ts_col_ref),
                                            std::move(window_interval_expr)));
      std::vector<std::unique_ptr<const ResolvedExpr>> bucket_args;
      bucket_args.push_back(std::move(ts_minus_window));
      GOOGLESQL_ASSIGN_OR_RETURN(auto step_interval_expr, get_step_interval_expr());
      bucket_args.push_back(std::move(step_interval_expr));
      GOOGLESQL_ASSIGN_OR_RETURN(auto origin_expr, get_origin_expr());
      bucket_args.push_back(std::move(origin_expr));
      GOOGLESQL_ASSIGN_OR_RETURN(auto bucket,
                       fn_builder_.TimestampBucket(std::move(bucket_args)));

      GOOGLESQL_ASSIGN_OR_RETURN(auto step_interval_expr_2, get_step_interval_expr());
      return fn_builder_.Add(std::move(bucket),
                             std::move(step_interval_expr_2));
    };

    // Creates the end point expression for GENERATE_TIMESTAMP_ARRAY:
    // TIMESTAMP_BUCKET(
    //   t.ts,
    //   <step_size>,
    //   <origin>
    // )
    auto make_end_gen_array_expr =
        [&]() -> absl::StatusOr<std::unique_ptr<const ResolvedExpr>> {
      auto ts_col_ref = MakeResolvedColumnRef(args.timestamp_column->type(),
                                              *args.timestamp_column, false);
      std::vector<std::unique_ptr<const ResolvedExpr>> bucket_args;
      bucket_args.push_back(std::move(ts_col_ref));
      GOOGLESQL_ASSIGN_OR_RETURN(auto step_interval_expr, get_step_interval_expr());
      bucket_args.push_back(std::move(step_interval_expr));
      GOOGLESQL_ASSIGN_OR_RETURN(auto origin_expr, get_origin_expr());
      bucket_args.push_back(std::move(origin_expr));
      return fn_builder_.TimestampBucket(std::move(bucket_args));
    };

    // Generate the array of window starts:
    // GENERATE_TIMESTAMP_ARRAY(start, end,
    //   INTERVAL (SELECT step_nanos FROM _hop_params) NANOSECOND)
    GOOGLESQL_ASSIGN_OR_RETURN(auto start_for_gen, make_start_gen_array_expr());
    GOOGLESQL_ASSIGN_OR_RETURN(auto end_for_gen, make_end_gen_array_expr());

    auto date_part_literal = MakeResolvedLiteral(
        types::DatePartEnumType(),
        Value::Enum(types::DatePartEnumType(), functions::NANOSECOND));

    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const ResolvedExpr> step_nanos_expr,
                     get_step_nanos_expr());
    GOOGLESQL_ASSIGN_OR_RETURN(
        auto generate_array_expr,
        fn_builder_.GenerateTimestampArrayWithDatePart(
            std::move(start_for_gen), std::move(end_for_gen),
            std::move(step_nanos_expr), std::move(date_part_literal)));

    ResolvedColumn window_start_col =
        column_factory_.MakeCol("$hop", "window_start", types::TimestampType());

    // Save input columns before moving input_relation.
    const ResolvedColumnList input_columns = input_relation->column_list();

    ResolvedColumnList array_scan_output_cols = input_columns;
    array_scan_output_cols.push_back(window_start_col);
    // LEFT OUTER JOIN UNNEST(...) AS generated_windows
    // This preserves the input row even if array generation yields a NULL
    // array (if the timestamp is NULL), in which case window columns will
    // be NULL.
    auto array_scan = MakeResolvedArrayScan(
        array_scan_output_cols, std::move(input_relation),
        std::move(generate_array_expr), window_start_col,
        /*array_offset_column=*/nullptr, /*join_expr=*/nullptr,
        /*is_outer=*/true);

    const ResolvedColumnList& output_column_list =
        args.original_node->column_list();
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> output_expr_list;

    // 1. Copy all input columns, excluding any existing "window_start" or
    // "window_end" because HOP overwrites them. HOP adds its computed
    // WINDOW_START and WINDOW_END at the end of the output_column_list.
    // TODO: b/525546845 - Delete this logic. Matching on ResolvedColumn::name()
    // is fragile as it is primarily an alias/hint. This filtering will become
    // obsolete once HOP is implemented as a passthrough TVF (where columns
    // are not dropped).
    int out_index = 0;
    for (int i = 0; i < input_columns.size(); ++i) {
      if (googlesql_base::CaseEqual(input_columns[i].name(), "WINDOW_START") ||
          googlesql_base::CaseEqual(input_columns[i].name(), "WINDOW_END")) {
        continue;
      }
      output_expr_list.push_back(MakeResolvedComputedColumn(
          output_column_list[out_index++],
          MakeResolvedColumnRef(input_columns[i].type(), input_columns[i],
                                false)));
    }

    // 2. Compute the inserted time window columns (appended at the end).
    const ResolvedColumn& final_ws_col = output_column_list[out_index++];
    GOOGLESQL_RET_CHECK(googlesql_base::CaseEqual(final_ws_col.name(), "WINDOW_START"));
    output_expr_list.push_back(MakeResolvedComputedColumn(
        final_ws_col, MakeResolvedColumnRef(window_start_col.type(),
                                            window_start_col, false)));

    const ResolvedColumn& final_we_col = output_column_list[out_index++];
    GOOGLESQL_RET_CHECK(googlesql_base::CaseEqual(final_we_col.name(), "WINDOW_END"));
    auto window_start_ref =
        MakeResolvedColumnRef(window_start_col.type(), window_start_col, false);
    GOOGLESQL_ASSIGN_OR_RETURN(auto window_interval_expr, get_window_interval_expr());
    GOOGLESQL_ASSIGN_OR_RETURN(auto window_end_expr,
                     fn_builder_.Add(std::move(window_start_ref),
                                     std::move(window_interval_expr)));
    output_expr_list.push_back(
        MakeResolvedComputedColumn(final_we_col, std::move(window_end_expr)));

    GOOGLESQL_RET_CHECK_EQ(out_index, output_column_list.size());
    return ResolvedProjectScanBuilder()
        .set_column_list(output_column_list)
        .set_expr_list(std::move(output_expr_list))
        .set_input_scan(std::move(array_scan))
        .BuildMutable();
  }

  // ==========================================================================
  // SHARED HELPERS
  // ==========================================================================

  const AnalyzerOptions& analyzer_options_;
  Catalog& catalog_;
  TypeFactory& type_factory_;
  ColumnFactory& column_factory_;
  FunctionCallBuilder fn_builder_;
  ProductMode product_mode_;
};

}  // namespace

class HopFunctionRewriter : public Rewriter {
 public:
  absl::StatusOr<std::unique_ptr<const ResolvedNode>> Rewrite(
      const AnalyzerOptions& options, const ResolvedNode& input,
      Catalog& catalog, TypeFactory& type_factory,
      AnalyzerOutputProperties& output_properties) const override {
    GOOGLESQL_RET_CHECK(options.id_string_pool() != nullptr);
    GOOGLESQL_RET_CHECK(options.column_id_sequence_number() != nullptr);
    ColumnFactory column_factory(0, options.id_string_pool().get(),
                                 options.column_id_sequence_number());

    HopRewriteVisitor rewriter(options, catalog, type_factory, column_factory);
    GOOGLESQL_RETURN_IF_ERROR(input.Accept(&rewriter));
    return rewriter.ConsumeRootNode<ResolvedNode>();
  }

  std::string Name() const override { return "HopFunctionRewriter"; }
};

const Rewriter* GetHopFunctionRewriter() {
  static const auto* kRewriter = new HopFunctionRewriter;
  return kRewriter;
}

}  // namespace googlesql
