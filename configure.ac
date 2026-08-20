AC_PREREQ([2.69])
AC_INIT([uib], [0.1], [razor@blackwall.org])
AC_CONFIG_SRCDIR([src/context.c])
AC_CONFIG_FILES([config.mk])
AC_PROG_CC
AC_CHECK_TOOL([AR], [ar])
AC_PATH_PROG([INSTALL], [install], [install])
AC_PATH_TOOL([PKG_CONFIG], [pkg-config], [no])

AC_ARG_ENABLE([provider],
	AS_HELP_STRING([--disable-provider],
		[do not build the transparent libibverbs provider]), [],
	[enable_provider=auto])
AC_ARG_ENABLE([af-xdp],
	AS_HELP_STRING([--disable-af-xdp], [do not build AF_XDP support]), [],
	[enable_af_xdp=auto])
AC_ARG_ENABLE([devmem],
	AS_HELP_STRING([--disable-devmem], [do not build TCP devmem support]), [],
	[enable_devmem=auto])
AC_ARG_ENABLE([dmabuf],
	AS_HELP_STRING([--disable-dmabuf],
		[do not build DMA-BUF memory registration support]), [],
	[enable_dmabuf=auto])
AC_ARG_ENABLE([zerocopy],
	AS_HELP_STRING([--disable-zerocopy],
		[do not build MSG_ZEROCOPY socket support]), [],
	[enable_zerocopy=auto])
AC_ARG_WITH([liburing],
	AS_HELP_STRING([--with-liburing=PREFIX],
		[find liburing below PREFIX instead of probing]), [],
	[with_liburing=auto])

URING_CFLAGS=
URING_LIBS=
AS_IF([test "x$with_liburing" != xauto], [
	URING_CFLAGS="-I$with_liburing/include"
	URING_LIBS="-L$with_liburing/lib -luring"
], [test "$PKG_CONFIG" != no && $PKG_CONFIG --exists liburing], [
	URING_CFLAGS=`$PKG_CONFIG --cflags liburing`
	URING_LIBS=`$PKG_CONFIG --libs liburing`
], [test -f "$srcdir/../liburing/src/include/liburing.h"], [
	URING_CFLAGS="-I$srcdir/../liburing/src/include"
])
saved_CPPFLAGS=$CPPFLAGS
CPPFLAGS="$CPPFLAGS $URING_CFLAGS"
AC_CHECK_HEADER([liburing.h], [],
	[AC_MSG_ERROR([liburing headers are required; use --with-liburing=PREFIX])])
CPPFLAGS=$saved_CPPFLAGS
AS_IF([test -z "$URING_LIBS"], [
	saved_LIBS=$LIBS
	AC_SEARCH_LIBS([io_uring_queue_init], [uring],
		[URING_LIBS="$ac_cv_search_io_uring_queue_init"], [
		LIBS="-l:liburing.so.2 $saved_LIBS"
		CPPFLAGS="$CPPFLAGS $URING_CFLAGS"
		AC_LINK_IFELSE([AC_LANG_PROGRAM(
			[[#include <liburing.h>]],
			[[struct io_uring ring; io_uring_queue_init(2, &ring, 0);]])],
			[URING_LIBS="-l:liburing.so.2"],
			[AC_MSG_ERROR([liburing library is required])])
		CPPFLAGS=$saved_CPPFLAGS
	])
	LIBS=$saved_LIBS
])
saved_LIBS=$LIBS
saved_CPPFLAGS=$CPPFLAGS
LIBS="$URING_LIBS $LIBS"
CPPFLAGS="$CPPFLAGS $URING_CFLAGS"
AC_LINK_IFELSE([AC_LANG_PROGRAM(
	[[#include <liburing.h>]],
	[[struct io_uring ring;
	  struct msghdr message = { 0 };
	  struct io_uring_sqe sqe;
	  int error;
	  io_uring_prep_recvmsg_multishot(&sqe, -1, &message, 0);
	  io_uring_setup_buf_ring(&ring, 8, 1, 0, &error);]])], [],
	[AC_MSG_ERROR([liburing with multishot recvmsg and provided buffer rings is required])])
LIBS=$saved_LIBS
CPPFLAGS=$saved_CPPFLAGS

HAVE_IBVERBS=0
IBVERBS_CFLAGS=
IBVERBS_LIBS=
AS_IF([test "$PKG_CONFIG" != no && $PKG_CONFIG --exists libibverbs], [
	IBVERBS_CFLAGS=`$PKG_CONFIG --cflags libibverbs`
	IBVERBS_LIBS=`$PKG_CONFIG --libs libibverbs`
	HAVE_IBVERBS=1
], [
	AC_CHECK_HEADER([infiniband/verbs.h], [
		AC_CHECK_LIB([ibverbs], [ibv_get_device_list], [
			IBVERBS_LIBS=-libverbs
			HAVE_IBVERBS=1
		])
	])
])
AS_IF([test "x$enable_provider" = xyes && test "$HAVE_IBVERBS" != 1],
	[AC_MSG_ERROR([--enable-provider requires libibverbs development files])])
AS_IF([test "x$enable_provider" = xno], [ENABLE_PROVIDER=0],
	[ENABLE_PROVIDER=$HAVE_IBVERBS])

HAVE_AFXDP=0
AFXDP_CFLAGS=
AFXDP_LIBS=
AS_IF([test "x$enable_af_xdp" != xno && test "$PKG_CONFIG" != no &&
	$PKG_CONFIG --exists libxdp libbpf], [
	HAVE_AFXDP=1
	AFXDP_CFLAGS=`$PKG_CONFIG --cflags libxdp libbpf`
	AFXDP_LIBS=`$PKG_CONFIG --libs libxdp libbpf`
])
AS_IF([test "x$enable_af_xdp" = xyes && test "$HAVE_AFXDP" != 1],
	[AC_MSG_ERROR([--enable-af-xdp requires libxdp and libbpf])])

HAVE_DEVMEM=0
AS_IF([test "x$enable_devmem" != xno], [
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#define _GNU_SOURCE
#include <sys/socket.h>]], [[int option = SO_DEVMEM_DMABUF; return option;]])],
		[HAVE_DEVMEM=1])
])
AS_IF([test "x$enable_devmem" = xyes && test "$HAVE_DEVMEM" != 1],
	[AC_MSG_ERROR([--enable-devmem requires a kernel UAPI with SO_DEVMEM_DMABUF])])

HAVE_DMABUF=0
AS_IF([test "x$enable_dmabuf" != xno], [
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <linux/dma-buf.h>
#include <sys/ioctl.h>]],
		[[struct dma_buf_sync sync = { 0 };
		  return ioctl(-1, DMA_BUF_IOCTL_SYNC, &sync);]])],
		[HAVE_DMABUF=1])
])
AS_IF([test "x$enable_dmabuf" = xyes && test "$HAVE_DMABUF" != 1],
	[AC_MSG_ERROR([--enable-dmabuf requires the Linux dma-buf UAPI])])

