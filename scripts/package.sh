#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
version=$(cat VERSION)
out=$(realpath -m "${1:-dist}")
mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
install -Dm755 build/EFS.so "$stage/System/Filesystems/EFS.so"
install -Dm644 preview/Filesystem-EFS.pdf "$stage/System/Resources/Documentation/Filesystem-EFS.pdf"
mkdir -p "$stage/DEBIAN"
cat > "$stage/DEBIAN/control" <<EOF
Package: infiltrator-filesystem-efs
Version: $version
Architecture: amd64
Maintainer: Infiltrator Projects
Description: Native SGI Extent File System Common V2 module
EOF
dpkg-deb --build --root-owner-group "$stage" "$out/infiltrator-filesystem-efs_${version}_amd64.deb"
