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

#ifndef GOOGLESQL_PUBLIC_ANNOTATION_IS_VERSIONED_H_
#define GOOGLESQL_PUBLIC_ANNOTATION_IS_VERSIONED_H_

#include "googlesql/public/annotation/non_propagating_annotation_spec.h"
#include "googlesql/public/types/annotation.h"

namespace googlesql {

// IsVersionedAnnotation is a non-propagating annotation used to indicate that a
// column (or its subfields) is versioned.
class IsVersionedAnnotation : public NonPropagatingAnnotationSpec {
 public:
  IsVersionedAnnotation() = default;
  ~IsVersionedAnnotation() override = default;

  static int GetId() { return static_cast<int>(AnnotationKind::kIsVersioned); }
  int Id() const override { return GetId(); }
};

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_ANNOTATION_IS_VERSIONED_H_