HAVE_ZEROCOPY=0
AS_IF([test "x$enable_zerocopy" != xno], [
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#define _GNU_SOURCE
#include <sys/socket.h>
#include <linux/errqueue.h>]],
		[[int flags = MSG_ZEROCOPY;
		  int option = SO_ZEROCOPY;
		  return flags + option + SO_EE_ORIGIN_ZEROCOPY;]])],
		[HAVE_ZEROCOPY=1])
])
AS_IF([test "x$enable_zerocopy" = xyes && test "$HAVE_ZEROCOPY" != 1],
	[AC_MSG_ERROR([--enable-zerocopy requires Linux MSG_ZEROCOPY support])])

AC_PATH_PROG([VNG], [vng], [no])
AC_PATH_PROG([IBV_RC_PINGPONG], [ibv_rc_pingpong], [no])
HAVE_VIRTME=0
HAVE_IBV_TOOLS=0
AS_IF([test "$VNG" != no], [HAVE_VIRTME=1])
AS_IF([test "$IBV_RC_PINGPONG" != no], [HAVE_IBV_TOOLS=1])

AC_SUBST([URING_CFLAGS])
AC_SUBST([URING_LIBS])
AC_SUBST([HAVE_IBVERBS])
AC_SUBST([ENABLE_PROVIDER])
AC_SUBST([IBVERBS_CFLAGS])
AC_SUBST([IBVERBS_LIBS])
AC_SUBST([HAVE_AFXDP])
AC_SUBST([AFXDP_CFLAGS])
AC_SUBST([AFXDP_LIBS])
AC_SUBST([HAVE_DEVMEM])
AC_SUBST([HAVE_DMABUF])
AC_SUBST([HAVE_ZEROCOPY])
AC_SUBST([HAVE_VIRTME])
AC_SUBST([HAVE_IBV_TOOLS])

AC_MSG_NOTICE([configuration summary:])
AC_MSG_NOTICE([  libibverbs provider: $ENABLE_PROVIDER])
AC_MSG_NOTICE([  AF_XDP transport:    $HAVE_AFXDP])
AC_MSG_NOTICE([  TCP devmem UAPI:     $HAVE_DEVMEM])
AC_MSG_NOTICE([  DMA-BUF MRs:         $HAVE_DMABUF])
AC_MSG_NOTICE([  socket zero-copy:    $HAVE_ZEROCOPY])
AC_MSG_NOTICE([  virtme-ng tests:     $HAVE_VIRTME])
AC_MSG_NOTICE([  ibverbs tools:       $HAVE_IBV_TOOLS])
AC_OUTPUT
