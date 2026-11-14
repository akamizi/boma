#!/bin/bash
# Copia il template di Boma in una copia di void-packages e ne aggiorna versione e checksum.
# Uso: prepare.sh DIR_VOID_PACKAGES DIR_BOMA
set -euo pipefail
vp=$1 boma=$2
mkdir -p "$vp/srcpkgs/boma"
cp "$boma/packaging/void/srcpkgs/boma/template" "$vp/srcpkgs/boma/template"
ver=$(grep -m1 '^project(boma VERSION' "$boma/CMakeLists.txt" | sed 's/.*VERSION \([0-9.]*\).*/\1/')
sed -i "s/^version=.*/version=$ver/" "$vp/srcpkgs/boma/template"
url="https://github.com/akamizi/boma/archive/refs/tags/v$ver.tar.gz"
sum=$(curl -fsSL "$url" | sha256sum | cut -d' ' -f1)
sed -i "s/^checksum=.*/checksum=$sum/" "$vp/srcpkgs/boma/template"
