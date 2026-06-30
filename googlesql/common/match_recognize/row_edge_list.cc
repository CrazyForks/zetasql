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

#include "googlesql/common/match_recognize/row_edge_list.h"

#include <cstddef>
#include <string>
#include <vector>

#include "googlesql/common/match_recognize/compiled_nfa.h"
#include "googlesql/common/match_recognize/nfa.h"
#include "absl/base/nullability.h"
#include "googlesql/base/check.h"
#include "absl/strings/str_cat.h"

namespace googlesql::functions::match_recognize {

void RowEdgeList::ClearRows() {
  bits_.clear();
  num_rows_ = 0;
}

int RowEdgeList::num_rows() const { return num_rows_; }

int RowEdgeList::AddRow() {
  int row_index = num_rows_;
  ++num_rows_;
  size_t num_bits = static_cast<size_t>(num_rows_) * num_edges();
  size_t num_words = (num_bits + 63) / 64;
  bits_.resize(num_words, 0);
  return row_index;
}

size_t RowEdgeList::GetBitIndex(int row_number, int edge_number) const {
  ABSL_DCHECK_GE(row_number, 0);
  ABSL_DCHECK_GE(edge_number, 0);
  size_t bit_index =
      static_cast<size_t>(row_number) * num_edges() + edge_number;
  ABSL_DCHECK_LT(bit_index, static_cast<size_t>(num_rows_) * num_edges())
      << "bit index out of bounds: " << bit_index
      << " (row_number: " << row_number << ", edge_number: " << edge_number
      << ", size: " << static_cast<size_t>(num_rows_) * num_edges() << ")";
  return bit_index;
}

void RowEdgeList::MarkEdge(int row_number, int edge_number) {
  size_t bit_index = GetBitIndex(row_number, edge_number);
  bits_[bit_index / 64] |= (1ULL << (bit_index % 64));
}

void RowEdgeList::UnmarkEdge(int row_number, int edge_number) {
  size_t bit_index = GetBitIndex(row_number, edge_number);
  bits_[bit_index / 64] &= ~(1ULL << (bit_index % 64));
}

bool RowEdgeList::IsMarked(int row_number, int edge_number) const {
  size_t bit_index = GetBitIndex(row_number, edge_number);
  return (bits_[bit_index / 64] & (1ULL << (bit_index % 64))) != 0;
}

const Edge* /*absl_nullable*/ RowEdgeList::GetHighestPrecedenceMarkedEdge(
    int row_number, NFAState state) const {
  for (const Edge& edge : nfa_.GetEdgesFrom(state)) {
    if (IsMarked(row_number, edge.edge_number)) {
      return &edge;
    }
  }
  return nullptr;
}

// NOMUTANTS -- Debug utility only.
std::string RowEdgeList::DebugString() const {
  std::string result;

  // We will be next adding a string specifying the marked/unmarked status of
  // the rows, which will look something like this:
  // Rows (x = marked, o = unmarked):
  //     0123456789012345
  // 0:  oooooxxxooooooox
  // 1:  ooooxxooooooooxo
  // 2:  ooooxxooooooxxxx
  // ...
  // 10: ooxxoxooooooxooo
  //
  // To make the lines line up, we need to first calculate how many characters
  // are needed to display "<row_number>: " for the largest possible row number
  // (the "+ 2" is for the two characters after the number, the colon and the
  // space).
  size_t max_row_label_size = absl::StrCat(num_rows() - 1).size() + 2;

  // Now, add the header.
  absl::StrAppend(&result, "Rows (x = marked, o = unmarked):\n");
  absl::StrAppend(&result, std::string(max_row_label_size, ' '));
  for (int i = 0; i < num_edges(); ++i) {
    // For each edge number, show the ones digit only, as it needs to fit in one
    // character to line up with the x/o value for the edge on each row.
    absl::StrAppend(&result, std::string(1, '0' + (i % 10)));
  }
  absl::StrAppend(&result, "\n");

  // Now, add the x's and o's for each row indicate which edges are marked.
  for (int row_number = 0; row_number < num_rows(); ++row_number) {
    absl::StrAppend(&result, row_number, ": ");
    for (int edge_number = 0; edge_number < num_edges(); ++edge_number) {
      absl::StrAppend(&result, IsMarked(row_number, edge_number) ? "x" : "o");
    }
    absl::StrAppend(&result, "\n");
  }
  return result;
}

}  // namespace googlesql::functions::match_recognize
