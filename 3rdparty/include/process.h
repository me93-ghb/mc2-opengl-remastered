#ifdef _WIN32
/* Windows: use the real <process.h> (this shim sits on the -I path). */
#include_next <process.h>
#else
/* macos-port / non-Windows: the engine only uses _getpid() from <process.h>. */
#ifndef MC2_COMPAT_PROCESS_H
#define MC2_COMPAT_PROCESS_H
#include <unistd.h>
static inline int _getpid(void) { return (int)getpid(); }
#endif /* MC2_COMPAT_PROCESS_H */
#endif /* _WIN32 */
