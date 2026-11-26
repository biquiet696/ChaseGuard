/*
 * ChaseGuard - minimal Win32 declarations (the plugin is built without the Windows SDK and
 * without a C runtime, so it has no dependency on any Visual C++ Redistributable).
 * License: GPL-3.0-or-later (see LICENSE)
 */
#ifndef CHASEGUARD_WIN32_H
#define CHASEGUARD_WIN32_H

#define WINAPI __stdcall
typedef int            BOOL;
typedef unsigned long  DWORD;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
typedef long           LONG;
typedef void          *HANDLE;
typedef void          *HMODULE;
typedef unsigned long  SIZE_T;
typedef char          *va_list_t;

#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG)-1)

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1

#define EXCEPTION_ACCESS_VIOLATION     0xC0000005u
#define EXCEPTION_NONCONTINUABLE       0x1u
#define EXCEPTION_CONTINUE_EXECUTION   (-1)
#define EXCEPTION_CONTINUE_SEARCH      0

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD             0x100
#define MEM_COMMIT             0x1000

#define GENERIC_WRITE          0x40000000u
#define FILE_SHARE_READ        0x1
#define FILE_SHARE_WRITE       0x2
#define OPEN_ALWAYS            4
#define CREATE_ALWAYS          2
#define FILE_ATTRIBUTE_NORMAL  0x80
#define FILE_END               2

#define MB_ICONERROR           0x10
#define MB_ICONWARNING         0x30
#define MB_SETFOREGROUND       0x10000
#define MB_TOPMOST             0x40000

typedef struct EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters;
    DWORD ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct FLOATING_SAVE_AREA {
    DWORD ControlWord, StatusWord, TagWord, ErrorOffset, ErrorSelector, DataOffset, DataSelector;
    BYTE  RegisterArea[80];
    DWORD Spare0;
} FLOATING_SAVE_AREA;

typedef struct CONTEXT {
    DWORD ContextFlags;
    DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    FLOATING_SAVE_AREA FloatSave;
    DWORD SegGs, SegFs, SegEs, SegDs;
    DWORD Edi, Esi, Ebx, Edx, Ecx, Eax;
    DWORD Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    BYTE  ExtendedRegisters[512];
} CONTEXT;

typedef struct EXCEPTION_POINTERS {
    EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT *ContextRecord;
} EXCEPTION_POINTERS;

typedef LONG (WINAPI *PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);

typedef struct MEMORY_BASIC_INFORMATION {
    void *BaseAddress;
    void *AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION;

typedef struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

_Static_assert(sizeof(CONTEXT) == 0x2CC, "CONTEXT layout");
_Static_assert(__builtin_offsetof(CONTEXT, Eip) == 0xB8, "CONTEXT.Eip");
_Static_assert(__builtin_offsetof(CONTEXT, Esp) == 0xC4, "CONTEXT.Esp");
_Static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 28, "MBI layout");

#define IMPORT __declspec(dllimport)
IMPORT void  *WINAPI AddVectoredExceptionHandler(DWORD first, PVECTORED_EXCEPTION_HANDLER h);
IMPORT DWORD  WINAPI RemoveVectoredExceptionHandler(void *h);
IMPORT BOOL   WINAPI VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old);
IMPORT SIZE_T WINAPI VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);
IMPORT HMODULE WINAPI GetModuleHandleA(const char *name);
IMPORT DWORD  WINAPI GetModuleFileNameA(HMODULE m, char *buf, DWORD size);
IMPORT HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa,
                                 DWORD disposition, DWORD flags, HANDLE tmpl);
IMPORT BOOL   WINAPI WriteFile(HANDLE f, const void *buf, DWORD len, DWORD *written, void *ov);
IMPORT DWORD  WINAPI GetFileSize(HANDLE f, DWORD *high);
IMPORT DWORD  WINAPI SetFilePointer(HANDLE f, LONG dist, LONG *high, DWORD method);
IMPORT BOOL   WINAPI CloseHandle(HANDLE h);
IMPORT void   WINAPI GetLocalTime(SYSTEMTIME *t);
IMPORT DWORD  WINAPI GetTickCount(void);
IMPORT unsigned WINAPI GetPrivateProfileIntA(const char *app, const char *key, int def,
                                             const char *file);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT DWORD  WINAPI GetCurrentThreadId(void);
IMPORT BOOL   WINAPI FlushInstructionCache(HANDLE p, const void *addr, SIZE_T size);
IMPORT BOOL   WINAPI DisableThreadLibraryCalls(HMODULE m);
IMPORT DWORD  WINAPI GetLastError(void);
IMPORT void   WINAPI Sleep(DWORD ms);
IMPORT HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, DWORD (WINAPI *fn)(void *), void *arg,
                                  DWORD flags, DWORD *tid);
typedef LONG (WINAPI *PTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
IMPORT PTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER f);
IMPORT BOOL   WINAPI GetModuleHandleExA(DWORD flags, const char *name, HMODULE *module);
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x4
IMPORT int    WINAPI MessageBoxA(void *wnd, const char *text, const char *caption, unsigned type);
IMPORT int    WINAPI wvsprintfA(char *out, const char *fmt, va_list_t args);

#endif
