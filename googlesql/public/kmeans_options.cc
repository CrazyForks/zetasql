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

#include "googlesql/proto/kmeans_options.pb.h"

namespace googlesql {

KMeansOptions DefaultKMeansOptions() {
  KMeansOptions options;
  options.set_num_iterations(10);
  options.set_distance_type(KMeansOptions::EUCLIDEAN);
  options.set_num_restarts(1);
  options.set_min_relative_progress(0.01);
  options.set_init_method(KMeansOptions::RANDOM);
  options.set_fail_on_invalid_vector(true);
  return options;
}

}  // namespace googlesql
