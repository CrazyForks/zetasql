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

#include "zetasql/public/types/list_backed_type.h"

#include <stack>
#include <string>

#include "zetasql/public/types/value_representations.h"
#include "zetasql/public/value_content.h"

namespace zetasql {

// Format container in non-recursive way. The users might give a deeply
// nested struct and cause stack overflow crashes for recursive methods
// TODO: Refactor for MAP type support. MapType is a container but
// does not inherit from the (now incorrectly named) ContainerType. Investigate
// factoring the heap-based stack structure out into a common supertype.
std::string ListBackedType::FormatValueContent(
    const ValueContent& value_content,
    const Type::FormatValueContentOptions& options) const {
  std::string result;
  struct Entry {
    const ValueContent value_content;
    const ListBackedType* container_type;
    int next_child_index;
  };
  std::stack<Entry> stack;
  stack.push(Entry{value_content, this, 0});
  while (!stack.empty()) {
    const Entry top = stack.top();
    const ListBackedType* container_type = top.container_type;
    internal::ValueContentOrderedListRef* container_ref =
        top.value_content.GetAs<internal::ValueContentOrderedListRef*>();
    const internal::ValueContentOrderedList* container = container_ref->value();
    // If we're just getting started printing container, then
    // print this container prefix (e.g. "[" for array or "{" for struct)
    if (top.next_child_index == 0) {
      result.append(
          container_type->GetFormatPrefix(top.value_content, options));
    }
    const size_t num_children = container->num_elements();
    int child_index = top.next_child_index;
    while (true) {
      if (child_index >= num_children) {
        result.push_back(container_type->GetFormatClosingCharacter(options));
        stack.pop();
        break;
      }
      if (child_index != 0) {
        result.append(", ");
      }
      const internal::NullableValueContent child =
          container->element(child_index);

      const Type* child_type = container_type->GetElementType(child_index);
      result.append(container_type->GetFormatElementPrefix(
          child_index, child.is_null(), options));
      ++child_index;
      if (!child.is_null() && (child_type->kind() == TYPE_STRUCT ||
                               child_type->kind() == TYPE_ARRAY)) {
        stack.top().next_child_index = child_index;
        stack.push(Entry{child.value_content(),
                         static_cast<const ListBackedType*>(child_type), 0});
        break;
      }

      std::string element_str =
          FormatNullableValueContent(child, child_type, options);
      if (options.mode == Type::FormatValueContentOptions::Mode::kDebug &&
          options.verbose) {
        element_str =
            child_type->AddCapitalizedTypePrefix(element_str, child.is_null());
      }
      result.append(element_str);
    }
  }
  return result;
}

}  // namespace zetasql
