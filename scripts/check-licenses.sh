#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

check_header()
{
	file=$1
	expected=$2
	actual=$(sed -n '1p' "$file")
	test "$actual" = "$expected" || {
		echo "unexpected SPDX header in $file: $actual" >&2
		exit 1
	}
}

check_header src/bth.c '/* SPDX-License-Identifier: Apache-2.0 AND Linux-OpenIB */'
check_header src/pd.c '/* SPDX-License-Identifier: Apache-2.0 AND Unlicense */'

test -f LICENSES/Linux-OpenIB.txt
test -f LICENSES/Unlicense.txt
test -f LICENSES/README.md
grep -Fq 'src/bth.c' LICENSES/README.md
grep -Fq 'src/pd.c' LICENSES/README.md

echo 'license metadata: ok'
