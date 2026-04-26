#!/bin/sh
set -eu

shared=libuib.so
soname=libuib.so.0
real=libuib.so.0.1.0
map=src/libuib.map

test "$(readlink "$shared")" = "$soname"
test "$(readlink "$soname")" = "$real"
readelf -d "$real" | grep -F '(SONAME)' | grep -F "[$soname]" >/dev/null

expected=$(mktemp "${TMPDIR:-/tmp}/uib-abi-expected.XXXXXX")
actual=$(mktemp "${TMPDIR:-/tmp}/uib-abi-actual.XXXXXX")
trap 'rm -f -- "$expected" "$actual"' EXIT INT TERM
awk '
	/^[[:space:]]*global:/ { globals = 1; next }
	/^[[:space:]]*local:/ { globals = 0 }
	globals && /^[[:space:]]*uibv?_[A-Za-z0-9_]+;/ {
		gsub(/^[[:space:]]*|;.*/, "")
		print
	}
' "$map" | sort -u > "$expected"
nm -D --defined-only --format=posix "$real" | awk '
	$1 ~ /^uibv?_[A-Za-z0-9_]+(@@UIB(_PRIVATE)?_0\.1)?$/ {
		sub(/@@UIB(_PRIVATE)?_0\.1$/, "", $1)
		print $1
	}
' | sort -u > "$actual"
diff -u "$expected" "$actual"

echo 'shared library ABI: ok'
