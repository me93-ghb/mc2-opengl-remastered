#ifndef PLATFORM_WINDOWS_H
#define PLATFORM_WINDOWS_H

#define NOMINMAX

#ifdef PLATFORM_WINDOWS
	#include<windows.h>
#else

	#include"platform_windef.h"
	#include"platform_winbase.h"
	#include"platform_winuser.h"
	#include"platform_winnls.h"
	#include"platform_mmsystem.h"
	#include"platform_str.h"    // macos-port: MSVC->POSIX name shims reach every TU
	#include"platform_stdlib.h" // macos-port: _splitpath / _itoa shims reach every TU
#endif // PLATFORM_WINDOWS_H

#endif /* _WINDOWS_ */
