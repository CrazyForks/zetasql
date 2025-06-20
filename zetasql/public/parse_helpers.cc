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

#include "zetasql/public/parse_helpers.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zetasql/base/logging.h"
#include "zetasql/parser/ast_node_kind.h"
#include "zetasql/parser/flex_tokenizer.h"
#include "zetasql/parser/parse_tree.h"
#include "zetasql/parser/parser.h"
#include "zetasql/parser/statement_properties.h"
#include "zetasql/parser/tm_token.h"
#include "zetasql/public/error_helpers.h"
#include "zetasql/public/language_options.h"
#include "zetasql/public/options.pb.h"
#include "zetasql/public/parse_location.h"
#include "zetasql/public/parse_resume_location.h"
#include "zetasql/resolved_ast/resolved_node_kind.pb.h"
#include "zetasql/base/check.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

absl::Status IsValidStatementSyntax(absl::string_view sql,
                                    ErrorMessageMode error_message_mode,
                                    const LanguageOptions& language_options) {
  std::unique_ptr<ParserOutput> parser_output;
  const absl::Status parse_status =
      ParseStatement(sql, ParserOptions(language_options), &parser_output);
  return MaybeUpdateErrorFromPayload(
      error_message_mode, /*keep_error_location_payload=*/error_message_mode ==
                              ERROR_MESSAGE_WITH_PAYLOAD,
      sql, parse_status);
}

absl::Status IsValidNextStatementSyntax(
    ParseResumeLocation* resume_location, ErrorMessageMode error_message_mode,
    bool* at_end_of_input, const LanguageOptions& language_options) {
  std::unique_ptr<ParserOutput> parser_output;
  const absl::Status parse_status =
      ParseNextStatement(resume_location, ParserOptions(language_options),
                         &parser_output, at_end_of_input);
  return MaybeUpdateErrorFromPayload(
      error_message_mode, /*keep_error_location_payload=*/error_message_mode ==
                              ERROR_MESSAGE_WITH_PAYLOAD,
      resume_location->input(), parse_status);
}

ResolvedNodeKind GetStatementKind(absl::string_view sql,
                                  const LanguageOptions& language_options) {
  return GetNextStatementKind(ParseResumeLocation::FromStringView(sql),
                              language_options);
}

