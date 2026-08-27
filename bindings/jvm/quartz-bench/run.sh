#!/bin/sh
# Usage: run.sh <Quartz checkout> <libquartz.so> [cpu] [runs]
set -eu
src=$1
lib=$2
cpu=${3:-3}
runs=${4:-7}
here=$(cd "$(dirname "$0")" && pwd)
out=$here/build/c
mkdir -p "$out"
clang -O2 -std=c11 -ffp-contract=off -I"$src/include" "$here/src/main/c/bench.c" -o "$out/bench" \
    -L"$(dirname "$lib")" -lquartz -Wl,-rpath,"$(dirname "$lib")"
(cd "$here/.." && ./gradlew -q :quartz-bench:installDist)
echo "load: $(cut -d' ' -f1-3 /proc/loadavg)"
i=0
while [ "$i" -lt "$runs" ]; do
    taskset -c "$cpu" nice -n 10 "$out/bench"
    JAVA_OPTS="-Dquartz.library=$lib" taskset -c "$cpu" nice -n 10 "$here/build/install/quartz-bench/bin/quartz-bench"
    i=$((i + 1))
done
echo "load: $(cut -d' ' -f1-3 /proc/loadavg)"
