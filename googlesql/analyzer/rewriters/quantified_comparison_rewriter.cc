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

#include "googlesql/analyzer/rewriters/quantified_comparison_rewriter.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/analyzer/substitute.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output_properties.h"
#include "googlesql/public/builtin_function.pb.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.pb.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/rewriter_interface.h"
#include "googlesql/public/types/collation.h"
#include "googlesql/public/types/simple_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/types/type_modifiers.h"
#include "googlesql/public/types/type_parameters.h"
#include "googlesql/resolved_ast/column_factory.h"
#include "googlesql/resolved_ast/make_node_vector.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_builder.h"
#include "googlesql/resolved_ast/resolved_ast_enums.pb.h"
#include "googlesql/resolved_ast/resolved_ast_rewrite_visitor.h"
#include "googlesql/resolved_ast/resolved_collation.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "absl/base/no_destructor.h"
#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/types/span.h"
#include "googlesql/base/ret_check.h"

namespace googlesql {
namespace {

// Template for rewriting the ANY form using EXISTS and CASE:
//   <val> <OP> ANY UNNEST(<values>)
// Returns: BOOL
// Semantic Rules:
//   Let cmp[i] := (val OP values[i])
//   1. If cmp[i] is TRUE for any values[i], return TRUE
//   2. If values is empty or NULL, return FALSE
//   3. If cmp[i] is FALSE for all values[i], return FALSE
//   4. Otherwise return NULL
// Substitutions:
//   $0 is the comparison operator (=, !=, etc)
//   $1 is the field access specifier.  When the values are structs, this is
//      the path to the field to compare (e.g., ".field_name"), otherwise it
//      is empty.  This is used for a special case of subquery expressions that
//      have an array as the element type, and we wrap them in structs.
constexpr absl::string_view kOpAnyTemplate = R"SQL(
CASE
  WHEN EXISTS(SELECT 1 FROM UNNEST(input_values) AS y WHERE input_val $0 y$1)
    THEN TRUE
  WHEN EXISTS(SELECT 1 FROM UNNEST(input_values) AS y WHERE (input_val $0 y$1) IS NULL)
    THEN NULL
  ELSE FALSE
END
)SQL";

// Template for rewriting the ALL form using EXISTS and CASE:
//   <val> <OP> ALL UNNEST(<values>)
// Returns: BOOL
// Semantic Rules:
//   Let cmp[i] := (val OP values[i])
//   1. If values is empty or NULL, return TRUE
//   2. If cmp[i] is TRUE for all values[i], return TRUE
//   3. If cmp[i] is FALSE for any values[i], return FALSE
//   4. Otherwise return NULL
// Substitutions:
//   $0 is the comparison operator (=, !=, etc)
//   $1 is the field access specifier.  When the values are structs, this is
//      the path to the field to compare (e.g., ".field_name"), otherwise it
//      is empty.  This is used for a special case of subquery expressions that
//      have an array as the element type, and we wrap them in structs.
constexpr absl::string_view kOpAllTemplate = R"SQL(
CASE
  WHEN EXISTS(SELECT 1 FROM UNNEST(input_values) AS y WHERE NOT(input_val $0 y$1))
    THEN FALSE
  WHEN EXISTS(SELECT 1 FROM UNNEST(input_values) AS y WHERE (input_val $0 y$1) IS NULL)
    THEN NULL
  ELSE TRUE
END
)SQL";

// These are used for wrapping arrays in structs to work around the lack of
// nested arrays.
constexpr absl::string_view kStructWrapperFieldName = "v";
constexpr absl::string_view kStructWrapperFieldAccess = ".v";

// The general approach is to use AnalyzeSubstitute to rewrite the array form
// into a CASE expression with EXISTS subqueries, and the list form into a chain
// of ANDs or ORs.
//
// We cannot use the current BUILTIN_FUNCTION_INLINER with the list form since
// we have repeated arguments.
//
// We cannot make an array from the list and reuse the array rewriter since the
// list may contain arrays, and GoogleSQL currently does not support nested
// arrays.
class QuantifiedComparisonRewriteVisitor : public ResolvedASTRewriteVisitor {
 public:
  QuantifiedComparisonRewriteVisitor(const AnalyzerOptions& analyzer_options,
                                     Catalog& catalog,
                                     TypeFactory& type_factory,
                                     ColumnFactory& column_factory)
      : analyzer_options_(analyzer_options),
        catalog_(catalog),
        type_factory_(type_factory),
        column_factory_(column_factory) {}

