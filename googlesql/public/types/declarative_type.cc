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

#include "googlesql/public/types/declarative_type.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.pb.h"
#include "googlesql/public/types/internal_utils.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_modifiers.h"
#include "googlesql/public/types/value_equality_check_options.h"
#include "googlesql/public/value.pb.h"
#include "absl/functional/overload.h"
#include "absl/hash/hash.h"
#include "googlesql/base/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace googlesql {

size_t DeclarativeTypeDescriptor::GetEstimatedOwnedMemoryBytesSize() const {
  return sizeof(*this) +
         // Data is stored on the heap
         sizeof(Data) +
         // Add just the heap memory, since the stack is already accounted
         // for in sizeof(Data).
         // Heap memory for the display name.
         internal::GetExternallyAllocatedMemoryEstimate(data_->display_name) +
         // Heap memory for the feature set.
         internal::GetExternallyAllocatedMemoryEstimate(
             data_->additional_required_language_features);
}

DeclarativeType::DeclarativeType(const TypeFactoryBase* factory,
                                 DeclarativeTypeDescriptor data)
    : Type(factory, TYPE_DECLARATIVE), data_(std::move(data)) {}

std::string DeclarativeType::ShortTypeName(ProductMode mode) const {
  return data_.display_name();
}

std::string DeclarativeType::TypeName(ProductMode mode) const {
  return data_.display_name();
}

absl::StatusOr<std::string> DeclarativeType::TypeNameWithModifiers(
    const TypeModifiers& type_modifiers, ProductMode mode) const {
  if (!type_modifiers.IsEmpty()) {
    return absl::InvalidArgumentError(
        "Type modifiers are not supported for declarative types");
  }
  return data_.display_name();
}

std::vector<const Type*> DeclarativeType::ComponentTypes() const { return {}; }

bool DeclarativeType::SupportsEquality() const {
  return std::visit(
      absl::Overload(
          [](DeclarativeTypeDescriptor::EqualityDisallowed) { return false; },
          [&](DeclarativeTypeDescriptor::EqualityDelegated) {
            return backing_type()->SupportsEquality();
          }),
      data_.equality_strategy());
}

bool DeclarativeType::SupportsGroupingImpl(
    const LanguageOptions& language_options,
    const Type** no_grouping_type) const {
  bool supports_grouping = std::visit(
      absl::Overload(
          [](DeclarativeTypeDescriptor::EqualityDisallowed) { return false; },
          [&](DeclarativeTypeDescriptor::EqualityDelegated) {
            // We do not care which part of the backing type caused the issue.
            // We will report the current type as the one not supporting
            // grouping below, before returning.
            return backing_type()->SupportsGroupingImpl(
                language_options,
                /*no_grouping_type=*/nullptr);
          }),
      data_.equality_strategy());

  if (!supports_grouping && no_grouping_type != nullptr) {
    *no_grouping_type = this;
  }
  return supports_grouping;
}

bool DeclarativeType::SupportsPartitioningImpl(
    const LanguageOptions& language_options,
    const Type** no_partitioning_type) const {
  bool supports_partitioning = std::visit(
      absl::Overload(
          [](DeclarativeTypeDescriptor::EqualityDisallowed) { return false; },
          [&](DeclarativeTypeDescriptor::EqualityDelegated) {
            // We do not care which part of the backing type caused the issue.
            // We will report the current type as the one not supporting
            // partitioning below, before returning.
            return backing_type()->SupportsPartitioningImpl(
                language_options, /*no_partitioning_type=*/nullptr);
          }),
      data_.equality_strategy());
  if (!supports_partitioning && no_partitioning_type != nullptr) {
    *no_partitioning_type = this;
  }
  return supports_partitioning;
}

bool DeclarativeType::SupportsOrdering(const LanguageOptions& language_options,
                                       std::string* type_description) const {
  // TODO: Support ordering strategies.
  return false;
}

void DeclarativeType::ClearValueContent(const ValueContent& value) const {
  backing_type()->ClearValueContent(value);
}

