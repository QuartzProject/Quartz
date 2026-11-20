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
if [ "$(uname -s)" = Darwin ]; then
    # Libraries Homebrew's LLVM pulls in (z3) travel inside the package, found next to what loads them.
    for file in "dist/$name"/lib/*.dylib; do
        for dep in $(otool -L "$file" | awk '/\/opt\/homebrew|\/usr\/local\/opt/ { print $1 }'); do
            base="$(basename "$dep")"
            if [ ! -f "dist/$name/lib/$base" ]; then
                cp "$dep" "dist/$name/lib/$base"
                chmod u+w "dist/$name/lib/$base"
                install_name_tool -id "@loader_path/$base" "dist/$name/lib/$base"
                codesign --force --sign - "dist/$name/lib/$base"
            fi
            install_name_tool -change "$dep" "@loader_path/$base" "$file"
        done
        codesign --force --sign - "$file"
    done
fi
cd dist
case "$target" in
Windows-*) zip -qr "$name.zip" "$name" ;;
*) tar -cJf "$name.tar.xz" "$name" ;;
esac
rm -rf "$name"
ls -l
