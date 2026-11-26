#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Independent test oracle ONLY. Never used by the installed provider.
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL --retry 3 https://github.com/danifunker/rusty-backup/releases/download/v2026-11-26-08-56/rb-cli-linux-musl-x64-2026-11-26-08-56.tar.gz -o "$work/oracle.tar.gz"
printf '%s  %s\n' 5c5222b43bd630d908841eb6524d22dbb1fea5ce2c0b7dfc0951b840c444f640 "$work/oracle.tar.gz" | sha256sum -c -
mkdir "$work/oracle" "$work/input"
tar -xzf "$work/oracle.tar.gz" -C "$work/oracle"
cli=$(find "$work/oracle" -type f -name 'rb-cli' -print -quit)
test -n "$cli" && test -x "$cli"
mkdir "$work/input/nested"
dd if=/dev/urandom of="$work/input/nested/payload.bin" bs=65536 count=2 status=none
dd if=/dev/urandom of="$work/input/guard.bin" bs=1025 count=1 status=none
: > "$work/input/empty"
"$cli" new hd sgi-efs --size 32M --name ORACLE --from-dir "$work/input" "$work/foreign.img"
before=$(sha256sum "$work/foreign.img")
build/test_oracle "${1:-./build/EFS.so}" read "$work/foreign.img" "$work/input"
test "$before" = "$(sha256sum "$work/foreign.img")"
build/test_oracle "${1:-./build/EFS.so}" write "$work/provider.img" "$work/input"
before=$(sha256sum "$work/provider.img")
"$cli" fsck --checkonly --fs-type efs "$work/provider.img"
for file in nested/payload.bin guard.bin empty; do
  "$cli" get --fs-type efs "$work/provider.img" "/$file" "$work/out"
  cmp "$work/input/$file" "$work/out"
  rm "$work/out"
done
test "$before" = "$(sha256sum "$work/provider.img")"
echo 'EFS independent bidirectional format, fsck and byte interoperability PASS'
