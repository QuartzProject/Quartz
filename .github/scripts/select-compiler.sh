#!/usr/bin/env bash
# Installs Clang and LLVM 23 and writes QUARTZ_CMAKE_ARGS for a build whose binaries carry their C++ runtime.
set -euo pipefail

retry() {
    local tries="$1" attempt=1
    shift
    until "$@"; do
        if [ "$attempt" -ge "$tries" ]; then
            echo "giving up on: $*" >&2
            return 1
        fi
        sleep $((attempt * 15))
        attempt=$((attempt + 1))
    done
}

case "$(uname -s)" in
Linux)
    # Ubuntu 24.04's libstdc++ 14 lacks parts of C++23 (std::format's "{:?}"); Clang picks the newest GCC it finds.
    retry 4 sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
    retry 4 sudo apt-get install -y --no-install-recommends g++-15
    retry 4 wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    retry 4 sudo bash /tmp/llvm.sh 23
    retry 4 sudo apt-get install -y --no-install-recommends ninja-build llvm-23-dev libzstd-dev zlib1g-dev
    static="-static-libstdc++ -static-libgcc"
    echo "QUARTZ_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 -DQUARTZ_LLVM_CONFIG=/usr/bin/llvm-config-23 \"-DCMAKE_EXE_LINKER_FLAGS=$static\" \"-DCMAKE_SHARED_LINKER_FLAGS=$static\"" >>"$GITHUB_ENV"
    ;;
Darwin)
    if [ "${QUARTZ_TARGET_ARCH:-}" = x86_64 ] && [ "$(uname -m)" = arm64 ]; then
        # Homebrew has no x86_64 LLVM for an arm64 host: Apple's Clang and libc++ build the interpreter, run by Rosetta.
        sudo softwareupdate --install-rosetta --agree-to-license || true
        xcode="$(find /Applications -maxdepth 1 -name 'Xcode_*.app' | sort -V | tail -1)"
        [ -z "$xcode" ] || sudo xcode-select -s "$xcode"
        clang --version
        retry 4 brew install ninja
        echo "QUARTZ_CMAKE_ARGS=-DCMAKE_C_COMPILER=$(xcrun -f clang) -DCMAKE_CXX_COMPILER=$(xcrun -f clang++) -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3" >>"$GITHUB_ENV"
        exit 0
    fi
    retry 4 brew install llvm ninja
    llvm="$(brew --prefix llvm)"
    "$llvm/bin/llvm-config" --version | grep -q '^23\.' || { echo "Homebrew's llvm is not LLVM 23" >&2; exit 1; }
    # Homebrew's libc++ is newer than the system one: link its static archive so the binaries need only the system.
    cxx="$llvm/lib/c++/libc++.a"
    # LLVM's static libraries ask for -lzstd: a folder holding only the archive keeps Homebrew's dylib out.
    mkdir -p "$HOME/quartz-static"
    cp "$(brew --prefix zstd)/lib/libzstd.a" "$HOME/quartz-static/"
    static="-L$HOME/quartz-static"
    echo "QUARTZ_CMAKE_ARGS=-DCMAKE_C_COMPILER=$llvm/bin/clang -DCMAKE_CXX_COMPILER=$llvm/bin/clang++ -DQUARTZ_LLVM_CONFIG=$llvm/bin/llvm-config -DCMAKE_EXE_LINKER_FLAGS=$static -DCMAKE_SHARED_LINKER_FLAGS=$static \"-DCMAKE_CXX_STANDARD_LIBRARIES=-lc++ $cxx\" -DCMAKE_CXX_FLAGS=-D_LIBCPP_DISABLE_AVAILABILITY" >>"$GITHUB_ENV"
    ;;
MINGW* | MSYS*)
    echo "QUARTZ_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-static -DCMAKE_SHARED_LINKER_FLAGS=-static" >>"$GITHUB_ENV"
    ;;
*)
    echo "QUARTZ_CMAKE_ARGS=" >>"$GITHUB_ENV"
    ;;
esac