ResolvedNodeKind GetStatementKind(ASTNodeKind node_kind) {
  switch (node_kind) {
    case AST_QUERY_STATEMENT:
      return RESOLVED_QUERY_STMT;
    case AST_EXPLAIN_STATEMENT:
      return RESOLVED_EXPLAIN_STMT;
    case AST_CLONE_DATA_STATEMENT:
      return RESOLVED_CLONE_DATA_STMT;
    case AST_EXPORT_DATA_STATEMENT:
      return RESOLVED_EXPORT_DATA_STMT;
    case AST_EXPORT_METADATA_STATEMENT:
      return RESOLVED_EXPORT_METADATA_STMT;
    case AST_EXPORT_MODEL_STATEMENT:
      return RESOLVED_EXPORT_MODEL_STMT;
    case AST_CALL_STATEMENT:
      return RESOLVED_CALL_STMT;
    case AST_CREATE_CONSTANT_STATEMENT:
      return RESOLVED_CREATE_CONSTANT_STMT;
    case AST_CREATE_CONNECTION_STATEMENT:
      return RESOLVED_CREATE_CONNECTION_STMT;
    case AST_CREATE_DATABASE_STATEMENT:
      return RESOLVED_CREATE_DATABASE_STMT;
    case AST_CREATE_FUNCTION_STATEMENT:
      return RESOLVED_CREATE_FUNCTION_STMT;
    case AST_CREATE_INDEX_STATEMENT:
      return RESOLVED_CREATE_INDEX_STMT;
    case AST_CREATE_MODEL_STATEMENT:
      return RESOLVED_CREATE_MODEL_STMT;
    case AST_CREATE_TABLE_FUNCTION_STATEMENT:
      return RESOLVED_CREATE_TABLE_FUNCTION_STMT;
    case AST_CREATE_PROCEDURE_STATEMENT:
      return RESOLVED_CREATE_PROCEDURE_STMT;
    case AST_CREATE_VIEW_STATEMENT:
      return RESOLVED_CREATE_VIEW_STMT;
    case AST_CREATE_MATERIALIZED_VIEW_STATEMENT:
      return RESOLVED_CREATE_MATERIALIZED_VIEW_STMT;
    case AST_CREATE_APPROX_VIEW_STATEMENT:
      return RESOLVED_CREATE_APPROX_VIEW_STMT;
    case AST_CREATE_SCHEMA_STATEMENT:
      return RESOLVED_CREATE_SCHEMA_STMT;
    case AST_CREATE_EXTERNAL_SCHEMA_STATEMENT:
      return RESOLVED_CREATE_EXTERNAL_SCHEMA_STMT;
    case AST_CREATE_SNAPSHOT_TABLE_STATEMENT:
      return RESOLVED_CREATE_SNAPSHOT_TABLE_STMT;
    case AST_CREATE_PROPERTY_GRAPH_STATEMENT:
      return RESOLVED_CREATE_PROPERTY_GRAPH_STMT;
    case AST_CREATE_TABLE_STATEMENT:
      return RESOLVED_CREATE_TABLE_STMT;
    case AST_CREATE_EXTERNAL_TABLE_STATEMENT:
      return RESOLVED_CREATE_EXTERNAL_TABLE_STMT;
    case AST_CREATE_PRIVILEGE_RESTRICTION_STATEMENT:
      return RESOLVED_CREATE_PRIVILEGE_RESTRICTION_STMT;
    case AST_CREATE_ROW_ACCESS_POLICY_STATEMENT:
      return RESOLVED_CREATE_ROW_ACCESS_POLICY_STMT;
    case AST_DEFINE_TABLE_STATEMENT:
      return RESOLVED_DEFINE_TABLE_STMT;
    case AST_DELETE_STATEMENT:
      return RESOLVED_DELETE_STMT;
    case AST_INSERT_STATEMENT:
      return RESOLVED_INSERT_STMT;
    case AST_UPDATE_STATEMENT:
      return RESOLVED_UPDATE_STMT;
    case AST_MERGE_STATEMENT:
      return RESOLVED_MERGE_STMT;
    case AST_TRUNCATE_STATEMENT:
      return RESOLVED_TRUNCATE_STMT;
    case AST_DESCRIBE_STATEMENT:
      return RESOLVED_DESCRIBE_STMT;
    case AST_SHOW_STATEMENT:
      return RESOLVED_SHOW_STMT;
    case AST_BEGIN_STATEMENT:
      return RESOLVED_BEGIN_STMT;
    case AST_SET_TRANSACTION_STATEMENT:
      return RESOLVED_SET_TRANSACTION_STMT;
    case AST_COMMIT_STATEMENT:
      return RESOLVED_COMMIT_STMT;
    case AST_ROLLBACK_STATEMENT:
      return RESOLVED_ROLLBACK_STMT;
    case AST_START_BATCH_STATEMENT:
      return RESOLVED_START_BATCH_STMT;
    case AST_RUN_BATCH_STATEMENT:
      return RESOLVED_RUN_BATCH_STMT;
    case AST_ABORT_BATCH_STATEMENT:
      return RESOLVED_ABORT_BATCH_STMT;
    case AST_UNDROP_STATEMENT:
      return RESOLVED_UNDROP_STMT;
    case AST_DROP_STATEMENT:
    case AST_DROP_ENTITY_STATEMENT:
      return RESOLVED_DROP_STMT;
    case AST_DROP_FUNCTION_STATEMENT:
      return RESOLVED_DROP_FUNCTION_STMT;
    case AST_DROP_TABLE_FUNCTION_STATEMENT:
      return RESOLVED_DROP_TABLE_FUNCTION_STMT;
    case AST_DROP_PRIVILEGE_RESTRICTION_STATEMENT:
      return RESOLVED_DROP_PRIVILEGE_RESTRICTION_STMT;
    case AST_DROP_ROW_ACCESS_POLICY_STATEMENT:
      return RESOLVED_DROP_ROW_ACCESS_POLICY_STMT;
    case AST_DROP_ALL_ROW_ACCESS_POLICIES_STATEMENT:
      return RESOLVED_DROP_ROW_ACCESS_POLICY_STMT;
    case AST_DROP_MATERIALIZED_VIEW_STATEMENT:
      return RESOLVED_DROP_MATERIALIZED_VIEW_STMT;
    case AST_DROP_SNAPSHOT_TABLE_STATEMENT:
      return RESOLVED_DROP_SNAPSHOT_TABLE_STMT;
    case AST_DROP_SEARCH_INDEX_STATEMENT:
      return RESOLVED_DROP_INDEX_STMT;
    case AST_DROP_VECTOR_INDEX_STATEMENT:
      return RESOLVED_DROP_INDEX_STMT;
    case AST_GRANT_STATEMENT:
      return RESOLVED_GRANT_STMT;
    case AST_REVOKE_STATEMENT:
      return RESOLVED_REVOKE_STMT;
    case AST_ALTER_CONNECTION_STATEMENT:
      return RESOLVED_ALTER_CONNECTION_STMT;
    case AST_ALTER_DATABASE_STATEMENT:
      return RESOLVED_ALTER_DATABASE_STMT;
    case AST_ALTER_SCHEMA_STATEMENT:
      return RESOLVED_ALTER_SCHEMA_STMT;
    case AST_ALTER_EXTERNAL_SCHEMA_STATEMENT:
      return RESOLVED_ALTER_EXTERNAL_SCHEMA_STMT;
    case AST_ALTER_TABLE_STATEMENT:
      return RESOLVED_ALTER_TABLE_STMT;
    case AST_ALTER_VIEW_STATEMENT:
      return RESOLVED_ALTER_VIEW_STMT;
    case AST_ALTER_MATERIALIZED_VIEW_STATEMENT:
      return RESOLVED_ALTER_MATERIALIZED_VIEW_STMT;
    case AST_ALTER_APPROX_VIEW_STATEMENT:
      return RESOLVED_ALTER_APPROX_VIEW_STMT;
    case AST_ALTER_PRIVILEGE_RESTRICTION_STATEMENT:
      return RESOLVED_ALTER_PRIVILEGE_RESTRICTION_STMT;
    case AST_ALTER_MODEL_STATEMENT:
      return RESOLVED_ALTER_MODEL_STMT;
    case AST_ALTER_ROW_ACCESS_POLICY_STATEMENT:
      return RESOLVED_ALTER_ROW_ACCESS_POLICY_STMT;
    case AST_ALTER_INDEX_STATEMENT:
      return RESOLVED_ALTER_INDEX_STMT;
    case AST_RENAME_STATEMENT:
      return RESOLVED_RENAME_STMT;
    case AST_IMPORT_STATEMENT:
      return RESOLVED_IMPORT_STMT;
    case AST_MODULE_STATEMENT:
      return RESOLVED_MODULE_STMT;
    case AST_ANALYZE_STATEMENT:
      return RESOLVED_ANALYZE_STMT;
    case AST_ASSERT_STATEMENT:
      return RESOLVED_ASSERT_STMT;
    case AST_SYSTEM_VARIABLE_ASSIGNMENT:
      return RESOLVED_ASSIGNMENT_STMT;
    case AST_EXECUTE_IMMEDIATE_STATEMENT:
      return RESOLVED_EXECUTE_IMMEDIATE_STMT;
    case AST_ALTER_ALL_ROW_ACCESS_POLICIES_STATEMENT:
      return RESOLVED_ALTER_ALL_ROW_ACCESS_POLICIES_STMT;
    case AST_CREATE_ENTITY_STATEMENT:
      return RESOLVED_CREATE_ENTITY_STMT;
    case AST_ALTER_ENTITY_STATEMENT:
      return RESOLVED_ALTER_ENTITY_STMT;
    case AST_AUX_LOAD_DATA_STATEMENT:
      return RESOLVED_AUX_LOAD_DATA_STMT;
    default:
      break;
  }
  ZETASQL_VLOG(1) << "Unrecognized parse node kind: "
          << ASTNode::NodeKindToString(node_kind);
  return RESOLVED_LITERAL;
}

