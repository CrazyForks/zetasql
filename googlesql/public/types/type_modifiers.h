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

#ifndef GOOGLESQL_PUBLIC_TYPES_TYPE_MODIFIERS_H_
#define GOOGLESQL_PUBLIC_TYPES_TYPE_MODIFIERS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "googlesql/common/thread_stack.h"
#include "googlesql/public/type_modifiers.pb.h"
#include "googlesql/public/types/annotation.h"
#include "googlesql/public/types/collation.h"
#include "googlesql/public/types/type_parameters.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace googlesql {

// This class wraps modifiers that can follow a type name in a type
// declaration, such as type parameters and collation. Type modifiers provide
// additional information and constraints on the designated type, often
// controlling casts or coercions. For example:
//   1. CAST(input AS STRING(10)) - Produces an error if the result string is
//      longer than 10 characters.
//   2. Similarly, a column defined with type STRING(10) will also reject any
//      input string longer than 10 characters, e.g. during an INSERT operation.
//
// `TypeModifiers` bundles `TypeParameters` and `Collation` into a single
// object that can be associated with a type. Both `TypeParameters` and
// `Collation` must (individually) match the structure of the type they're
// modifying, or be empty.
//
// `Collation` controls how values should be compared, e.g. "und:ci" specifies
// case-insensitive comparison.
//
// `TypeParameters` are erasable type parameters (they apply during the
// coercion) but are then forgotten downstream.
// There are different kinds modifying various types. The examples above show
// how STRING can have TypeParameter controlling its length. Another example
// NUMERIC(P, S) which specifies the precision and scale. When specified, they
// would trim the input value to fit in the precision, and provide an error if
// the value is out of range.
class TypeModifiers {
 public:
  // Constructs TypeModifiers with input type modifier objects.
  static TypeModifiers MakeTypeModifiers(TypeParameters type_parameters,
                                         Collation collation);

  // Constructs empty TypeModifiers where each modifier class is empty. Default
  // constructor must be public to be used in the ResolvedAST.
  TypeModifiers() = default;

  TypeModifiers(const TypeModifiers& that);
  TypeModifiers(TypeModifiers&& that) noexcept = default;
  TypeModifiers& operator=(const TypeModifiers& that);
  TypeModifiers& operator=(TypeModifiers&& that) noexcept = default;
  ~TypeModifiers() = default;

  const TypeParameters& type_parameters() const;

  const Collation& collation() const;

  TypeParameters release_type_parameters();

  Collation release_collation();

  // Returns true if all modifier classes are empty.
  bool IsEmpty() const {
    return type_parameters().IsEmpty() && collation().Empty();
  }

  bool Equals(const TypeModifiers& that) const;
  bool operator==(const TypeModifiers& that) const { return Equals(that); }

  // Returns true if the 2 TypeModifiers objects are equal, disregarding
  // TimestampTypeParameters with the default precision.
  bool EqualsWithDefaultTimestampPrecision(
      const TypeModifiers& that, int64_t default_timestamp_precision) const;

  // Returns true if the TypeModifiers is equivalent to the annotation map.
  // Accounts only for annotations which correspond to some type modifiers.
  // Other annotations are ignored.
  absl::StatusOr<bool> EqualsAnnotations(
      const AnnotationMap* annotation_map,
      int64_t default_timestamp_precision) const;

  // Creates a TypeModifiers object corresponding to the `annotation map`, e.g.
  // specifying the target collation dictated by the collation annotations.
  // Ignores annotations which do not correspond to any type modifiers.
  static absl::StatusOr<TypeModifiers> MakeTypeModifiers(
      const AnnotationMap* annotation_map);

  absl::Status Serialize(TypeModifiersProto* proto) const;
  static absl::StatusOr<TypeModifiers> Deserialize(
      const TypeModifiersProto& proto);

  std::string DebugString() const;

  // Get the i-th child type modifiers.
  //
  // For example:
  // - if the type modifiers is for an ARRAY type, then GetChild(0) returns the
  //   type modifiers of the element type.
  // - if the type modifiers is for an STRUCT type, STRUCT<A type_a, B type_b,
  //   ...>, then GetChild(0) returns the type modifiers for field A,
  //   GetChild(1) returns the type modifiers for field B, etc.
  //
  // If the type modifiers is empty, returns empty type modifiers.
  // If the type modifiers is not empty, and `i` is greater than or equal the
  // number of children, an internal error is returned.
  absl::StatusOr<TypeModifiers> GetChild(int i) const;

 private:
  TypeModifiers(TypeParameters type_parameters, Collation collation);

  std::unique_ptr<TypeParameters> type_parameters_;
  std::unique_ptr<Collation> collation_;
};

GOOGLESQL_ASSERT_OBJ_SIZE(TypeModifiers, 16);

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_TYPES_TYPE_MODIFIERS_H_
