#define __WINE_CONFIG_H

#define HAVE_STRERROR
#define HAVE_MEMMOVE
#define HAVE_FLOAT_H

#undef inline
#define inline __inline

#define HAVE_STRING_H
#define HAVE_SPAWNVP
#define HAVE_IO_H
#define HAVE_POLL
#define HAVE_PROCESS_H
#define DECLSPEC_HOTPATCH
#define DECLSPEC_NORETURN
#define HAVE__ISNAN 1
#define HAVE__FINITE 1
#define HAVE__STRICMP 1
#define HAVE_ISNAN 1
#define HAVE_ISINF 1
#define HAVE_ISFINITE 1
#define HAVE__SPAWNVP 1
#define USE_COMPILER_EXCEPTIONS

#define DLLPREFIX ""

#ifdef __x86_64__
#define HAVE_SIZE_T
typedef unsigned __int64    size_t;
#endif

#ifdef WOW64
#define HAVE_SIZE_T
typedef unsigned __int64 size_t;
#endif

//#include <direct.h>
#if _MSC_VER < 1900
#define snprintf _snprintf
#endif

#define strtoull _strtoui64