ResolvedNodeKind GetNextStatementKind(
    const ParseResumeLocation& resume_location,
    const LanguageOptions& language_options) {
  bool statement_is_ctas = false;
  ASTNodeKind node_kind = ParseNextStatementKind(
      resume_location, language_options, &statement_is_ctas);
  return statement_is_ctas ?
      RESOLVED_CREATE_TABLE_AS_SELECT_STMT : GetStatementKind(node_kind);
}

absl::Status GetStatementProperties(absl::string_view input,
                                    const LanguageOptions& language_options,
                                    StatementProperties* statement_properties) {
  return GetNextStatementProperties(ParseResumeLocation::FromStringView(input),
                                    language_options, statement_properties);
}

absl::Status GetNextStatementProperties(
    const ParseResumeLocation& resume_location,
    const LanguageOptions& language_options,
    StatementProperties* statement_properties) {
  // Parsing the next statement properties may return an AST for statement
  // level hints, so we create an arena here to own the AST nodes.
  ParserOptions parser_options;
  parser_options.set_language_options(language_options);
  parser_options.CreateDefaultArenasIfNotSet();

  parser::ASTStatementProperties ast_statement_properties;
  // Since the ASTStatementProperties will include the ASTHint node if
  // statement level hints are present, we need a vector to own the allocated
  // ASTNodes.
  std::vector<std::unique_ptr<ASTNode>> allocated_ast_nodes;

  ZETASQL_RETURN_IF_ERROR(ParseNextStatementProperties(
      resume_location, parser_options, &allocated_ast_nodes,
      &ast_statement_properties));
  statement_properties->node_kind =
      GetStatementKind(ast_statement_properties.node_kind);

  if (ast_statement_properties.is_create_table_as_select) {
    ZETASQL_RET_CHECK_EQ(statement_properties->node_kind, RESOLVED_CREATE_TABLE_STMT);
    statement_properties->node_kind = RESOLVED_CREATE_TABLE_AS_SELECT_STMT;
  }

  statement_properties->is_create_temporary_object =
      (ast_statement_properties.create_scope == ASTCreateStatement::TEMPORARY);

  // Set the statement type (DDL, DML, etc.)
  switch (ast_statement_properties.node_kind) {
    case AST_QUERY_STATEMENT:
      statement_properties->statement_category = StatementProperties::SELECT;
      break;
    case AST_ALTER_ALL_ROW_ACCESS_POLICIES_STATEMENT:
    case AST_ALTER_CONNECTION_STATEMENT:
    case AST_ALTER_DATABASE_STATEMENT:
    case AST_ALTER_ENTITY_STATEMENT:
    case AST_ALTER_MATERIALIZED_VIEW_STATEMENT:
    case AST_ALTER_APPROX_VIEW_STATEMENT:
    case AST_ALTER_PRIVILEGE_RESTRICTION_STATEMENT:
    case AST_ALTER_ROW_ACCESS_POLICY_STATEMENT:
    case AST_ALTER_SCHEMA_STATEMENT:
    case AST_ALTER_EXTERNAL_SCHEMA_STATEMENT:
    case AST_ALTER_TABLE_STATEMENT:
    case AST_ALTER_VIEW_STATEMENT:
    case AST_ALTER_MODEL_STATEMENT:
    case AST_ALTER_INDEX_STATEMENT:
    case AST_CREATE_CONSTANT_STATEMENT:
    case AST_CREATE_DATABASE_STATEMENT:
    case AST_CREATE_ENTITY_STATEMENT:
    case AST_CREATE_EXTERNAL_TABLE_STATEMENT:
    case AST_CREATE_FUNCTION_STATEMENT:
    case AST_CREATE_INDEX_STATEMENT:
    case AST_CREATE_CONNECTION_STATEMENT:
    case AST_CREATE_MATERIALIZED_VIEW_STATEMENT:
    case AST_CREATE_APPROX_VIEW_STATEMENT:
    case AST_CREATE_MODEL_STATEMENT:
    case AST_CREATE_PROCEDURE_STATEMENT:
    case AST_CREATE_PROPERTY_GRAPH_STATEMENT:
    case AST_CREATE_PRIVILEGE_RESTRICTION_STATEMENT:
    case AST_CREATE_ROW_ACCESS_POLICY_STATEMENT:
    case AST_CREATE_SCHEMA_STATEMENT:
    case AST_CREATE_EXTERNAL_SCHEMA_STATEMENT:
    case AST_CREATE_SNAPSHOT_TABLE_STATEMENT:
    case AST_CREATE_TABLE_FUNCTION_STATEMENT:
    case AST_CREATE_TABLE_STATEMENT:
    case AST_CREATE_VIEW_STATEMENT:
    case AST_DEFINE_TABLE_STATEMENT:
    case AST_DROP_ALL_ROW_ACCESS_POLICIES_STATEMENT:
    case AST_DROP_ENTITY_STATEMENT:
    case AST_DROP_FUNCTION_STATEMENT:
    case AST_DROP_TABLE_FUNCTION_STATEMENT:
    case AST_DROP_MATERIALIZED_VIEW_STATEMENT:
    case AST_DROP_PRIVILEGE_RESTRICTION_STATEMENT:
    case AST_DROP_ROW_ACCESS_POLICY_STATEMENT:
    case AST_DROP_SNAPSHOT_TABLE_STATEMENT:
    case AST_DROP_SEARCH_INDEX_STATEMENT:
    case AST_DROP_VECTOR_INDEX_STATEMENT:
    case AST_DROP_STATEMENT:
    case AST_UNDROP_STATEMENT:
    case AST_RENAME_STATEMENT:
    case AST_CREATE_SNAPSHOT_STATEMENT:
    case AST_DEFINE_MACRO_STATEMENT:
      statement_properties->statement_category = StatementProperties::DDL;
      break;
    case AST_DELETE_STATEMENT:
    case AST_INSERT_STATEMENT:
    case AST_MERGE_STATEMENT:
    case AST_TRUNCATE_STATEMENT:
    case AST_UPDATE_STATEMENT:
      statement_properties->statement_category = StatementProperties::DML;
      break;
    case AST_ANALYZE_STATEMENT:
    case AST_ABORT_BATCH_STATEMENT:
    case AST_ASSERT_STATEMENT:
    case AST_ASSIGNMENT_FROM_STRUCT:
    case AST_AUX_LOAD_DATA_STATEMENT:
    case AST_BEGIN_STATEMENT:
    case AST_BREAK_STATEMENT:
    case AST_CALL_STATEMENT:
    case AST_COMMIT_STATEMENT:
    case AST_CONTINUE_STATEMENT:
    case AST_DESCRIBE_STATEMENT:
    case AST_EXECUTE_IMMEDIATE_STATEMENT:
    case AST_EXPLAIN_STATEMENT:
    case AST_CLONE_DATA_STATEMENT:
    case AST_EXPORT_DATA_STATEMENT:
    case AST_EXPORT_MODEL_STATEMENT:
    case AST_EXPORT_METADATA_STATEMENT:
    case AST_FOR_IN_STATEMENT:
    case AST_GRANT_STATEMENT:
    case AST_IF_STATEMENT:
    case AST_IMPORT_STATEMENT:
    case AST_MODULE_STATEMENT:
    case AST_PARAMETER_ASSIGNMENT:
    case AST_RAISE_STATEMENT:
    case AST_REPEAT_STATEMENT:
    case AST_RETURN_STATEMENT:
    case AST_REVOKE_STATEMENT:
    case AST_ROLLBACK_STATEMENT:
    case AST_RUN_BATCH_STATEMENT:
    case AST_SET_TRANSACTION_STATEMENT:
    case AST_SHOW_STATEMENT:
    case AST_SINGLE_ASSIGNMENT:
    case AST_START_BATCH_STATEMENT:
    case AST_SYSTEM_VARIABLE_ASSIGNMENT:
    case AST_VARIABLE_DECLARATION:
    case AST_WHILE_STATEMENT:
    case AST_CASE_STATEMENT:
      statement_properties->statement_category = StatementProperties::OTHER;
      break;
    case kUnknownASTNodeKind:
      statement_properties->statement_category = StatementProperties::UNKNOWN;
      break;
    // We do not expect AST_HINTED_STATEMENT when the parser is in next
    // statement kind mode.
    case AST_HINTED_STATEMENT:
    default:
      ZETASQL_RET_CHECK_FAIL() << "Unexpected AST node type: "
                       << ASTNode::NodeKindToString(
                           ast_statement_properties.node_kind);
      break;
  }

  statement_properties->statement_level_hints.clear();
  ast_statement_properties.statement_level_hints.swap(
      statement_properties->statement_level_hints);

  return absl::OkStatus();
}

