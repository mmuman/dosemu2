dnl Process this file with autoconf to produce a configure script.
AC_INIT
AC_CONFIG_FILES([Makefile.conf])

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

AC_PATH_PROGS([RPCGEN], [searpc-codegen searpc-codegen.py],
  [AC_MSG_ERROR(searpc-codegen not found)])
AC_SUBST(RPCGEN)

AC_MSG_CHECKING([remote DPMI])
CONFIG_HOST=`uname -s`
machine=`cc -dumpmachine | cut -d- -f1 | sed 's/i.86/i386/'`
if test "$CONFIG_HOST" = "Linux"; then
  if test "$machine" = "i386" -o "$machine" = "x86_64"; then
    AC_MSG_RESULT([supported])
    DREMOTE=1
    AC_SUBST(DREMOTE)
  else
    AC_MSG_RESULT([unsupported arch])
  fi
else
  AC_MSG_RESULT([unsupported host])
fi

AC_OUTPUT
