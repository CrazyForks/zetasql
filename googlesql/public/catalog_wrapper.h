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

#ifndef GOOGLESQL_PUBLIC_CATALOG_WRAPPER_H_
#define GOOGLESQL_PUBLIC_CATALOG_WRAPPER_H_

#include <memory>
#include <string>
#include <utility>

#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/procedure.h"
#include "googlesql/public/property_graph.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"

namespace googlesql {

// FunctionCatalogWrapper is an implementation of the GoogleSQL Catalog,
// that simply wraps another Catalog and only exposes its functions (not
// the wrapped Catalog's tables, types, etc.).
class FunctionCatalogWrapper : public ::googlesql::SimpleCatalog {
 public:
  FunctionCatalogWrapper(absl::string_view name,
                         ::googlesql::TypeFactory* type_factory,
                         ::googlesql::Catalog* wrapped_catalog)
      : SimpleCatalog(name, type_factory), wrapped_catalog_(wrapped_catalog) {}
  FunctionCatalogWrapper(const FunctionCatalogWrapper&) = delete;
  FunctionCatalogWrapper& operator=(const FunctionCatalogWrapper&) = delete;
  ~FunctionCatalogWrapper() override = default;

  absl::Status FindFunction(
      const absl::Span<const std::string>& path,
      const ::googlesql::Function** function,
      const FindOptions& options = FindOptions()) override;

 private:
  // Not owned.
  ::googlesql::Catalog* wrapped_catalog_ = nullptr;
};

// CatalogWrapper is a helper for implementing your own catalog that reuses
// behaviors from an underlying catalog. By default, all calls get delegated
// to the underlying catalog.
//
// One common way to use this would be to override Table resolution, but to use
// the underlying catalog for all other catalog facilities.
class CatalogWrapper : public googlesql::Catalog {
 public:
  struct Options {
    bool allow_table = true;
    bool allow_model = true;
    bool allow_function = true;
    bool allow_table_valued_function = true;
    bool allow_procedure = true;
    bool allow_type = true;
    bool allow_constant = true;
    bool allow_sequence = true;
    bool allow_property_graph = true;
    bool allow_conversion = true;

    static Options AllDisabled() {
      return {.allow_table = false,
              .allow_model = false,
              .allow_function = false,
              .allow_table_valued_function = false,
              .allow_procedure = false,
              .allow_type = false,
              .allow_constant = false,
              .allow_sequence = false,
              .allow_property_graph = false,
              .allow_conversion = false};
    }
  };

  // Takes ownership of `wrapped_owned`.
  explicit CatalogWrapper(std::unique_ptr<googlesql::Catalog> wrapped_owned)
      : wrapped_owned_(std::move(wrapped_owned)),
        wrapped_(wrapped_owned_.get()) {}

  // Does not take ownership of `wrapped`.
  explicit CatalogWrapper(googlesql::Catalog* wrapped)
      : wrapped_owned_(nullptr), wrapped_(wrapped) {}

  // Takes ownership of `wrapped_owned` and applies `options` to Find* calls on
  // the wrapped catalog.
  explicit CatalogWrapper(std::unique_ptr<googlesql::Catalog> wrapped_owned,
                          Options options)
      : wrapped_owned_(std::move(wrapped_owned)),
        wrapped_(wrapped_owned_.get()),
        options_(options) {}

  // Does not take ownership of `wrapped` and applies `options` to Find* calls
  // on the wrapped catalog.
  explicit CatalogWrapper(googlesql::Catalog* wrapped, Options options)
      : wrapped_owned_(nullptr), wrapped_(wrapped), options_(options) {}

  std::string FullName() const override { return wrapped_->FullName(); }

