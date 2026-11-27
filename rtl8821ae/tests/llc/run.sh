#!/bin/sh
# Host test of the Ethernet <-> 802.11 LLC conversion (RFC 1042, bridge tunnel, IEEE 802.3 / NetBEUI frames). It
# compiles the real functions out of src/wlan/sta.c (between the LLC_BEGIN and LLC_END markers):  sh tests/llc/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
awk '/LLC_BEGIN/{p=1;next} /LLC_END/{p=0} p' "$here/../../src/wlan/sta.c" > "$tmp/block.inc"
cp "$here/test.c" "$tmp/test.c"
gcc -w -I"$tmp" -o "$tmp/t" "$tmp/test.c"
"$tmp/t"
