#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.50])
AC_INIT([libchistka], [1.1.0], [jasonlingle@yahoo.com])
AM_INIT_AUTOMAKE
AC_CONFIG_MACRO_DIR([m4])
LT_PREREQ([1.5])
LT_INIT([dlopen])
AC_CONFIG_SRCDIR([src/shim.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile src/Makefile man/Makefile])
# Disable Automake's braindead want to rerun ./configure if it thinks it
# needs to regenerate the entire build system
AM_MAINTAINER_MODE

# Checks for programs.
AC_PROG_CC
AC_PROG_RANLIB

# Checks for libraries.
AC_SEARCH_LIBS(dlopen, [dl])
AC_SEARCH_LIBS(sem_wait, [rt])

# Checks for header files.
AC_CHECK_HEADERS([stdlib.h string.h unistd.h dlfcn.h fcntl.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_MODE_T
AC_TYPE_PID_T
AC_C_INLINE
AC_TYPE_OFF_T

# Checks for library functions.
AC_CHECK_FUNCS([dup2 putenv memset strdup alarm getcwd strchr strstr])
AC_FUNC_FORK
AC_FUNC_MALLOC

AC_OUTPUT
