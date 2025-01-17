dnl Process this file with autoconf to produce a configure script.
AC_INIT
AC_CONFIG_HEADERS([_searpc_config.h])
AC_CONFIG_FILES([Makefile.conf])

# for dumpmachine
AC_PROG_CC

PKG_CHECK_MODULES([GLIB2], [glib-2.0], [
    CFLAGS="$CFLAGS $GLIB2_CFLAGS"
    LIBS="$LIBS $GLIB2_LIBS"
  ],
  [ AC_MSG_ERROR(glib2-devel not found) ]
)

PKG_CHECK_MODULES([SEARPC], [libsearpc], [
    CFLAGS="$CFLAGS $SEARPC_CFLAGS"
    LIBS="$LIBS $SEARPC_LIBS"
  ],
  [ AC_MSG_ERROR(libsearpc-devel not found) ]
)

AC_PATH_PROGS([RPCGEN], [searpc-codegen searpc-codegen.py])
if test -z "$RPCGEN"; then
  AC_MSG_WARN(searpc-codegen not found)
  cp $srcdir/sr_bkp/*.h .
  cp $srcdir/sr_bkp/dremote/*.h dremote
fi
AC_SUBST(RPCGEN)

AC_MSG_CHECKING([for remote DPMI-compatible host])
CONFIG_HOST=`uname -s`
CONFIG_OS=`uname -o`
machine=`$CC -dumpmachine | cut -d- -f1 | sed 's/i.86/i386/'`
if test "$CONFIG_HOST" != "Linux"; then
  nok=1
  AC_MSG_RESULT([unsupported host])
fi
if test "$nok" != "1"; then
  if test "$machine" != "i386" -a "$machine" != "x86_64"; then
    nok=1
    AC_MSG_RESULT([unsupported arch])
  fi
fi
if test "$nok" != "1"; then
  if test "$CONFIG_OS" = "Android"; then
    nok=1
    AC_MSG_RESULT([no, uffd unsupported on android])
  fi
fi
if test "$nok" != "1"; then
  AC_MSG_RESULT([ok])
fi
if test "$nok" != "1"; then
  PKG_CHECK_MODULES([SECCOMP], [libseccomp],,
    [
      nok=1
      AC_MSG_WARN(libseccomp-devel not found)
    ]
  )
fi
if test "$nok" != "1"; then
  AC_CHECK_DECLS([UFFD_FEATURE_WP_HUGETLBFS_SHMEM],,
    [
      nok=1
      AC_MSG_WARN(uffd not supported)
    ], [[#include <linux/userfaultfd.h>]]
  )
fi
if test "$nok" != "1"; then
  AC_MSG_NOTICE([remote DPMI supported])
  DREMOTE=1
  AC_SUBST(DREMOTE)
fi

AC_CHECK_DECLS([UFFD_FEATURE_WP_ASYNC],,,[[#include <linux/userfaultfd.h>]])

AC_OUTPUT

sed -E 's/#define PACKAGE_.+//' _searpc_config.h >searpc_config.hh
