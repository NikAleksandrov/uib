#!/bin/sh
# SPDX-License-Identifier: Apache-2.0

last=
for arg in "$@"; do
	last=$arg
done
if [ "$last" != 127.0.0.1 ]; then
	sleep 2
fi

printf '%s\n' '---------------------------------------------------------------------------------------'
printf '%s\n' ' #bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]'
printf '%s\n' ' 4096       8              12.50              12.00                0.183'
exit 0