 private:
  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedFunctionCall(
      std::unique_ptr<const ResolvedFunctionCall> node) override;

  absl::StatusOr<std::unique_ptr<const ResolvedNode>>
  PostVisitResolvedSubqueryExpr(
      std::unique_ptr<const ResolvedSubqueryExpr> node) override;

  // Rewrites a function of the form:
  //   val OP {ANY|ALL} (value1, [...])
  // to use a chain of OR or AND expressions.
  absl::StatusOr<std::unique_ptr<const ResolvedNode>> RewriteListForm(
      std::unique_ptr<const ResolvedFunctionCall> node, absl::string_view op,
      bool is_all);

  // Rewrites a function of the form:
  //   val OP {ANY|ALL} UNNEST(<array-expression>)
  // to use CASE and EXISTS subqueries.
  absl::StatusOr<std::unique_ptr<const ResolvedNode>> RewriteArrayForm(
      std::unique_ptr<const ResolvedFunctionCall> node, absl::string_view op,
      bool is_all);

  // Rewrites a subquery of the form:
  //   val OP {ANY|ALL} (SELECT ...)
  // to use CASE and EXISTS subqueries
  // the same as the array rewriter, by creating an array-subquery from the
  // scan.
  absl::StatusOr<std::unique_ptr<const ResolvedNode>> RewriteSubqueryForm(
      std::unique_ptr<const ResolvedSubqueryExpr> node, absl::string_view op,
      bool is_all);

  const AnalyzerOptions& analyzer_options_;
  Catalog& catalog_;
  TypeFactory& type_factory_;
  ColumnFactory& column_factory_;
};

struct RewriterConfig {
  bool is_array = false;
  std::string op;
  bool is_all = false;

  RewriterConfig& array() {
    is_array = true;
    return *this;
  }

  RewriterConfig& any(absl::string_view _op) {
    op = _op;
    return *this;
  }

  RewriterConfig& all(absl::string_view _op) {
    is_all = true;
    op = _op;
    return *this;
  }
};

static const RewriterConfig* /*absl_nullable*/ GetRewriterConfig(
    const ResolvedFunctionCall* node) {
  static const absl::NoDestructor<
      absl::flat_hash_map<FunctionSignatureId, RewriterConfig>>
      config_map{{
          // ANY list
          {FN_EQ_ANY, RewriterConfig().any("=")},
          {FN_NE_ANY, RewriterConfig().any("!=")},
          {FN_LT_ANY, RewriterConfig().any("<")},
          {FN_LE_ANY, RewriterConfig().any("<=")},
          {FN_GT_ANY, RewriterConfig().any(">")},
          {FN_GE_ANY, RewriterConfig().any(">=")},
          // ALL list.
          {FN_EQ_ALL, RewriterConfig().all("=")},
          {FN_NE_ALL, RewriterConfig().all("!=")},
          {FN_LT_ALL, RewriterConfig().all("<")},
          {FN_LE_ALL, RewriterConfig().all("<=")},
          {FN_GT_ALL, RewriterConfig().all(">")},
          {FN_GE_ALL, RewriterConfig().all(">=")},
          // ANY array.
          {FN_EQ_ANY_ARRAY, RewriterConfig().any("=").array()},
          {FN_NE_ANY_ARRAY, RewriterConfig().any("!=").array()},
          {FN_LT_ANY_ARRAY, RewriterConfig().any("<").array()},
          {FN_LE_ANY_ARRAY, RewriterConfig().any("<=").array()},
          {FN_GT_ANY_ARRAY, RewriterConfig().any(">").array()},
          {FN_GE_ANY_ARRAY, RewriterConfig().any(">=").array()},
          // ALL array.
          {FN_EQ_ALL_ARRAY, RewriterConfig().all("=").array()},
          {FN_NE_ALL_ARRAY, RewriterConfig().all("!=").array()},
          {FN_LT_ALL_ARRAY, RewriterConfig().all("<").array()},
          {FN_LE_ALL_ARRAY, RewriterConfig().all("<=").array()},
          {FN_GT_ALL_ARRAY, RewriterConfig().all(">").array()},
          {FN_GE_ALL_ARRAY, RewriterConfig().all(">=").array()},
      }};

  if (node->function() == nullptr || !node->function()->IsGoogleSQLBuiltin()) {
    return nullptr;
  }
  auto it =
      config_map->find(FunctionSignatureId(node->signature().context_id()));
  if (it == config_map->end()) {
    return nullptr;
  }
  return &it->second;
}

