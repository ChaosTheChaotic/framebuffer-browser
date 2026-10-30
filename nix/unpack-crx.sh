#!/usr/bin/env bash
# Usage: unpack-crx <file.crx|file.zip|directory> <outdir>
#
# Turns a Chrome Web Store package into an unpacked extension directory that
# QWebEngineExtensionManager::loadExtension() accepts. A .crx is a zip with a
# header in front, so the header is skipped and the rest unzipped.
#   CRX3: "Cr24" | version=3 | header_len (u32) | header | zip
#   CRX2: "Cr24" | version=2 | key_len (u32) | sig_len (u32) | key | sig | zip
# A file without the "Cr24" magic is treated as a plain zip. A directory is
# copied as is (so any flake input or store path can be passed in without
# knowing what it is).
set -euo pipefail
export LC_ALL=C

[ $# -eq 2 ] || {
	echo "usage: unpack-crx <file> <outdir>" >&2
	exit 2
}
src=$1
out=$2

if [ -d "$src" ]; then
	mkdir -p "$out"
	cp -R --no-preserve=mode,ownership "$src"/. "$out"/
	chmod -R u+w "$out"
	rm -rf "$out/_metadata"
	exit 0
fi

u32() { od -An -tu4 --endian=little -j"$2" -N4 "$1" | tr -d '[:space:]'; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

zip=$src
if [ "$(head -c4 "$src")" = "Cr24" ]; then
	case "$(u32 "$src" 4)" in
		3) off=$((12 + $(u32 "$src" 8))) ;;
		2) off=$((16 + $(u32 "$src" 8) + $(u32 "$src" 12))) ;;
		*)
			echo "unpack-crx: unsupported CRX version in $src" >&2
			exit 1
			;;
	esac
	zip=$tmp/ext.zip
	tail -c +$((off + 1)) "$src" >"$zip"
fi

mkdir -p "$out"
rc=0
unzip -q -o "$zip" -d "$out" || rc=$?
# unzip exits 1 for harmless warnings
[ "$rc" -le 1 ] || {
	echo "unpack-crx: unzip failed on $src" >&2
	exit 1
}

# Chromium reserves "_metadata" (the store's signature data) for itself
rm -rf "$out/_metadata"

[ -f "$out/manifest.json" ] || {
	echo "unpack-crx: no manifest.json at the top level of $src" >&2
	exit 1
}
