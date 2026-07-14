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

#ifndef GOOGLESQL_ANALYZER_GRAPH_DML_RESOLVER_H_
#define GOOGLESQL_ANALYZER_GRAPH_DML_RESOLVER_H_

#include <memory>
#include <vector>

#include "googlesql/analyzer/graph_query_resolver_helper.h"
#include "googlesql/analyzer/name_scope.h"
#include "googlesql/analyzer/resolver.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/public/id_string.h"
#include "googlesql/public/property_graph.h"
#include "googlesql/public/types/graph_element_type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace googlesql {

// This class performs resolution for Graph DML operators during GoogleSQL
// analysis.
//
class GraphDmlResolver {
 public:
  GraphDmlResolver(Resolver* resolver, const PropertyGraph* graph)
      : resolver_(resolver), graph_(graph) {}

  // Resolves a Graph INSERT operator.
  // Returns a ResolvedGraphInsertScan that appends the newly created elements
  // as new columns.
  //
  // An INSERT operator can have multiple INSERT path patterns, each of which
  // must start and end with a node variable. A variable can be either of the
  // following:
  //  * Declaration: If a variable within an INSERT path pattern is not in
  //    scope, it is a declaration. The name becomes in scope for remaining
  //    INSERT path patterns and linear scans. A declaration in INSERT must
  //    have at least a label and optionally an element property specification.
  //  * Reference: If a variable within an INSERT path pattern is already in
  //    scope, it is a reference. A reference in INSERT must not contain any
  //    label or property set specification.
  //
  // If a new variable name appears multiple times within a single INSERT
  // operator, the first occurrence performs the declaration, and all subsequent
  // occurrences are references to that same element.
  //
  // Rules:
  //  * A declaration must match exactly ONE graph element table.
  //  * Edge variable reference is not allowed, as that implies mutating an
  //    existing edge, which is not allowed in INSERT. INSERT only creates new
  //    nodes or new edges between existing nodes.
  //  * Only label name as identifiers and label conjunctions (&) are allowed in
  //    a label expression.
  //  * Property specifications must map to properties in the matched table,
  //    and their values are coerced to property types using implicit
  //    assignment.
  //  * Duplicate property names in the same element specification are errors.
  // See (broken link):dml-insert for more details.
  absl::StatusOr<ResolvedGraphWithNameList<const ResolvedScan>>
  ResolveGqlInsert(const ASTGqlInsert& ast_insert, const NameScope* input_scope,
                   ResolvedGraphWithNameList<const ResolvedScan> input);

 private:
  // Resolves an AST property specification against the target element table.
  // Performs assignment based type coercion and returns an error if duplicates
  // are found.
  absl::StatusOr<
      std::vector<std::unique_ptr<const ResolvedGraphDMLPropertyItem>>>
  ResolveInsertProperties(const ASTGraphPropertySpecification* ast_prop_spec,
                          const GraphElementTable* target_table,
                          const NameScope* input_scope);

  // Builds the GraphElementType based on the properties of the target table.
  // Supports both static and dynamic graph element types.
  absl::StatusOr<const GraphElementType*> BuildGraphElementType(
      const GraphElementTable* target_table,
      GraphElementType::ElementKind element_kind);

  // Resolves an insert node element pattern.
  // - `node_pattern` is the AST node pattern to resolve.
  // - `input_scope` is the input name scope of the INSERT statement.
  // - `node_tables` is the set of all node tables in the graph.
  // - `variables` is a map of all named graph element variables seen so far in
  //   the same INSERT statement. Newly declared node variables are added to
  //   this map.
  // - `insert_node_list` is the list of all inserted node elements seen so far
  //   in the same INSERT statement. Newly created node elements are appended to
  //   this list.
  absl::StatusOr<ResolvedColumn> ResolveInsertNodePattern(
      const ASTGraphInsertNodePattern* node_pattern,
      const NameScope* input_scope,
      const absl::flat_hash_set<const GraphNodeTable*>& node_tables,
      IdStringLinkedHashMapCase<ResolvedColumn>& variables,
      std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>&
          insert_node_list);

  // Resolves an insert edge element pattern.
  // - `edge_pattern` is the AST edge pattern to resolve.
  // - `input_scope` is the input name scope of the INSERT statement.
  // - `source_col` is the resolved source node column of the edge.
  // - `dest_col` is the resolved destination node column of the edge.
  // - `edge_tables` is the set of all edge tables in the graph.
  // - `variables` is a map of all named graph element variables seen so far in
  //   the same INSERT statement. Newly declared edge variables are added to
  //   this map.
  // - `insert_node_list` is the list of all inserted node elements seen so far
  //   in the same INSERT statement.
  // - `insert_edge_list` is the list of all inserted edge elements seen so far
  //   in the same INSERT statement. Newly created edge elements are appended to
  //   this list.
  absl::StatusOr<ResolvedColumn> ResolveInsertEdgePattern(
      const ASTGraphInsertEdgePattern* edge_pattern,
      const NameScope* input_scope, const ResolvedColumn& source_col,
      const ResolvedColumn& dest_col,
      const absl::flat_hash_set<const GraphEdgeTable*>& edge_tables,
      IdStringLinkedHashMapCase<ResolvedColumn>& variables,
      const std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>&
          insert_node_list,
      std::vector<std::unique_ptr<const ResolvedComputedColumnBase>>&
          insert_edge_list);

  // Validates an edge endpoint (source or destination node) is valid for the
  // to-be-inserted edge.
  // If the endpoint node is a newly inserted node:
  //  - `actual_node_table` must not be nullptr. Validate that it matches the
  //    `expected_node_table`. Otherwise, return an error.
  // If the endpoint node is a referenced node:
  //  - `actual_node_table` must be nullptr. Validate that the node type of
  //    `node_col` is a supertype of the graph element type of the
  //    `expected_node_table`. Otherwise, return an error.
  //
  // - `error_location` is the AST node where the error occurred.
  // - `target_table` is the edge table being inserted.
  // - `expected_node_table` is the expected node table for the edge endpoint.
  // - `is_source` indicates whether the endpoint is a source or destination
  //   node.
  absl::Status ValidateEdgeEndpoint(
      const ASTGraphInsertEdgePattern& error_location,
      const GraphEdgeTable& target_table,
      const GraphNodeTable* actual_node_table,
      const GraphNodeTable& expected_node_table, const ResolvedColumn& node_col,
      bool is_source);

  Resolver* resolver_ = nullptr;
  const PropertyGraph* graph_ = nullptr;
};

}  // namespace googlesql

#endif  // GOOGLESQL_ANALYZER_GRAPH_DML_RESOLVER_H_