static const RewriterConfig* /*absl_nullable*/ GetSubqueryRewriterConfig(
    ResolvedSubqueryExprEnums::SubqueryType subquery_type) {
  static const absl::NoDestructor<
      absl::flat_hash_map<ResolvedSubqueryExpr::SubqueryType, RewriterConfig>>
      subquery_config_map{{
          {ResolvedSubqueryExpr::EQ_ANY, RewriterConfig().any("=")},
          {ResolvedSubqueryExpr::EQ_ALL, RewriterConfig().all("=")},
          {ResolvedSubqueryExpr::NE_ANY, RewriterConfig().any("!=")},
          {ResolvedSubqueryExpr::NE_ALL, RewriterConfig().all("!=")},
          {ResolvedSubqueryExpr::LT_ANY, RewriterConfig().any("<")},
          {ResolvedSubqueryExpr::LT_ALL, RewriterConfig().all("<")},
          {ResolvedSubqueryExpr::LE_ANY, RewriterConfig().any("<=")},
          {ResolvedSubqueryExpr::LE_ALL, RewriterConfig().all("<=")},
          {ResolvedSubqueryExpr::GT_ANY, RewriterConfig().any(">")},
          {ResolvedSubqueryExpr::GT_ALL, RewriterConfig().all(">")},
          {ResolvedSubqueryExpr::GE_ANY, RewriterConfig().any(">=")},
          {ResolvedSubqueryExpr::GE_ALL, RewriterConfig().all(">=")},
      }};

  auto it = subquery_config_map->find(subquery_type);
  if (it == subquery_config_map->end()) {
    return nullptr;
  }
  return &it->second;
}

absl::StatusOr<std::unique_ptr<const ResolvedNode>>
QuantifiedComparisonRewriteVisitor::PostVisitResolvedFunctionCall(
    std::unique_ptr<const ResolvedFunctionCall> node) {
  const RewriterConfig* /*absl_nullable*/ rewriter_info =
      GetRewriterConfig(node.get());
  if (rewriter_info == nullptr) {
    // Not a function we handle.
    return node;
  }

  if (rewriter_info->is_array) {
    return RewriteArrayForm(std::move(node), rewriter_info->op,
                            rewriter_info->is_all);
  } else {
    return RewriteListForm(std::move(node), rewriter_info->op,
                           rewriter_info->is_all);
  }
}

absl::StatusOr<std::unique_ptr<const ResolvedNode>>
QuantifiedComparisonRewriteVisitor::PostVisitResolvedSubqueryExpr(
    std::unique_ptr<const ResolvedSubqueryExpr> node) {
  const RewriterConfig* /*absl_nullable*/ rewriter_info =
      GetSubqueryRewriterConfig(node->subquery_type());
  if (rewriter_info == nullptr) {
    // Not a subquery we handle.
    return node;
  }
  return RewriteSubqueryForm(std::move(node), rewriter_info->op,
                             rewriter_info->is_all);
}

