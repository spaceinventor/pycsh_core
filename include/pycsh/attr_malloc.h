#pragma once

/* Version check macros */
#if defined(__GNUC__)
#  define GCC_VERSION (__GNUC__ * 10000 \
                       + __GNUC_MINOR__ * 100 \
                       + __GNUC_PATCHLEVEL__)
#else
#  define GCC_VERSION 0
#endif

#if defined(__clang__)
#  define CLANG_VERSION (__clang_major__ * 10000 \
                         + __clang_minor__ * 100 \
                         + __clang_patchlevel__)
#else
#  define CLANG_VERSION 0
#endif

/* Attribute macro */
#if (defined(__clang__) && CLANG_VERSION >= 150000) || (GCC_VERSION >= 110000)
#  define ATTR_MALLOC(f, a) __attribute__((malloc(f, a)))
#else
#  define ATTR_MALLOC(f, a) __attribute__((malloc))
#endif