  absl::Status FindTable(const absl::Span<const std::string>& path,
                         const googlesql::Table** table,
                         const FindOptions& options = FindOptions()) override {
    if (!options_.allow_table) {
      *table = nullptr;
      return absl::NotFoundError(
          absl::StrCat("Table ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindTable(path, table, options);
  }

  absl::Status FindModel(const absl::Span<const std::string>& path,
                         const Model** model,
                         const FindOptions& options = FindOptions()) override {
    if (!options_.allow_model) {
      *model = nullptr;
      return absl::NotFoundError(
          absl::StrCat("Model ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindModel(path, model, options);
  };

  absl::Status FindPropertyGraph(absl::Span<const std::string> path,
                                 const PropertyGraph*& property_graph,
                                 const FindOptions& options) override {
    if (!options_.allow_property_graph) {
      property_graph = nullptr;
      return absl::NotFoundError(absl::StrCat(
          "Property graph ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindPropertyGraph(path, property_graph, options);
  }

  absl::Status FindFunction(
      const absl::Span<const std::string>& path,
      const googlesql::Function** function,
      const FindOptions& options = FindOptions()) override {
    if (!options_.allow_function) {
      *function = nullptr;
      return absl::NotFoundError(
          absl::StrCat("Function ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindFunction(path, function, options);
  }

  absl::Status FindTableValuedFunction(
      const absl::Span<const std::string>& path,
      const googlesql::TableValuedFunction** function,
      const FindOptions& options = FindOptions()) override {
    if (!options_.allow_table_valued_function) {
      *function = nullptr;
      return absl::NotFoundError(absl::StrCat(
          "Table-valued function ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindTableValuedFunction(path, function, options);
  }

  absl::Status FindProcedure(
      const absl::Span<const std::string>& path,
      const googlesql::Procedure** procedure,
      const FindOptions& options = FindOptions()) override {
    if (!options_.allow_procedure) {
      *procedure = nullptr;
      return absl::NotFoundError(
          absl::StrCat("Procedure ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindProcedure(path, procedure, options);
  }

  absl::Status FindType(const absl::Span<const std::string>& path,
                        const googlesql::Type** type,
                        const FindOptions& options = FindOptions()) override {
    if (!options_.allow_type) {
      *type = nullptr;
      return absl::NotFoundError(
          absl::StrCat("Type ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindType(path, type, options);
  }

  absl::Status FindConstantWithPathPrefix(
      const absl::Span<const std::string> path, int* num_names_consumed,
      const Constant** constant,
      const FindOptions& options = FindOptions()) override {
    if (!options_.allow_constant) {
      *num_names_consumed = 0;
      *constant = nullptr;
      return absl::NotFoundError(
          absl::StrCat("Constant ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindConstantWithPathPrefix(path, num_names_consumed,
                                                constant, options);
  }

  absl::Status FindConversion(
      const googlesql::Type* from_type, const googlesql::Type* to_type,
      const googlesql::Catalog::FindConversionOptions& options,
      googlesql::Conversion* conversion) override {
    if (!options_.allow_conversion) {
      return absl::NotFoundError(
          absl::StrCat("Conversion ", from_type->DebugString(), " to ",
                       to_type->DebugString(), " not found"));
    }
    return wrapped_->FindConversion(from_type, to_type, options, conversion);
  }

  absl::Status FindSequence(const absl::Span<const std::string>& path,
                            const Sequence** sequence,
                            const FindOptions& options) override {
    if (!options_.allow_sequence) {
      *sequence = nullptr;
      return absl::NotFoundError(
          absl::StrCat("Sequence ", absl::StrJoin(path, "."), " not found"));
    }
    return wrapped_->FindSequence(path, sequence, options);
  }

  std::string SuggestTable(
      const absl::Span<const std::string>& mistyped_path) override {
    if (!options_.allow_table) {
      return "";
    }
    return wrapped_->SuggestTable(mistyped_path);
  }

  std::string SuggestModel(
      const absl::Span<const std::string>& mistyped_path) override {
    if (!options_.allow_model) {
      return "";
    }
    return wrapped_->SuggestModel(mistyped_path);
  }

  std::string SuggestFunction(
      const absl::Span<const std::string>& mistyped_path) override {
    if (!options_.allow_function) {
      return "";
    }
    return wrapped_->SuggestFunction(mistyped_path);
  }

  std::string SuggestTableValuedFunction(
      const absl::Span<const std::string>& mistyped_path) override {
    if (!options_.allow_table_valued_function) {
      return "";
    }
    return wrapped_->SuggestTableValuedFunction(mistyped_path);
  }

  std::string SuggestConstant(
      const absl::Span<const std::string>& mistyped_path) override {
    if (!options_.allow_constant) {
      return "";
    }
    return wrapped_->SuggestConstant(mistyped_path);
  }

  std::string SuggestSequence(
      const absl::Span<const std::string>& mistyped_path) override {
    if (!options_.allow_sequence) {
      return "";
    }
    return wrapped_->SuggestSequence(mistyped_path);
  }

  std::string SuggestPropertyGraph(
      absl::Span<const std::string> mistyped_path) override {
    if (!options_.allow_property_graph) {
      return "";
    }
    return wrapped_->SuggestPropertyGraph(mistyped_path);
  }

 protected:
  std::unique_ptr<googlesql::Catalog> wrapped_owned_;
  googlesql::Catalog* wrapped_;
  Options options_;
};

// ThreadSafeCatalogWrapper is a thread safe version of CatalogWrapper for
// fetching a module catalog.
class ThreadSafeCatalogWrapper : public googlesql::Catalog {
 public:
  // Takes ownership of `wrapped_owned`. May provide a shared or
  // self owned mutex.
  explicit ThreadSafeCatalogWrapper(
      std::unique_ptr<googlesql::Catalog> wrapped_owned,
      absl::Mutex* wrapper_mutex = nullptr)
      : owned_wrapped_mutex_(wrapper_mutex == nullptr
                                 ? std::make_unique<absl::Mutex>()
                                 : nullptr),
        wrapper_mutex_(wrapper_mutex == nullptr ? owned_wrapped_mutex_.get()
                                                : wrapper_mutex),
        wrapped_owned_(std::move(wrapped_owned)),
        wrapped_(wrapped_owned_.get()) {}

  // Does not take ownership of `wrapped`. May provide a shared or
  // self owned mutex.
  explicit ThreadSafeCatalogWrapper(googlesql::Catalog* wrapped,
                                    absl::Mutex* wrapper_mutex = nullptr)
      : owned_wrapped_mutex_(wrapper_mutex == nullptr
                                 ? std::make_unique<absl::Mutex>()
                                 : nullptr),
        wrapper_mutex_(wrapper_mutex == nullptr ? owned_wrapped_mutex_.get()
                                                : wrapper_mutex),
        wrapped_owned_(nullptr),
        wrapped_(wrapped) {}

  std::string FullName() const override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FullName();
  }

  absl::Status FindTable(const absl::Span<const std::string>& path,
                         const googlesql::Table** table,
                         const FindOptions& options) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindTable(path, table, options);
  }

  absl::Status FindFunction(const absl::Span<const std::string>& path,
                            const googlesql::Function** function,
                            const FindOptions& options) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindFunction(path, function, options);
  }

  absl::Status FindTableValuedFunction(
      const absl::Span<const std::string>& path,
      const googlesql::TableValuedFunction** function,
      const FindOptions& options) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindTableValuedFunction(path, function, options);
  }

  absl::Status FindProcedure(const absl::Span<const std::string>& path,
                             const googlesql::Procedure** procedure,
                             const FindOptions& options) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindProcedure(path, procedure, options);
  }

  absl::Status FindType(const absl::Span<const std::string>& path,
                        const googlesql::Type** type,
                        const FindOptions& options) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindType(path, type, options);
  }

  absl::Status FindConstantWithPathPrefix(
      const absl::Span<const std::string> path, int* num_names_consumed,
      const Constant** constant, const FindOptions& options) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindConstantWithPathPrefix(path, num_names_consumed,
                                                constant, options);
  }

  absl::Status FindConversion(
      const googlesql::Type* from_type, const googlesql::Type* to_type,
      const googlesql::Catalog::FindConversionOptions& options,
      googlesql::Conversion* conversion) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindConversion(from_type, to_type, options, conversion);
  }

  absl::Status FindSequence(const absl::Span<const std::string>& path,
                            const Sequence** sequence,
                            const FindOptions& options) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->FindSequence(path, sequence, options);
  }

  std::string SuggestTable(
      const absl::Span<const std::string>& mistyped_path) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->SuggestTable(mistyped_path);
  }

  std::string SuggestFunction(
      const absl::Span<const std::string>& mistyped_path) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->SuggestFunction(mistyped_path);
  }

  std::string SuggestTableValuedFunction(
      const absl::Span<const std::string>& mistyped_path) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->SuggestTableValuedFunction(mistyped_path);
  }

  std::string SuggestConstant(
      const absl::Span<const std::string>& mistyped_path) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->SuggestConstant(mistyped_path);
  }

  std::string SuggestSequence(
      const absl::Span<const std::string>& mistyped_path) override {
    absl::MutexLock l(wrapper_mutex_);
    return wrapped_->SuggestSequence(mistyped_path);
  }

 protected:
  std::unique_ptr<absl::Mutex> owned_wrapped_mutex_;
  mutable absl::Mutex* wrapper_mutex_;

  std::unique_ptr<googlesql::Catalog> wrapped_owned_
      ABSL_GUARDED_BY(*wrapper_mutex_);
  googlesql::Catalog* wrapped_ ABSL_GUARDED_BY(*wrapper_mutex_);
};

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_CATALOG_WRAPPER_H_
