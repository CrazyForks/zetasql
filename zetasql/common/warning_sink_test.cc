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

#include "zetasql/common/warning_sink.h"

#include "zetasql/common/errors.h"
#include "zetasql/base/testing/status_matchers.h"
#include "zetasql/public/parse_location.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"

using ::testing::SizeIs;

namespace zetasql {

constexpr absl::string_view kFakeFile1 = "Frozen1";
constexpr absl::string_view kFakeFile2 = "Frozen2";

TEST(WarningSinkTest, AddDuplicatesLocationIgnored) {
  WarningSink sink(/*consider_location=*/false);
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
  auto location1 = ParseLocationPoint::FromByteOffset(kFakeFile1, 2);
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlErrorAtPoint(location1) << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
  auto location2 = ParseLocationPoint::FromByteOffset(kFakeFile2, 2);
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlErrorAtPoint(location2) << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ... oh-own"));
  EXPECT_THAT(sink.warnings(), SizeIs(2));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::QUERY_TOO_COMPLEX,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(3));
  sink.Reset();
  EXPECT_THAT(sink.warnings(), SizeIs(0));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
}

TEST(WarningSinkTest, AddDuplicatesLocationConsidered) {
  WarningSink sink(/*consider_location=*/true);
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
  auto location1 = ParseLocationPoint::FromByteOffset(kFakeFile1, 2);
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlErrorAtPoint(location1) << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(2));
  auto location2 = ParseLocationPoint::FromByteOffset(kFakeFile2, 2);
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlErrorAtPoint(location2) << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(3));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlErrorAtPoint(location1) << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(3));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlErrorAtPoint(location2) << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(3));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ... oh-own"));
  EXPECT_THAT(sink.warnings(), SizeIs(4));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::QUERY_TOO_COMPLEX,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(5));
  sink.Reset();
  EXPECT_THAT(sink.warnings(), SizeIs(0));
  ZETASQL_ASSERT_OK(sink.AddWarning(DeprecationWarning::UNKNOWN,
                            MakeSqlError() << "Into the ..."));
  EXPECT_THAT(sink.warnings(), SizeIs(1));
}

}  // namespace zetasql