// Attach an operation collation specification to an expression.
//
// This is used to propagate collation information from a function call to
// an argument for use in a rewrite.  This is a helper for the list-form
// rewriter, where we rewrite something like $eq_any(a,b,c) into
// (a = b) OR (a = c) and rely on the equals operator propagating
// collation from its arguments.
absl::StatusOr<std::unique_ptr<const ResolvedExpr>> AttachCollationToLHS(
    std::unique_ptr<const ResolvedExpr> lhs,
    absl::Span<const ResolvedCollation> collation_list,
    TypeFactory& type_factory) {
  GOOGLESQL_RET_CHECK(!collation_list.empty());

  // Create an AnnotationMap (type collation) from the ResolvedCollation
  // (operation collation).
  std::unique_ptr<AnnotationMap> collation_map =
      AnnotationMap::Create(lhs->type());
  collation_list[0].PopulateAnnotationMap(collation_map.get());
  GOOGLESQL_ASSIGN_OR_RETURN(const AnnotationMap* common_collation,
                   type_factory.TakeOwnership(std::move(collation_map),
                                              /*normalize=*/true));

  // If the argument already has the proper collation, we don't need to do
  // anything.
  if (AnnotationMap::Equals(lhs->type_annotation_map(), common_collation)) {
    return lhs;
  }

  // Create a collation object for use in type modifiers.
  GOOGLESQL_ASSIGN_OR_RETURN(Collation common_collation_obj,
                   Collation::MakeCollation(*common_collation));

  // Attach the collation to the LHS expression.  The only valid ways to do
  // this is a CAST or a COLLATE call.  We opt for CAST here since it is more
  // general than COLLATE, which only works on strings or bytes.  If the LHS is
  // already a CAST, we update it; otherwise, we wrap it in a new CAST.
  if (lhs->node_kind() == RESOLVED_CAST) {
    // CASTs have collation in two places: in the type annotation map
    // and as a type modifier.  They need to agree so we need to update both,
    // preserving any other type modifiers (like string lengths, e.g.,
    // STRING(8)).
    const ResolvedCast* old_cast = lhs->GetAs<ResolvedCast>();
    TypeModifiers cast_type_modifiers = TypeModifiers::MakeTypeModifiers(
        old_cast->type_modifiers().type_parameters(), common_collation_obj);

    std::unique_ptr<const ResolvedCast> cast_node(
        static_cast<const ResolvedCast*>(lhs.release()));
    return ToBuilder(std::move(cast_node))
        .set_type_modifiers(cast_type_modifiers)
        .set_type_annotation_map(common_collation)
        .Build();
  } else {
    // Wrap other expression types in an identity CAST and attach the collation
    // as a type modifier.  We need to make sure the resulting ResolvedExpr
    // is valid and can stand on its own.
    //
    // (If we instead attempt a shortcut of only setting the
    // type_annotation_map(), we can create a mutant ResolvedExpr that would
    // never arise from parsing and analyzing SQL text.  This can prevent
    // SQLBuilder from working properly, and prevent the later AnalyzeSubstitute
    // call from being able to rewrite our expression into the template
    // and keep the collation semantics.)
    TypeModifiers new_type_modifiers = TypeModifiers::MakeTypeModifiers(
        TypeParameters(), common_collation_obj);
    const Type* lhs_type = lhs->type();
    std::unique_ptr<ResolvedCast> cast_node = MakeResolvedCast(
        lhs_type, std::move(lhs), /*return_null_on_error=*/false);
    cast_node->set_type_modifiers(new_type_modifiers);
    cast_node->set_type_annotation_map(common_collation);
    return cast_node;
  }
}

// Rewrite the list form by creating a chain of OR or AND expressions.
//
// (Alternatively this could be done by making an array from the expression list
// and plugging into the array rewriter, but there are challenges when the
// expressions are arrays themselves, as nested arrays are not currently
// supported.)
absl::StatusOr<std::unique_ptr<const ResolvedNode>>
QuantifiedComparisonRewriteVisitor::RewriteListForm(
    std::unique_ptr<const ResolvedFunctionCall> node, absl::string_view op,
    bool is_all) {
  // (LHS, arg1, arg2, ...)
  GOOGLESQL_RET_CHECK_GE(node->argument_list_size(), 2)
      << "Quantified comparison should have at least 2 arguments. Got: "
      << node->DebugString();

  // Take the arguments out of the node to make a new expression.
  auto builder = ToBuilder(std::move(node));
  std::vector<std::unique_ptr<const ResolvedExpr>> arguments =
      builder.release_argument_list();

  // If the function call has collation, propagate it to the LHS argument so
  // it is applied to all comparisons in the resulting chain.
  if (!builder.collation_list().empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        arguments[0],
        AttachCollationToLHS(std::move(arguments[0]), builder.collation_list(),
                             type_factory_));
  }

  // Extract the LHS value and make a template from the remaining arguments.
  //
  //   123 > ANY (1,2,3)
  //   => (input_val > arg1) OR (input_val > arg2) OR (input_val > arg3)
  //
  //   123 < ALL (1,2,3)
  //   => (input_val < arg1) AND (input_val < arg2) AND (input_val < arg3)
  //
  // AnalyzeSubstitute is used here because it guarantees that each
  // substituted variable is evaluated only once. This simplifies the
  // implementation by avoiding the need for nested subqueries to ensure
  // input_val is not re-evaluated.
  std::unique_ptr<const ResolvedExpr> val_expr = std::move(arguments[0]);
  GOOGLESQL_RET_CHECK_NE(val_expr, nullptr);
  std::string sql_template;
  std::string logical_op{is_all ? "AND" : "OR"};
  absl::flat_hash_map<std::string, const ResolvedExpr*> vars{
      {"input_val", val_expr.get()}};
  for (int i = 1; i < arguments.size(); ++i) {
    std::string arg_name = absl::StrFormat("arg%d", i);
    GOOGLESQL_RET_CHECK_NE(arguments[i].get(), nullptr);
    vars[arg_name] = arguments[i].get();
    absl::StrAppendFormat(&sql_template, " %s (input_val %s %s)",
                          (i > 1) ? logical_op : "", op, arg_name);
  }
  return AnalyzeSubstitute(analyzer_options_, catalog_, type_factory_,
                           sql_template, vars);
}

