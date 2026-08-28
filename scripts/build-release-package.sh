#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kind=${1:-}
variant=${2:-}
output_dir=${3:-$project_dir/dist}
version=$(sed -n 's/^AC_INIT(\[uib\], \[\([^]]*\)\].*/\1/p' \
	"$project_dir/configure.ac")
commit=$(git -C "$project_dir" rev-parse HEAD)
source_epoch=$(git -C "$project_dir" show -s --format=%ct HEAD)

case "$version" in
''|*[!0-9A-Za-z.+~-]*)
	echo "cannot determine a package-safe version from configure.ac" >&2
	exit 1
	;;
esac

case "$kind" in
deb|rpm|apk|static)
	;;
*)
	echo "usage: $0 deb|rpm|apk|static VARIANT [OUTPUT-DIRECTORY]" >&2
	exit 2
	;;
esac

mkdir -p "$output_dir"
output_dir=$(CDPATH='' cd -- "$output_dir" && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/uib-package.XXXXXX")
trap 'rm -rf -- "$work_dir"' EXIT INT TERM

write_build_info()
{
	local_root=$1
	{
		printf 'uib-version=%s\n' "$version"
		printf 'uib-commit=%s\n' "$commit"
		printf 'package-kind=%s\n' "$kind"
		printf 'package-variant=%s\n' "$variant"
		printf 'source-date-epoch=%s\n' "$source_epoch"
		cat /etc/os-release
		printf 'compiler=%s\n' "$(cc --version | sed -n '1p')"
		for module in libibverbs liburing libmnl liburcu-bp zlib \
			libxdp libbpf; do
			module_version=$(pkg-config --modversion "$module" \
				2>/dev/null || printf 'not-installed')
			printf '%s=%s\n' "$module" "$module_version"
		done
	} > "$local_root/BUILD-INFO"
}

stage_dynamic_tree()
{
	root=$1
	lib_dir=$2
	mkdir -p "$root$lib_dir" "$root/usr/include" \
		"$root/usr/share/doc/uib/LICENSES" \
		"$root/usr/share/man/man7"
	install -m 0644 "$project_dir/libuib.a" "$root$lib_dir/libuib.a"
	install -m 0755 "$project_dir/libuib.so.0.1.0" \
		"$root$lib_dir/libuib.so.0.1.0"
	ln -s libuib.so.0.1.0 "$root$lib_dir/libuib.so.0"
	ln -s libuib.so.0 "$root$lib_dir/libuib.so"
	install -m 0755 "$project_dir/libuibv.so" "$root$lib_dir/libuibv.so"
	cp -a "$project_dir/include/uib" "$root/usr/include/"
	chmod 0644 "$root/usr/include/uib/"*.h
	install -m 0644 "$project_dir/man/uib.7" \
		"$root/usr/share/man/man7/uib.7"
	for name in README.md LICENSE NOTICE AUTHORS; do
		install -m 0644 "$project_dir/$name" \
			"$root/usr/share/doc/uib/$name"
	done
	install -m 0644 "$project_dir"/LICENSES/*.txt \
		"$root/usr/share/doc/uib/LICENSES/"
	install -m 0644 "$project_dir/LICENSES/README.md" \
		"$root/usr/share/doc/uib/LICENSES/README.md"
	write_build_info "$root/usr/share/doc/uib"
	if [ -f "$project_dir/distribution-compatibility.txt" ]; then
		install -m 0644 "$project_dir/distribution-compatibility.txt" \
			"$root/usr/share/doc/uib/distribution-compatibility.txt"
	fi
}

build_deb()
{
	case "$variant" in
	ubuntu-24.04)
		release="1~ubuntu24.04"
		;;
	ubuntu-26.04)
		release="1~ubuntu26.04"
		;;
	debian-13)
		release="1~debian13"
		;;
	*)
		echo "unsupported deb variant: $variant" >&2
		exit 2
		;;
	esac

	architecture=$(dpkg --print-architecture)
	multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
	package_root=$work_dir/uib
	library_dir=/usr/lib/$multiarch
	stage_dynamic_tree "$package_root" "$library_dir"
	mkdir -p "$package_root/DEBIAN"
	mkdir -p "$work_dir/debian"
	cat > "$work_dir/debian/control" <<EOF
Source: uib
Section: libs
Priority: optional
Maintainer: Nikolay Aleksandrov <razor@blackwall.org>
Standards-Version: 4.7.0

Package: uib
Architecture: any
Description: userspace RDMA protocol library and libibverbs provider
EOF
	dependencies=$(cd "$work_dir" && dpkg-shlibdeps -O \
		-l"$package_root$library_dir" \
		-e"$package_root$library_dir/libuib.so" \
		-e"$package_root$library_dir/libuibv.so" | \
		sed -n 's/^shlibs:Depends=//p')
	[ -n "$dependencies" ] || {
		echo "dpkg-shlibdeps produced no dependencies" >&2
		exit 1
	}
	installed_size=$(du -sk "$package_root" | awk '{ print $1 }')
	cat > "$package_root/DEBIAN/control" <<EOF
Package: uib
Version: $version-$release
Section: libs
Priority: optional
Architecture: $architecture
Maintainer: Nikolay Aleksandrov <razor@blackwall.org>
Installed-Size: $installed_size
Depends: $dependencies
Description: userspace RDMA protocol library and libibverbs provider
 UIB implements RDMA wire protocols in userspace and supplies libuibv.so for
 transparent execution of dynamically linked libibverbs applications.
EOF
	package=$output_dir/uib_${version}-${release}_${variant}_${architecture}.deb
	dpkg-deb --root-owner-group --build "$package_root" "$package"
	dpkg-deb --info "$package"
	dpkg-deb --contents "$package"
}

build_rpm()
{
	[ "$variant" = fedora-44 ] || {
		echo "unsupported rpm variant: $variant" >&2
		exit 2
	}

	top=$work_dir/rpmbuild
	stage=$work_dir/root
	changelog_date=$(date -u -d "@$source_epoch" '+%a %b %d %Y')
	mkdir -p "$top/BUILD" "$top/BUILDROOT" "$top/RPMS" \
		"$top/SOURCES" "$top/SPECS" "$top/SRPMS"
	stage_dynamic_tree "$stage" /usr/lib64
	tar -C "$stage" -czf "$top/SOURCES/uib-root.tar.gz" .
	cat > "$top/SPECS/uib.spec" <<EOF
Name:           uib
Version:        $version
Release:        1%{?dist}
Summary:        Userspace RDMA protocol library and libibverbs provider
License:        Apache-2.0 AND Linux-OpenIB AND BSD-2-Clause-Views AND Unlicense
URL:            https://github.com/NikAleksandrov/uib
Packager:       Nikolay Aleksandrov <razor@blackwall.org>
Source0:        uib-root.tar.gz

%description
UIB implements RDMA wire protocols in userspace and supplies libuibv.so for
transparent execution of dynamically linked libibverbs applications.

%prep

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -xzf %{SOURCE0} -C %{buildroot}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license /usr/share/doc/uib/LICENSE
%license /usr/share/doc/uib/LICENSES/
%doc /usr/share/doc/uib/AUTHORS
%doc /usr/share/doc/uib/BUILD-INFO
%doc /usr/share/doc/uib/NOTICE
%doc /usr/share/doc/uib/README.md
%doc /usr/share/doc/uib/distribution-compatibility.txt
/usr/include/uib/
/usr/lib64/libuib.a
/usr/lib64/libuib.so*
/usr/lib64/libuibv.so
/usr/share/man/man7/uib.7*

%changelog
* $changelog_date Nikolay Aleksandrov <razor@blackwall.org> - $version-1
- Automated release build from $commit
EOF
	rpmbuild --define "_topdir $top" -bb "$top/SPECS/uib.spec"
	package=$(find "$top/RPMS" -type f -name '*.rpm' -print -quit)
	[ -n "$package" ] || {
		echo "rpmbuild did not produce a package" >&2
		exit 1
	}
	output=$output_dir/$(basename "$package")
	cp "$package" "$output"
	rpm -qip "$output"
	rpm -qlp "$output"
}

build_apk()
{
	[ "$variant" = alpine-3.24 ] || {
		echo "unsupported apk variant: $variant" >&2
		exit 2
	}

	build_user=uib-build
	if ! id "$build_user" >/dev/null 2>&1; then
		adduser -D "$build_user"
		addgroup "$build_user" abuild
	fi
	start_dir=$work_dir/apk
	mkdir -p "$start_dir"
	git -C "$project_dir" archive --format=tar \
		--prefix="uib-$version/" HEAD | \
		gzip -n > "$start_dir/uib-$version.tar.gz"
	checksum=$(sha512sum "$start_dir/uib-$version.tar.gz" | \
		awk '{ print $1 }')
	cat > "$start_dir/APKBUILD" <<EOF
# Maintainer: Nikolay Aleksandrov <razor@blackwall.org>
pkgname=uib
pkgver=$version
pkgrel=0
pkgdesc="Userspace RDMA protocol library and libibverbs provider"
url="https://github.com/NikAleksandrov/uib"
arch="x86_64"
license="Apache-2.0 AND Linux-OpenIB AND BSD-2-Clause-Views AND Unlicense"
subpackages="\$pkgname-doc"
makedepends="autoconf automake build-base libbpf-dev libmnl-dev libtool liburing-dev libxdp-dev linux-headers pkgconf rdma-core-dev userspace-rcu-dev zlib-dev"
source="uib-$version.tar.gz"
sha512sums="$checksum  uib-$version.tar.gz"

build() {
	./configure --prefix=/usr --enable-provider --enable-af-xdp
	make
}

check() {
	make test
	make provider-cq-event-test
	make provider-nccl-abi-test
}

package() {
	make DESTDIR="\$pkgdir" install
}
EOF
	chown -R "$build_user:$build_user" "$work_dir"
	su - "$build_user" -c "abuild-keygen -a -n"
	find "/home/$build_user/.abuild" -type f -name '*.rsa.pub' \
		-exec cp {} /etc/apk/keys/ \;
	su - "$build_user" -c "cd '$start_dir' && abuild -r"
	find "/home/$build_user/packages" -type f -name 'uib-*.apk' \
		-exec cp {} "$output_dir/" \;
	find "/home/$build_user/.abuild" -type f -name '*.rsa.pub' \
		-exec cp {} "$output_dir/" \;
	packages=$(find "$output_dir" -maxdepth 1 -type f -name 'uib-*.apk' -print)
	[ -n "$packages" ] || {
		echo "abuild did not produce an APK package" >&2
		exit 1
	}
	for package in $packages; do
		apk add --simulate --allow-untrusted "$package" >/dev/null
	done
}

build_static()
{
	[ "$variant" = static-linux-amd64 ] || {
		echo "unsupported static variant: $variant" >&2
		exit 2
	}
	runtime=${RUNTIME_ARTIFACT_DIR:-$project_dir/simulation/.artifacts}/uib-runtime
	test -f "$runtime/lib/libuib-standalone.a"
	root_name=uib-$version-static-linux-amd64
	root=$work_dir/$root_name
	mkdir -p "$root/lib" "$root/include" "$root/share"
	install -m 0644 "$runtime/lib/libuib-standalone.a" "$root/lib/"
	cp -a "$project_dir/include/uib" "$root/include/"
	chmod 0644 "$root/include/uib/"*.h
	cp -a "$runtime/share/uib/licenses" "$root/share/"
	install -m 0644 "$runtime/share/uib/runtime-manifest.json" "$root/"
	install -m 0644 "$project_dir/README.md" "$root/"
	write_build_info "$root"
	archive=$output_dir/$root_name.tar.gz
	tar --sort=name --mtime="@$source_epoch" --owner=0 --group=0 \
		--numeric-owner -C "$work_dir" -cf - "$root_name" | \
		gzip -n > "$archive"
	tar -tzf "$archive" >/dev/null
}

case "$kind" in
deb)
	build_deb
	;;
rpm)
	build_rpm
	;;
apk)
	build_apk
	;;
static)
	build_static
	;;
esac