void DeclarativeType::CopyValueContent(const ValueContent& from,
                                       ValueContent* to) const {
  backing_type()->CopyValueContent(from, to);
}

std::string DeclarativeType::CapitalizedName() const {
  return data_.display_name();
}

bool DeclarativeType::IsSupportedType(
    const LanguageOptions& language_options) const {
  if (!language_options.LanguageFeatureEnabled(
          FEATURE_DECLARATIVE_TYPE_FRAMEWORK)) {
    return false;
  }
  if (!backing_type()->IsSupportedType(language_options)) {
    return false;
  }
  for (LanguageFeature feature :
       data_.additional_required_language_features()) {
    if (!language_options.LanguageFeatureEnabled(feature)) {
      return false;
    }
  }
  return true;
}

int64_t DeclarativeType::GetEstimatedOwnedMemoryBytesSize() const {
  return sizeof(*this) + data_.GetEstimatedOwnedMemoryBytesSize() -
         sizeof(data_);
}

uint64_t DeclarativeType::GetValueContentExternallyAllocatedByteSize(
    const ValueContent& value) const {
  return backing_type()->GetValueContentExternallyAllocatedByteSize(value);
}

static DeclarativeTypeProto::AllowCoercionMode CoercionModeToProto(
    DeclarativeTypeDescriptor::AllowCoercionMode coercion_mode) {
  switch (coercion_mode) {
    case DeclarativeTypeDescriptor::AllowCoercionMode::kNoCoercion:
      return DeclarativeTypeProto::ALLOW_COERCION_MODE_NO_COERCION;
    case DeclarativeTypeDescriptor::AllowCoercionMode::kExplicitOnly:
      return DeclarativeTypeProto::ALLOW_COERCION_MODE_EXPLICIT_ONLY;
    case DeclarativeTypeDescriptor::AllowCoercionMode::kAllowAllCoercion:
      return DeclarativeTypeProto::ALLOW_COERCION_MODE_ALLOW_ALL_COERCION;
  }
}

absl::Status DeclarativeType::SerializeToProtoAndDistinctFileDescriptorsImpl(
    const BuildFileDescriptorSetMapOptions& options, TypeProto* type_proto,
    FileDescriptorSetMap* file_descriptor_set_map) const {
  type_proto->set_type_kind(kind());
  DeclarativeTypeProto* declarative_type_proto =
      type_proto->mutable_declarative_type();
  DeclarativeTypeProto::TypeIdProto* type_id_proto =
      declarative_type_proto->mutable_type_id();

  type_id_proto->set_name_space(id().name_space);
  type_id_proto->set_local_id(id().local_id);
  type_id_proto->set_counter(id().counter);
  declarative_type_proto->set_display_name(data_.display_name());

  declarative_type_proto->set_coercion_from_backing_type(
      CoercionModeToProto(data_.coercion_from_backing_type()));

  declarative_type_proto->set_coercion_to_backing_type(
      CoercionModeToProto(data_.coercion_to_backing_type()));

  DeclarativeTypeProto::ReturningStrategy returning_strategy;
  std::visit(
      absl::Overload(
          [&](DeclarativeTypeDescriptor::ReturningDelegated) {
            returning_strategy = DeclarativeTypeProto::RETURNING_DELEGATED;
          },
          [&](DeclarativeTypeDescriptor::ReturningDisallowed) {
            returning_strategy = DeclarativeTypeProto::RETURNING_DISALLOWED;
          }),
      data_.returning_strategy());
  declarative_type_proto->set_returning_strategy(returning_strategy);

  DeclarativeTypeProto::EqualityStrategy equality_strategy;
  std::visit(absl::Overload(
                 [&](DeclarativeTypeDescriptor::EqualityDelegated) {
                   equality_strategy = DeclarativeTypeProto::EQUALITY_DELEGATED;
                 },
                 [&](DeclarativeTypeDescriptor::EqualityDisallowed) {
                   equality_strategy =
                       DeclarativeTypeProto::EQUALITY_DISALLOWED;
                 }),
             data_.equality_strategy());
  declarative_type_proto->set_equality_strategy(equality_strategy);

  // Serialize the backing type
  GOOGLESQL_RETURN_IF_ERROR(
      backing_type()->SerializeToProtoAndDistinctFileDescriptorsImpl(
          options, declarative_type_proto->mutable_backing_type(),
          file_descriptor_set_map));

  // Serialize additional required language features deterministically
  std::vector<LanguageFeature> sorted_features(
      data_.additional_required_language_features().begin(),
      data_.additional_required_language_features().end());
  std::sort(sorted_features.begin(), sorted_features.end());
  for (LanguageFeature feature : sorted_features) {
    declarative_type_proto->add_additional_required_language_features(feature);
  }

  return absl::OkStatus();
}