// Rewrite the array form into a CASE expression with EXISTS subqueries.
//
// (Technically this could be performed by the BUILTIN_FUNCTION_INLINER, but
// since we also need to support the list and subquery forms, which cannot use
// that rewriter (due to having variadic args or not being a function), it is
// more valuable to have all the rewrite logic and templates in one place.)
absl::StatusOr<std::unique_ptr<const ResolvedNode>>
QuantifiedComparisonRewriteVisitor::RewriteArrayForm(
    std::unique_ptr<const ResolvedFunctionCall> node, absl::string_view op,
    bool is_all) {
  GOOGLESQL_RET_CHECK_EQ(node->argument_list_size(), 2)
      << "Quantified comparison with array has exactly 2 arguments. Got: "
      << node->DebugString();

  std::string sql_template =
      absl::Substitute(is_all ? kOpAllTemplate : kOpAnyTemplate, op, "");

  const ResolvedExpr* val_expr = node->argument_list(0);
  GOOGLESQL_RET_CHECK_NE(val_expr, nullptr);
  const ResolvedExpr* values_array_expr = node->argument_list(1);
  GOOGLESQL_RET_CHECK_NE(values_array_expr, nullptr);
  return AnalyzeSubstitute(
      analyzer_options_, catalog_, type_factory_, sql_template,
      {{"input_val", val_expr}, {"input_values", values_array_expr}});
}

