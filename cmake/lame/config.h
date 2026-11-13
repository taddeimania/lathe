/* LAME's config.h for Lathe's build (LAME's own comes from autoconf, which CMake doesn't run). */
#ifndef LATHE_LAME_CONFIG_H
#define LATHE_LAME_CONFIG_H
#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define PACKAGE "lame"
#define VERSION "3.100"
/* machine.h leaves this to config.h */
typedef float ieee754_float32_t;
#if defined(_MSC_VER)
 #pragma warning(disable: 4244 4267 4305 4018 4996 4101 4146)
#endif
#endif
