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
check_header src/cq.c '/* SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause-Views */'
check_header src/ptr_ring.h '/* SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause-Views */'
check_header bpf/uib_afxdp_kern.c '// SPDX-License-Identifier: GPL-2.0-only'

test -f LICENSES/Linux-OpenIB.txt
test -f LICENSES/Unlicense.txt
test -f LICENSES/BSD-2-Clause-Views-Dmitry-Vyukov.txt
test -f LICENSES/GPL-2.0-only.txt
test -f LICENSES/README.md
grep -Fq 'src/bth.c' LICENSES/README.md
grep -Fq 'src/pd.c' LICENSES/README.md
grep -Fq 'src/cq.c' LICENSES/README.md
grep -Fq 'src/ptr_ring.h' LICENSES/README.md
grep -Fq 'bpf/uib_afxdp_kern.c' LICENSES/README.md

echo 'license metadata: ok'