// Rewrite the subquery form into the same template as the array form.  We do
// this by converting the subquery into an array subquery and plugging that in
// as the array.
//
// There is special handling if the subquery element type is an array itself, in
// which case we first wrap it in a struct to work around the lack of nested
// array support.
//
// The common case can be thought of like this:
//
//   Given: 123 = any (select 456)
//     -> input_val = 123
//     -> input_values = ARRAY(select 456)
//     -> rewrite into "cmp(input_val, val) for val in input_values"
//
// When the rows in the subquery are arrays themselves, we wrap with
// a struct, essentially:
//
//   Given: [123] = any (select [456])
//     -> input_val = [123]
//     -> input_values = ARRAY(SELECT STRUCT(original_column as v)
//                             FROM (select [456] as original_column))
//     -> rewrite into "cmp(input_val, val.v) for val in input_values"
//
absl::StatusOr<std::unique_ptr<const ResolvedNode>>
QuantifiedComparisonRewriteVisitor::RewriteSubqueryForm(
    std::unique_ptr<const ResolvedSubqueryExpr> node, absl::string_view op,
    bool is_all) {
  GOOGLESQL_RET_CHECK_NE(node->in_expr(), nullptr);
  GOOGLESQL_RET_CHECK_NE(node->subquery(), nullptr);
  GOOGLESQL_RET_CHECK_EQ(node->subquery()->column_list_size(), 1);

  // We have something like:
  //   <in_expr> OP {ANY|ALL} (<subquery_scan>).
  auto builder = ToBuilder(std::move(node));
  std::unique_ptr<const ResolvedExpr> in_expr = builder.release_in_expr();
  std::unique_ptr<const ResolvedScan> subquery_scan =
      builder.release_subquery();
  std::vector<std::unique_ptr<const ResolvedColumnRef>> parameter_list =
      builder.release_parameter_list();

  // Figure out the type we will have within the array.  If the element type
  // itself is an array, we need to wrap it in a struct since nested arrays are
  // not currently supported, otherwise it is just an array of the element type.
  const Type* element_type = subquery_scan->column_list(0).type();
  const bool wrap_in_struct = element_type->IsArray();
  absl::string_view field_access =
      (wrap_in_struct ? kStructWrapperFieldAccess : "");
  const ArrayType* array_type;
  if (wrap_in_struct) {
    // Make a struct column from the element type.
    const StructType* struct_type;
    GOOGLESQL_RETURN_IF_ERROR(type_factory_.MakeStructType(
        {{std::string(kStructWrapperFieldName), element_type}}, &struct_type));
    ResolvedColumn struct_column =
        column_factory_.MakeCol("$subquery", "struct_wrapped", struct_type);

    // Replace the subquery scan with a project scan with the struct column
    // referring to the original column.
    //
    // (Note that the new resolved column ref is uncorrelated since it is the
    // original *output* column.)
    const ResolvedColumn original_column = subquery_scan->column_list(0);
    std::unique_ptr<ResolvedProjectScan> struct_wrapped_subquery_scan =
        MakeResolvedProjectScan(
            {struct_column},
            MakeNodeVector(MakeResolvedComputedColumn(
                struct_column,
                MakeResolvedMakeStruct(struct_type,
                                       MakeNodeVector(MakeResolvedColumnRef(
                                           element_type, original_column,
                                           /*is_correlated=*/false))))),
            std::move(subquery_scan));
    subquery_scan = std::move(struct_wrapped_subquery_scan);

    // ARRAY<STRUCT<v ELEMENT_TYPE>>
    GOOGLESQL_ASSIGN_OR_RETURN(
        array_type,
        type_factory_.MakeArrayType(struct_type, analyzer_options_.language()));
  } else {
    // ARRAY<ELEMENT_TYPE>
    GOOGLESQL_ASSIGN_OR_RETURN(array_type,
                     type_factory_.MakeArrayType(element_type,
                                                 analyzer_options_.language()));
  }

  // Make an array subquery expression.
  std::unique_ptr<ResolvedSubqueryExpr> array_subquery_expr =
      MakeResolvedSubqueryExpr(array_type, ResolvedSubqueryExpr::ARRAY,
                               std::move(parameter_list),
                               /*in_expr=*/nullptr, std::move(subquery_scan));

  // Propagate any hints from the original subquery.
  array_subquery_expr->set_hint_list(builder.release_hint_list());

  // If the input expression has collation, propagate it to the elements of the
  // generated array.  We have a ResolvedCollation and need to convert it to an
  // AnnotationMap to attach to the array subquery expression.
  if (!builder.in_collation().Empty()) {
    std::unique_ptr<AnnotationMap> array_annotation_map =
        AnnotationMap::Create(array_type);
    AnnotationMap* element_map =
        array_annotation_map->AsStructMap()->mutable_field(0);
    if (wrap_in_struct) {
      // If we wrapped the element in a struct, we need to set the annotation
      // map of the inner element.
      element_map = element_map->AsStructMap()->mutable_field(0);
    }
    builder.in_collation().PopulateAnnotationMap(element_map);
    GOOGLESQL_ASSIGN_OR_RETURN(
        const AnnotationMap* type_annotation_map,
        type_factory_.TakeOwnership(std::move(array_annotation_map)));
    array_subquery_expr->set_type_annotation_map(type_annotation_map);
  }

  // Plug the LHS and array subquery into the template.
  std::string sql_template = absl::Substitute(
      is_all ? kOpAllTemplate : kOpAnyTemplate, op, field_access);
  return AnalyzeSubstitute(analyzer_options_, catalog_, type_factory_,
                           sql_template,
                           {{"input_val", in_expr.get()},
                            {"input_values", array_subquery_expr.get()}});
}

}  // namespace

class QuantifiedComparisonRewriter : public Rewriter {
 public:
  absl::StatusOr<std::unique_ptr<const ResolvedNode>> Rewrite(
      const AnalyzerOptions& options, std::unique_ptr<const ResolvedNode> input,
      Catalog& catalog, TypeFactory& type_factory,
      AnalyzerOutputProperties& output_properties) const override {
    // These checks ensure that 'options' is fully initialized for use by
    // AnalyzeSubstitute and FunctionCallBuilder.
    GOOGLESQL_RET_CHECK(options.id_string_pool() != nullptr);
    GOOGLESQL_RET_CHECK(options.column_id_sequence_number() != nullptr);
    ColumnFactory column_factory(0, options.id_string_pool().get(),
                                 options.column_id_sequence_number());
    QuantifiedComparisonRewriteVisitor rewriter(options, catalog, type_factory,
                                                column_factory);
    return rewriter.VisitAll(std::move(input));
  }

  std::string Name() const override { return "QuantifiedComparisonRewriter"; }
};

const Rewriter* GetQuantifiedComparisonRewriter() {
  static const auto* kRewriter = new QuantifiedComparisonRewriter;
  return kRewriter;
}

}  // namespace googlesql
