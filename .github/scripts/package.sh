#!/usr/bin/env bash
# Usage: package.sh <version> <target> <build dir>
# Installs the build into quartz-<version>-<target> and archives it in dist/ (.zip on Windows, .tar.xz elsewhere).
set -euo pipefail

version="${1:?version}"
target="${2:?target}"
build="${3:?build dir}"
name="quartz-$version-$target"

rm -rf "dist/$name"
cmake --install "$build" --prefix "dist/$name" --strip
cp LICENSE "dist/$name/"
cd dist
case "$target" in
Windows-*) zip -qr "$name.zip" "$name" ;;
*) tar -cJf "$name.tar.xz" "$name" ;;
esac
rm -rf "$name"
ls -l
