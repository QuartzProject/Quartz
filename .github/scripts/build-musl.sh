#!/bin/sh
# Usage: build-musl.sh <version> <target>, inside an Alpine container at the source root.
# Builds, tests and packages Quartz; the binaries need only musl itself.
set -eu

version="${1:?version}"
target="${2:?target}"

n=0
until apk add --no-cache clang23 lld23 llvm23-dev llvm23-static g++ musl-dev linux-headers cmake ninja ccache bash \
    file binutils xz zstd-dev zstd-static zlib-dev zlib-static libxml2-dev libxml2-static; do
    n=$((n + 1))
    [ "$n" -lt 4 ] || exit 1
    sleep $((n * 15))
done

# LLVM asks for -lz and -lzstd: a folder holding only their archives keeps the shared ones out of the package.
mkdir -p /tmp/quartz-static
cp /usr/lib/libz.a /usr/lib/libzstd.a /tmp/quartz-static/
link="-L/tmp/quartz-static -static-libstdc++ -static-libgcc"

ccache --zero-stats
cmake --preset release -DCMAKE_BUILD_TYPE=Release "-DQUARTZ_VERSION=$version" -DQUARTZ_SHARED=ON -DQUARTZ_COMPILER=ON \
    -DQUARTZ_JVM=OFF -DQUARTZ_ANDROID=OFF -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_C_COMPILER=clang-23 \
    -DCMAKE_CXX_COMPILER=clang++-23 -DQUARTZ_LLVM_CONFIG=/usr/bin/llvm-config-23 \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    "-DCMAKE_EXE_LINKER_FLAGS=$link" "-DCMAKE_SHARED_LINKER_FLAGS=$link"
cmake --build build/release
ctest --test-dir build/release --output-on-failure
ccache --show-stats

for binary in build/release/cli/quartz build/release/lsp/quartz-lsp build/release/src/libquartz.so \
    build/release/compiler/libquartz_compiler.so; do
    file -L "$binary"
    if readelf -d "$binary" | grep NEEDED | grep -v -e libc.musl -e 'libquartz\.so'; then
        echo "$binary needs more than musl"
        exit 1
    fi
done

bash .github/scripts/package.sh "$version" "$target" build/release
