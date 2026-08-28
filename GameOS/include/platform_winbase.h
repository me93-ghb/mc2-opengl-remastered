#ifndef _WINBASE_H
#define _WINBASE_H

#ifdef __cplusplus
extern "C" {
#endif

#define WINBASEAPI    

#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2

#define INVALID_HANDLE_VALUE (HANDLE)-1

typedef struct _SYSTEMTIME {
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
} SYSTEMTIME,*LPSYSTEMTIME;

typedef struct _TIME_ZONE_INFORMATION {
	LONG Bias;
	WCHAR StandardName[32];
	SYSTEMTIME StandardDate;
	LONG StandardBias;
	WCHAR DaylightName[32];
	SYSTEMTIME DaylightDate;
	LONG DaylightBias;
} TIME_ZONE_INFORMATION,*LPTIME_ZONE_INFORMATION;

typedef struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME,*PFILETIME,*LPFILETIME;

typedef struct _WIN32_FIND_DATAA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;

#ifdef _WIN32_WCE
    DWORD dwOID;
#else
	DWORD dwReserved0;
	DWORD dwReserved1;
#endif

	CHAR cFileName[MAX_PATH];

#ifndef _WIN32_WCE
	CHAR cAlternateFileName[14];
#endif

} WIN32_FIND_DATAA,*PWIN32_FIND_DATAA,*LPWIN32_FIND_DATAA;

#define WIN32_FIND_DATA WIN32_FIND_DATAA

DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION);
VOID WINAPI GetSystemTime(LPSYSTEMTIME);

#define RtlMoveMemory memmove
#define RtlCopyMemory memcpy
#define RtlFillMemory(d,l,f) memset((d), (f), (l))
#define RtlZeroMemory(d,l) RtlFillMemory((d),(l),0)
#define MoveMemory RtlMoveMemory
#define CopyMemory RtlCopyMemory
#define FillMemory RtlFillMemory
#define ZeroMemory RtlZeroMemory

BOOL CreateDirectory(LPCSTR,LPSECURITY_ATTRIBUTES);
BOOL SetFileAttributes(LPCSTR,DWORD);
BOOL DeleteFile(LPCSTR);
BOOL CopyFile(LPCTSTR, LPCTSTR, BOOL);

int lstrlen(LPCSTR);
int lstrlenA(LPCSTR);

#define lstrcmpi lstrcmpiA

int lstrcmpiA(LPCSTR,LPCSTR);
int lstrcmpiW(LPCWSTR,LPCWSTR);


WINBASEAPI HANDLE WINAPI FindFirstFileA(LPCSTR,LPWIN32_FIND_DATAA);
//WINBASEAPI HANDLE WINAPI FindFirstFileW(LPCWSTR,LPWIN32_FIND_DATAW);
#define FindFirstFile FindFirstFileA

WINBASEAPI BOOL WINAPI FindNextFileA(HANDLE,LPWIN32_FIND_DATAA);
//WINBASEAPI BOOL WINAPI FindNextFileW(HANDLE,LPWIN32_FIND_DATAW);
#define FindNextFile FindNextFileA

BOOL WINAPI FindClose(HANDLE);

// macos-port: file/time query APIs used by the mod-discovery + file-resolve trace
// in mclib/file.cpp. POSIX-backed implementations live in platform_winbase.cpp.
typedef union _LARGE_INTEGER {
	struct { DWORD LowPart; LONG HighPart; };
	long long QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
	struct { DWORD LowPart; DWORD HighPart; };
	unsigned long long QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef enum _GET_FILEEX_INFO_LEVELS {
	GetFileExInfoStandard,
	GetFileExMaxInfoLevel
} GET_FILEEX_INFO_LEVELS;

#define GENERIC_READ          0x80000000u
#define FILE_SHARE_READ       0x00000001u
#define OPEN_EXISTING         3
#define FILE_ATTRIBUTE_NORMAL 0x00000080u

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER*);
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER*);
BOOL WINAPI GetFileAttributesExA(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
#define GetFileAttributesEx GetFileAttributesExA
LONG WINAPI CompareFileTime(const FILETIME*, const FILETIME*);
HANDLE WINAPI CreateFileA(LPCSTR, DWORD, DWORD, LPVOID, DWORD, DWORD, HANDLE);
#define CreateFile CreateFileA
BOOL WINAPI ReadFile(HANDLE, LPVOID, DWORD, DWORD*, LPVOID);
BOOL WINAPI CloseHandle(HANDLE);

#define INVALID_FILE_ATTRIBUTES  ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
DWORD WINAPI GetFileAttributesA(LPCSTR);
#define GetFileAttributes GetFileAttributesA
DWORD WINAPI GetFullPathNameA(LPCSTR, DWORD, LPSTR, LPSTR*);
#define GetFullPathName GetFullPathNameA
DWORD WINAPI GetCurrentThreadId(void);
unsigned long long WINAPI GetTickCount64(void);

WINBASEAPI PVOID WINAPI VirtualAlloc(PVOID,DWORD,DWORD,DWORD);
WINBASEAPI BOOL WINAPI VirtualFree(PVOID,DWORD,DWORD);

WINBASEAPI DWORD WINAPI GetLastError(void);

#ifdef __cplusplus
}
#endif

#endif /* _WINBASE_H */