static bool AreSame(
    const DeclarativeTypeDescriptor::ReturningStrategy& returning_strategy1,
    const DeclarativeTypeDescriptor::ReturningStrategy& returning_strategy2) {
  return returning_strategy1.index() == returning_strategy2.index();
}

static bool AreSame(
    const DeclarativeTypeDescriptor::EqualityStrategy& equality_strategy1,
    const DeclarativeTypeDescriptor::EqualityStrategy& equality_strategy2) {
  return equality_strategy1.index() == equality_strategy2.index();
}

bool DeclarativeType::IsIdenticalTo(const DeclarativeType* other) const {
  if (other == nullptr) {
    return false;
  }
  return data_.IsIdenticalTo(other->data_);
}

bool DeclarativeTypeDescriptor::IsIdenticalTo(
    const DeclarativeTypeDescriptor& other) const {
  return type_id() == other.type_id() &&
         display_name() == other.display_name() &&
         backing_type()->Equals(other.backing_type()) &&
         coercion_from_backing_type() == other.coercion_from_backing_type() &&
         coercion_to_backing_type() == other.coercion_to_backing_type() &&
         AreSame(returning_strategy(), other.returning_strategy()) &&
         AreSame(equality_strategy(), other.equality_strategy()) &&
         additional_required_language_features() ==
             other.additional_required_language_features();
}

bool DeclarativeType::EqualsForSameKind(const Type* that,
                                        bool equivalent) const {
  const DeclarativeType* other = that->AsDeclarativeType();
  ABSL_DCHECK(other != nullptr);
  if (id() == other->id()) {
    ABSL_DCHECK(IsIdenticalTo(other));
    return true;
  }
  return false;
}

void DeclarativeType::DebugStringImpl(bool details, TypeOrStringVector* stack,
                                      std::string* debug_string) const {
  absl::StrAppend(debug_string, data_.display_name());
}

absl::HashState DeclarativeType::HashTypeParameter(
    absl::HashState state) const {
  return absl::HashState::combine(std::move(state), id());
}

bool DeclarativeType::ValueContentEquals(
    const ValueContent& x, const ValueContent& y,
    const ValueEqualityCheckOptions& options) const {
  ABSL_DCHECK(std::holds_alternative<DeclarativeTypeDescriptor::EqualityDelegated>(
      data_.equality_strategy()));
  return backing_type()->ValueContentEquals(x, y, options);
}

bool DeclarativeType::ValueContentLess(const ValueContent& x,
                                       const ValueContent& y,
                                       const Type* other_type) const {
  return backing_type()->ValueContentLess(x, y, other_type);
}

absl::HashState DeclarativeType::HashValueContent(const ValueContent& value,
                                                  absl::HashState state) const {
  return backing_type()->HashValueContent(value, std::move(state));
}

std::string DeclarativeType::FormatValueContent(
    const ValueContent& value, const FormatValueContentOptions& options) const {
  return "ERROR('Unimplemented')";
}

absl::Status DeclarativeType::SerializeValueContent(
    const ValueContent& value, ValueProto* value_proto) const {
  return absl::UnimplementedError("Not implemented");
}

absl::Status DeclarativeType::DeserializeValueContent(
    const ValueProto& value_proto, ValueContent* value) const {
  return absl::UnimplementedError("Not implemented");
}

}  // namespace googlesql
