#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Moves the SDK to one version for a release: every component's own
# `version: "X"`, every kodediy/fck* dependency's `"==X"` in the manifests,
# and the tag the README's "Use it in your project" pins for kode-fck-sdk
# (`version: vX`). kode-sdk's own pin (kodediy/kode) is not this SDK's
# version and is left alone. The release workflow checks that the manifests
# equal the tag.
#
#   usage: tools/set_version.sh <X.Y.Z>
#   exit:  0 done, 1 a file did not take it, 2 not a version
#
set -euo pipefail
cd "$(dirname "$0")/.."

v="${1:-}"
if ! [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: tools/set_version.sh <X.Y.Z>" >&2
    exit 2
fi

for f in components/*/idf_component.yml; do
    sed -i.bak -E \
        -e "s/^version: \"[0-9.]+\"/version: \"$v\"/" \
        -e "/kodediy\/fck[a-z_]*:/ s/\"==[0-9.]+\"/\"==$v\"/" \
        "$f"
    rm -f "$f.bak"
    if ! grep -q "^version: \"$v\"$" "$f"; then
        echo "$f: no version line" >&2
        exit 1
    fi
done

# The README's dependency block: the version line after kode-fck-sdk's git and path.
perl -0pi -e "s{(kode-fck-sdk\\.git\\n[ \\t]*path:[^\\n]*\\n[ \\t]*version: )v[0-9.]+}{\${1}v$v}g" README.md
if ! grep -q "^    version: v$v\$" README.md; then
    echo "README.md: the kode-fck-sdk pin did not move" >&2
    exit 1
fi

tools/check_example_pins.sh
