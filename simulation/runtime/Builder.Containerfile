# SPDX-License-Identifier: Apache-2.0
ARG BASE_IMAGE=docker.io/library/ubuntu@sha256:\
2edbbc5dc405e9612ba3584ce95480277e3eb374407b5505fe26f17df77c7dbc
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
	binutils \
	build-essential \
	ca-certificates \
	curl \
	git \
	ibverbs-utils \
	iproute2 \
	libibverbs-dev \
	libmnl-dev \
	liburcu-dev \
	pkg-config \
	python3 \
	tar \
	zlib1g-dev \
	&& rm -rf /var/lib/apt/lists/*

ARG LIBMNL_VERSION=1.0.5
ARG LIBMNL_SHA256=274b9b919ef3152bfb3da3a13c950dd60d6e2bcd54230ffeca298d03b40d0525
RUN curl --fail --location --silent --show-error \
	-o /tmp/libmnl.tar.bz2 \
	"https://www.netfilter.org/projects/libmnl/files/libmnl-${LIBMNL_VERSION}.tar.bz2" \
	&& echo "${LIBMNL_SHA256}  /tmp/libmnl.tar.bz2" | sha256sum -c - \
	&& mkdir /tmp/libmnl \
	&& tar --extract --bzip2 --file=/tmp/libmnl.tar.bz2 \
		--directory=/tmp/libmnl --strip-components=1 \
	&& cd /tmp/libmnl \
	&& ./configure --prefix=/usr/local --disable-shared --enable-static \
	&& make -j"$(nproc)" \
	&& make install \
	&& install -D -m 644 COPYING \
		/usr/local/share/licenses/libmnl/COPYING \
	&& rm -rf /tmp/libmnl /tmp/libmnl.tar.bz2

ARG LIBURING_COMMIT=08468cc3830185c75f9e7edefd88aa01e5c2f8ab
ARG LIBURING_SHA256=1679f29b086a7c1cf217aff92bc192dae9f3ed3de82a70848b93dc82494c75b8
RUN curl --fail --location --silent --show-error \
	-o /tmp/liburing.tar.gz \
	"https://github.com/axboe/liburing/archive/${LIBURING_COMMIT}.tar.gz" \
	&& echo "${LIBURING_SHA256}  /tmp/liburing.tar.gz" | sha256sum -c - \
	&& mkdir /tmp/liburing \
	&& tar --extract --gzip --file=/tmp/liburing.tar.gz \
		--directory=/tmp/liburing --strip-components=1 \
	&& cd /tmp/liburing \
	&& ./configure --prefix=/usr/local \
	&& make -j"$(nproc)" \
	&& make install \
	&& install -D -m 644 COPYING \
		/usr/local/share/licenses/liburing/COPYING \
	&& rm -rf /tmp/liburing /tmp/liburing.tar.gz

ENV UIB_SOURCE_DEPENDENCIES='[{"name":"libmnl","version":"1.0.5","revision":"libmnl-1.0.5","source":"https://www.netfilter.org/projects/libmnl/files/libmnl-1.0.5.tar.bz2","sha256":"274b9b919ef3152bfb3da3a13c950dd60d6e2bcd54230ffeca298d03b40d0525","license":"/usr/local/share/licenses/libmnl/COPYING"},{"name":"liburing","version":"2.9","revision":"08468cc3830185c75f9e7edefd88aa01e5c2f8ab","source":"https://github.com/axboe/liburing/archive/08468cc3830185c75f9e7edefd88aa01e5c2f8ab.tar.gz","sha256":"1679f29b086a7c1cf217aff92bc192dae9f3ed3de82a70848b93dc82494c75b8","license":"/usr/local/share/licenses/liburing/COPYING"}]'

COPY simulation/runtime/build-portable-inside /usr/local/bin/

ENTRYPOINT ["/usr/local/bin/build-portable-inside"]