absl::Status SkipNextStatement(ParseResumeLocation* resume_location,
                               bool* at_end_of_input) {
  ZETASQL_RETURN_IF_ERROR(resume_location->Validate());

  auto tokenizer = std::make_unique<parser::ZetaSqlTokenizer>(
      resume_location->filename(), resume_location->input(),
      resume_location->byte_position(), /*force_flex=*/false);

  ParseLocationRange range;
  std::optional<int> semicolon_byte_offset;
  *at_end_of_input = false;
  while (true) {
    ZETASQL_ASSIGN_OR_RETURN(parser::Token next_token, tokenizer->GetNextToken(&range));
    if (next_token == parser::Token::EOI) {
      *at_end_of_input = true;
      break;
    }
    if (next_token == parser::Token::COMMENT) {
      continue;
    }
    if (semicolon_byte_offset.has_value()) {
      // We've seen a semicolon, and after continuing to process, we've hit
      // something other than EOI or COMMENT.  We are not at the end of the
      // input.
      break;
    }
    if (next_token == parser::Token::SEMICOLON) {
      // We've hit a semicolon.  We're at the end of the statement, but continue
      // consuming additional comments, breaking once we get to EOI or another
      // non-comment token.
      semicolon_byte_offset = range.end().GetByteOffset();
      continue;
    }
  }

  // If we've reached EOI, the byte position should be the size of the input,
  // regardless of where the semicolon was.
  if (*at_end_of_input) {
    resume_location->set_byte_position(
        static_cast<int>(resume_location->input().size()));
  } else {
    ZETASQL_RET_CHECK(semicolon_byte_offset.has_value());
    resume_location->set_byte_position(*semicolon_byte_offset);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> GetTopLevelTableNameFromDDLStatement(
    absl::string_view sql, const LanguageOptions& language_options) {
  bool unused_at_end_of_input;
  ParseResumeLocation resume_location =
      ParseResumeLocation::FromStringView(sql);
  return GetTopLevelTableNameFromNextDDLStatement(
      sql, resume_location, &unused_at_end_of_input, language_options);
}

absl::StatusOr<std::vector<std::string>>
GetTopLevelTableNameFromNextDDLStatement(
    absl::string_view sql, ParseResumeLocation& resume_location,
    bool* at_end_of_input, const LanguageOptions& language_options) {
  std::unique_ptr<ParserOutput> parser_output;
  ZETASQL_RETURN_IF_ERROR(ParseNextStatement(&resume_location,
                                     ParserOptions(language_options),
                                     &parser_output, at_end_of_input));
  ZETASQL_RET_CHECK(parser_output != nullptr);
  ZETASQL_RET_CHECK(parser_output->statement() != nullptr);
  const ASTStatement* statement = parser_output->statement();
  switch (statement->node_kind()) {
    case AST_CREATE_TABLE_STATEMENT:
      return statement->GetAsOrDie<ASTCreateTableStatement>()
          ->name()
          ->ToIdentifierVector();
    default:
      return zetasql_base::UnimplementedErrorBuilder()
             << "Unsupported AST node type in "
                "GetTopLevelTableNameFromNextDDLStatement: "
             << ASTNode::NodeKindToString(statement->node_kind());
      break;
  }
}

}  // namespace zetasql
