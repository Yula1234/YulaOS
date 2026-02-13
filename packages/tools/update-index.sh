#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Generate repo.idx from built packages

set -e

PACKAGES_DIR="packages/build"
INDEX_FILE="packages/repo.idx"

if [ ! -d "$PACKAGES_DIR" ]; then
    echo "error: $PACKAGES_DIR not found"
    exit 1
fi

echo "# SPIN Package Index v1" > "$INDEX_FILE"
echo "# Format: name|version|size|sha256|deps|description" >> "$INDEX_FILE"

for spk in "$PACKAGES_DIR"/*.spk; do
    if [ ! -f "$spk" ]; then
        continue
    fi

    size=$(stat -c%s "$spk" 2>/dev/null || stat -f%z "$spk" 2>/dev/null || echo "0")

    name=$(dd if="$spk" bs=1 skip=8 count=64 2>/dev/null | tr -d '\0' | head -n1)
    version=$(dd if="$spk" bs=1 skip=72 count=16 2>/dev/null | tr -d '\0' | head -n1)
    desc=$(dd if="$spk" bs=1 skip=88 count=128 2>/dev/null | tr -d '\0' | head -n1)

    sha256sum=$(sha256sum "$spk" 2>/dev/null | awk '{print $1}' || echo "0000000000000000000000000000000000000000000000000000000000000000")

    deps=""

    if [ -n "$name" ] && [ -n "$version" ]; then
        echo "$name|$version|$size|$sha256sum|$deps|$desc" >> "$INDEX_FILE"
        echo "indexed: $name-$version ($size bytes)"
    fi
done

echo "repo.idx generated successfully"
