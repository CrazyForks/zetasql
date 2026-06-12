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

#include "googlesql/public/kmeans_options.h"

#include "googlesql/common/testing/proto_matchers.h"
#include "googlesql/proto/kmeans_options.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace googlesql {
namespace {

using ::googlesql::testing::EqualsProto;

TEST(KMeansOptionsTest, DefaultKMeansOptions) {
  EXPECT_THAT(DefaultKMeansOptions(), EqualsProto(R"pb(
                num_iterations: 10
                distance_type: EUCLIDEAN
                num_restarts: 1
                min_relative_progress: 0.01
                init_method: RANDOM
                fail_on_invalid_vector: true
              )pb"));
}

}  // namespace
}  // namespace googlesql
