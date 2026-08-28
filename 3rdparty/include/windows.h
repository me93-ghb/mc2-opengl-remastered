#ifdef _WIN32
/* Windows: use the real <windows.h> (this shim sits on the -I path). */
#include_next <windows.h>
#else
/* macos-port / non-Windows: forward to GameOS's Win32-on-POSIX emulation, so the
 * ~two dozen TUs that include <windows.h> directly resolve to the same shims that
 * gameos.hpp pulls in. platform_windows.h has its own include guard, so this is
 * safe to reach more than once. */
#include <platform_windows.h>
#endif
