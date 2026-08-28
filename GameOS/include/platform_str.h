#pragma once

#ifndef PLATFORM_STR_H
#define PLATFORM_STR_H

#include<stdio.h> // va_list


// string functions to use in cross-platform environment
int	S_strcmp(const char* s1, const char* s2);
int S_strncmp(const char* s1, const char* s2, size_t max_count);
int S_stricmp(const char* s1, const char* s2);
int S_strnicmp(const char* s1, const char* s2, size_t max_count);
char* S_strupr(char* s);
char* S_strlwr(char* s);
int S_snprintf(char *str, size_t size, const char *format, ...);
int S_vsnprintf(char *str, size_t size, const char *format, va_list ap);
//int S_sprintf(char *str, const char *format, ...);

#define _strdup strdup

// macos-port: many call sites still use MSVC/CRT names. On non-Windows map them
// to their POSIX equivalents (PLATFORM_WINDOWS gets the real ones from the CRT).
#ifndef PLATFORM_WINDOWS
#include <strings.h> // strcasecmp / strncasecmp
#include <alloca.h>  // alloca
#include <stdlib.h>  // setenv
#define _stricmp   strcasecmp
#define _strnicmp  strncasecmp
#define _snprintf  snprintf
#define _vsnprintf vsnprintf
#define _alloca    alloca
// _putenv_s(name,val) and setenv(name,val,overwrite) both return 0 on success.
#define _putenv_s(name, val) setenv((name), (val), 1)
#define sprintf_s snprintf // callers use the explicit-size form sprintf_s(buf,size,fmt,...)
// OutputDebugString* target the Windows debugger; no-op elsewhere (real logging
// goes through the engine's own log paths).
#define OutputDebugStringA(s) ((void)(s))
#define OutputDebugString(s)  ((void)(s))
#endif


#endif // PLATFORM_STR_H
