#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ "$#" -lt 4 ]; then
	echo "usage: $0 OUTPUT UIB_ARCHIVE AR CC [STATIC_LINK_FLAGS...]" >&2
	exit 2
fi

output=$1
uib_archive=$2
archive_tool=$3
compiler=$4
shift 4

if [ ! -f "$uib_archive" ]; then
	echo "UIB archive not found: $uib_archive" >&2
	exit 1
fi

search_dirs=
libraries=
for option do
	case "$option" in
	-L*)
		directory=${option#-L}
		if [ -z "$directory" ]; then
			echo "empty static library search path" >&2
			exit 1
		fi
		search_dirs="$search_dirs $directory"
		;;
	-l*)
		name=${option#-l}
		library=
		for directory in $search_dirs; do
			if [ -f "$directory/lib$name.a" ]; then
				library=$directory/lib$name.a
				break
			fi
		done
		if [ -z "$library" ]; then
			library=$($compiler -print-file-name="lib$name.a")
		fi
		if [ "$library" = "lib$name.a" ] || [ ! -f "$library" ]; then
			echo "static library not found for -l$name" >&2
			exit 1
		fi
		case " $libraries " in
		*" $library "*) ;;
		*) libraries="$libraries $library" ;;
		esac
		;;
	*.a)
		if [ ! -f "$option" ]; then
			echo "static library not found: $option" >&2
			exit 1
		fi
		case " $libraries " in
		*" $option "*) ;;
		*) libraries="$libraries $option" ;;
		esac
		;;
	-pthread) ;;
	*)
		echo "unsupported static link flag: $option" >&2
		exit 1
		;;
	esac
done

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-static.XXXXXX")
archive=$work_dir/libuib-standalone.a
trap 'rm -rf -- "$work_dir"' EXIT INT TERM

{
	printf 'create %s\n' "$archive"
	printf 'addlib %s\n' "$uib_archive"
	for library in $libraries; do
		printf 'addlib %s\n' "$library"
	done
	printf 'save\nend\n'
} | "$archive_tool" -M

"$archive_tool" s "$archive"
mv -f -- "$archive" "$output"
