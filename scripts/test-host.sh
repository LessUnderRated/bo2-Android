#!/usr/bin/env bash
# Host tests for the parts of the Android port that need no phone: the touch model.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/bo2-host-tests"
mkdir -p "$out"
CXX="${CXX:-c++}"
"$CXX" -std=c++20 -Wall -Wextra -O1 -g ${SANITIZE:--fsanitize=address,undefined} -Iandroid/native/src \
    android/native/tests/touch_model_test.cpp android/native/src/touch_model.cpp -o "$out/touch_model_test"
"$out/touch_model_test"
"$CXX" -std=c++20 -Wall -Wextra -O1 -g ${SANITIZE:--fsanitize=address,undefined} -Iandroid/native/src \
    android/native/tests/display_plan_test.cpp android/native/src/display_plan.cpp -o "$out/display_plan_test"
"$out/display_plan_test"
"$CXX" -std=c++20 -Wall -Wextra -O1 -g ${SANITIZE:--fsanitize=address,undefined} -Iandroid/native/src \
    android/native/tests/graphics_test.cpp android/native/src/graphics.cpp -o "$out/graphics_test"
"$out/graphics_test"
