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

#ifndef GOOGLESQL_PUBLIC_SQL_PROCEDURE_H_
#define GOOGLESQL_PUBLIC_SQL_PROCEDURE_H_

#include <memory>
#include <utility>

#include "googlesql/public/catalog.h"
#include "googlesql/public/module_details.h"
#include "googlesql/public/procedure.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/memory/memory.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"

namespace googlesql {

// A SQLProcedure represents a procedure that is defined using a CREATE
// PROCEDURE statement.
//
// Serialization and deserialization of procedures are handled by the base
// `Procedure` class.
class SQLProcedure : public Procedure {
 public:
  // Creates a SQLProcedure from the resolved <create_procedure_statement>.
  // Returns an error if the SQLProcedure could not be successfully created.
  // Does not take ownership of <create_procedure_statement>, which must
  // outlive this class.
  // Creates a SQLProcedure with empty module details.
  static absl::StatusOr<std::unique_ptr<SQLProcedure>> Create(
      const ResolvedCreateProcedureStmt* create_procedure_statement) {
    return Create(create_procedure_statement, ModuleDetails::CreateEmpty());
  }

  // Same as above, but Creates a SQLProcedure with the specified
  // <module_details>.
  static absl::StatusOr<std::unique_ptr<SQLProcedure>> Create(
      const ResolvedCreateProcedureStmt* create_procedure_statement,
      ModuleDetails module_details) {
    GOOGLESQL_RETURN_IF_ERROR(
        create_procedure_statement->signature().IsValidForProcedure());
    return absl::WrapUnique(new SQLProcedure(create_procedure_statement,
                                             std::move(module_details)));
  }

  ~SQLProcedure() override = default;

  // This class is neither copyable nor assignable.
  SQLProcedure(const SQLProcedure&) = delete;
  SQLProcedure& operator=(const SQLProcedure&) = delete;

  const ResolvedCreateProcedureStmt* resolved_statement() const {
    return create_procedure_statement_;
  }

  const ModuleDetails& module_details() const { return module_details_; }

  Catalog* resolution_catalog() const { return resolution_catalog_; }

  // If set, <resolution_catalog_> is used to resolve statements in the
  // procedure body. If this object is created from a module, the resolution
  // catalog must be set.
  void set_resolution_catalog(Catalog* resolution_catalog) {
    resolution_catalog_ = resolution_catalog;
  }

 private:
  explicit SQLProcedure(
      const ResolvedCreateProcedureStmt* create_procedure_statement,
      ModuleDetails module_details)
      : Procedure(create_procedure_statement->name_path(),
                  create_procedure_statement->signature()),
        create_procedure_statement_(create_procedure_statement),
        module_details_(std::move(module_details)) {}

  // Not owned.
  const ResolvedCreateProcedureStmt* create_procedure_statement_;

  // Details about the containing GoogleSQL module.
  const ModuleDetails module_details_;

  // The catalog used to resolve the procedure body. Not owned.
  Catalog* resolution_catalog_ = nullptr;
};

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_SQL_PROCEDURE_H_
