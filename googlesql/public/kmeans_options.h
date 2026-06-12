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

#ifndef GOOGLESQL_PUBLIC_KMEANS_OPTIONS_H_
#define GOOGLESQL_PUBLIC_KMEANS_OPTIONS_H_

#include "googlesql/proto/kmeans_options.pb.h"

namespace googlesql {

// Returns a KMeansOptions proto with default values set. This is used to set
// the options argument in KMeans TVF and provides a common set of configs to
// tune the KMeans clustering algorithm.
KMeansOptions DefaultKMeansOptions();

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_KMEANS_OPTIONS_H_
