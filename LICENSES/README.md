<!-- SPDX-License-Identifier: Apache-2.0 -->

# License map

UIB's original code is licensed under Apache-2.0. The source distribution is
a mixed-license aggregate containing the following identified exceptions:

| Scope | License |
|---|---|
| Project files not listed below | `Apache-2.0` |
| `src/bth.c` | `Apache-2.0 AND Linux-OpenIB` |
| `lowbias32` mixer in `src/pd.c` | `Apache-2.0 AND Unlicense` |
| `src/cq.c`, `src/ptr_ring.h` | `Apache-2.0 AND BSD-2-Clause-Views` |
| `bpf/uib_afxdp_kern.c` | `GPL-2.0-only` |
| Generated `configure` | GNU Autoconf embedded permission notice |

`AND` means that both licenses apply to their respective portions of the
file. Copyright and attribution details are recorded in
[`../NOTICE`](../NOTICE), and the complete additional license texts are in
this directory.

The optional GPL-2.0-only AF_XDP redirect program is built as a separate eBPF
object. It is not linked into or installed with the Apache-2.0 libraries.
