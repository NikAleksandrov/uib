#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ ! -r /etc/os-release ]; then
	echo "cannot identify the distribution" >&2
	exit 1
fi

# shellcheck source=/dev/null
. /etc/os-release

if [ "$(id -u)" -ne 0 ]; then
	echo "distribution dependency installation must run as root" >&2
	exit 1
fi

case "$ID:$VERSION_ID" in
ubuntu:24.04|ubuntu:26.04|debian:13)
	export DEBIAN_FRONTEND=noninteractive
	apt-get update
	apt-get install -y --no-install-recommends \
		autoconf \
		automake \
		binutils \
		build-essential \
		ca-certificates \
		dpkg-dev \
		git \
		gzip \
		ibverbs-utils \
		iproute2 \
		libbpf-dev \
		libelf-dev \
		libibverbs-dev \
		libmnl-dev \
		libtool \
		liburcu-dev \
		liburing-dev \
		libxdp-dev \
		pkg-config \
		python3 \
		tar \
		zlib1g-dev
	rm -rf /var/lib/apt/lists/*
	;;
fedora:44)
	dnf install -y --setopt=install_weak_deps=False \
		autoconf \
		automake \
		binutils \
		ca-certificates \
		elfutils-libelf-devel \
		findutils \
		gcc \
		git \
		gzip \
		iproute \
		libbpf-devel \
		libibverbs-utils \
		libmnl-devel \
		libtool \
		liburing-devel \
		libxdp-devel \
		make \
		pkgconf-pkg-config \
		python3 \
		rdma-core \
		rdma-core-devel \
		rpm-build \
		tar \
		userspace-rcu-devel \
		zlib-devel
	dnf clean all
	;;
alpine:3.24.*)
	apk add --no-cache \
		abuild \
		autoconf \
		automake \
		bash \
		binutils \
		build-base \
		ca-certificates \
		elfutils-dev \
		git \
		gzip \
		iproute2 \
		libbpf-dev \
		libmnl-dev \
		libtool \
		liburing-dev \
		libxdp-dev \
		linux-headers \
		pkgconf \
		python3 \
		rdma-core \
		rdma-core-dev \
		tar \
		userspace-rcu-dev \
		zlib-dev
	;;
*)
	echo "unsupported CI distribution: $ID $VERSION_ID" >&2
	exit 1
	;;
esac
