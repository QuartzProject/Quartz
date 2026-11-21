#!/usr/bin/env bash
# Usage: test-android.sh <build directory>
# Runs the CLI and the tests that read only tests/ on the connected Android device or emulator.
set -euo pipefail

build="${1:?build directory}"
remote=/data/local/tmp/quartz-ci
tests="quartz_programs quartz_format_tests quartz_state quartz_deep quartz_capi_tests"

adb shell "rm -rf $remote && mkdir -p $remote"
adb push tests examples bench "$remote/"
adb push "$build/cli/quartz" "$remote/"
for test in $tests; do
    adb push "$build/tests/$test" "$remote/"
done
adb shell "chmod 755 $remote/quartz $remote/quartz_*"
adb shell "cd $remote && ./quartz --version"
adb shell "cd $remote && printf 'fn main() {\n    print(\"ok\")\n}\n' > hello.qz && ./quartz run hello.qz" | tr -d "\r" | grep -qx ok
for test in $tests; do
    adb shell "cd $remote && TMPDIR=$remote ./$test"
done
