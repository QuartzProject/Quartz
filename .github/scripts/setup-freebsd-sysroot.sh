#!/bin/sh
# Usage: setup-freebsd-sysroot.sh <directory>
# A FreeBSD 14.5 arm64 sysroot: the base system, then LLVM 23, zstd and libxml2 from the package repository.
set -eu

root="${1:?directory}"
[ ! -f "$root/.complete" ] || exit 0
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch() {
    curl -fLsS --retry 5 --retry-all-errors --retry-delay 10 --connect-timeout 20 --max-time 1800 "$1" -o "$2"
}

base=https://download.freebsd.org/releases/arm64/aarch64/14.5-RELEASE
fetch "$base/base.txz" "$work/base.txz"
fetch "$base/MANIFEST" "$work/MANIFEST"
digest="$(awk '$1 == "base.txz" { print $2 }' "$work/MANIFEST")"
printf '%s  %s\n' "$digest" "$work/base.txz" | sha256sum -c -
mkdir -p "$root"
tar -xJf "$work/base.txz" -C "$root" ./usr/include ./usr/lib ./lib

packages=https://pkg.freebsd.org/FreeBSD:14:aarch64/latest
fetch "$packages/packagesite.pkg" "$work/packagesite.pkg"
tar -xf "$work/packagesite.pkg" -C "$work" packagesite.yaml
for name in llvm23 zstd libxml2; do
    entry="$(jq -r --arg name "$name" 'select(.name == $name) | "\(.path) \(.pkgsize)"' "$work/packagesite.yaml")"
    path="${entry% *}"
    echo "$name: $path"
    fetch "$packages/$path" "$work/$name.pkg"
    [ "$(stat -c %s "$work/$name.pkg")" = "${entry#* }" ]
    tar -xf "$work/$name.pkg" -C "$root" --exclude='+*' 2>/dev/null
    rm "$work/$name.pkg"
done
touch "$root/.complete"
