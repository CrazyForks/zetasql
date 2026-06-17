#!/bin/bash
#
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e
set -x

MODE=$1

if [ "$MODE" = "build" ]; then
  # Build everything.
  bazel build ${BAZEL_ARGS} -c opt ...
elif [ "$MODE" = "release" ]; then
  VERSION=$2
  if [ -z "$VERSION" ]; then
    echo "Version is required for release mode"
    exit 1
  fi
  # Build C++ execute_query
  bazel build ${BAZEL_ARGS} -c opt --dynamic_mode=off //googlesql/tools/execute_query:execute_query
  cp /googlesql/bazel-bin/googlesql/tools/execute_query/execute_query $HOME/bin/execute_query

  # Build Java artifacts
  bazel build ${BAZEL_ARGS} -c opt --define=pom_version=${VERSION} java/...

  # Stage Java artifacts for copying
  mkdir -p /googlesql/java_staging
  bin=/googlesql/bazel-bin/java/com/google/googlesql
  for artifact in client jni-channel jni-channel-darwin jni-channel-linux types; do
    cp ${bin}/${artifact//-/_}_pom.xml /googlesql/java_staging/googlesql-${artifact}.pom
    cp ${bin}/${artifact//-/_}_jar.jar /googlesql/java_staging/googlesql-${artifact}.jar
    cp ${bin}/${artifact//-/_}_src.jar /googlesql/java_staging/googlesql-${artifact}-sources.jar
    cp ${bin}/${artifact//-/_}_javadoc.jar /googlesql/java_staging/googlesql-${artifact}-javadoc.jar
  done
else
  echo "Unknown mode: $MODE"
  echo "Supported modes are: build, release"
  exit 1
fi
