#!/bin/sh
# Host test of the receive-aggregation reorder buffer. It compiles the real code out of src/wlan/sta.c (the block
# between "#define BA_N" and the ADDBA response builder) against small stubs and feeds it randomly lost, delayed and
# wrapped-around sequence numbers. Needs only gcc:  sh tests/ba_reorder/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
awk '/^#define BA_N/{p=1} /ADDBA response \(action 1\)/{p=0} p' "$here/../../src/wlan/sta.c" > "$tmp/block.inc"
cp "$here/test.c" "$tmp/test.c"
gcc -w -I"$tmp" -o "$tmp/t" "$tmp/test.c"
"$tmp/t"
