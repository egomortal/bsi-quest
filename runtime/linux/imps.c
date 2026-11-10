#include "cpu.h"
#include <stdio.h>
typedef void (*hle_fn)(CPU*c);
typedef struct { uint32_t iat; const char *name; hle_fn fn; } Imp;
extern FILE *LOGF_RT; void stub_note(const char*n);
void hle_comctl32_ord345(CPU*c) __attribute__((weak));
static void stub_0(CPU*c){ stub_note("COMCTL32.dll!ord345"); c->eax=0; c->esp+=4+16; }
void hle_wsock32_bind(CPU*c) __attribute__((weak));
static void stub_1(CPU*c){ stub_note("WSOCK32.dll!bind"); c->eax=0; c->esp+=4+12; }
void hle_wsock32_WSAGetLastError(CPU*c) __attribute__((weak));
static void stub_2(CPU*c){ stub_note("WSOCK32.dll!WSAGetLastError"); c->eax=0; c->esp+=4+0; }
void hle_wsock32_connect(CPU*c) __attribute__((weak));
static void stub_3(CPU*c){ stub_note("WSOCK32.dll!connect"); c->eax=0; c->esp+=4+12; }
void hle_wsock32_listen(CPU*c) __attribute__((weak));
static void stub_4(CPU*c){ stub_note("WSOCK32.dll!listen"); c->eax=0; c->esp+=4+8; }
void hle_wsock32_select(CPU*c) __attribute__((weak));
static void stub_5(CPU*c){ stub_note("WSOCK32.dll!select"); c->eax=0; c->esp+=4+20; }
void hle_wsock32_ioctlsocket(CPU*c) __attribute__((weak));
static void stub_6(CPU*c){ stub_note("WSOCK32.dll!ioctlsocket"); c->eax=0; c->esp+=4+12; }
void hle_wsock32_sendto(CPU*c) __attribute__((weak));
static void stub_7(CPU*c){ stub_note("WSOCK32.dll!sendto"); c->eax=0; c->esp+=4+24; }
void hle_wsock32_send(CPU*c) __attribute__((weak));
static void stub_8(CPU*c){ stub_note("WSOCK32.dll!send"); c->eax=0; c->esp+=4+16; }
void hle_wsock32_recvfrom(CPU*c) __attribute__((weak));
static void stub_9(CPU*c){ stub_note("WSOCK32.dll!recvfrom"); c->eax=0; c->esp+=4+24; }
void hle_wsock32_recv(CPU*c) __attribute__((weak));
static void stub_10(CPU*c){ stub_note("WSOCK32.dll!recv"); c->eax=0; c->esp+=4+16; }
void hle_wsock32_getsockname(CPU*c) __attribute__((weak));
static void stub_11(CPU*c){ stub_note("WSOCK32.dll!getsockname"); c->eax=0; c->esp+=4+12; }
void hle_wsock32_closesocket(CPU*c) __attribute__((weak));
static void stub_12(CPU*c){ stub_note("WSOCK32.dll!closesocket"); c->eax=0; c->esp+=4+4; }
void hle_wsock32_WSAStartup(CPU*c) __attribute__((weak));
static void stub_13(CPU*c){ stub_note("WSOCK32.dll!WSAStartup"); c->eax=0; c->esp+=4+8; }
void hle_wsock32_shutdown(CPU*c) __attribute__((weak));
static void stub_14(CPU*c){ stub_note("WSOCK32.dll!shutdown"); c->eax=0; c->esp+=4+8; }
void hle_wsock32_gethostname(CPU*c) __attribute__((weak));
static void stub_15(CPU*c){ stub_note("WSOCK32.dll!gethostname"); c->eax=0; c->esp+=4+8; }
void hle_wsock32_socket(CPU*c) __attribute__((weak));
static void stub_16(CPU*c){ stub_note("WSOCK32.dll!socket"); c->eax=0; c->esp+=4+12; }
void hle_wsock32_ntohs(CPU*c) __attribute__((weak));
static void stub_17(CPU*c){ stub_note("WSOCK32.dll!ntohs"); c->eax=0; c->esp+=4+4; }
void hle_wsock32_accept(CPU*c) __attribute__((weak));
static void stub_18(CPU*c){ stub_note("WSOCK32.dll!accept"); c->eax=0; c->esp+=4+12; }
void hle_wsock32_htons(CPU*c) __attribute__((weak));
static void stub_19(CPU*c){ stub_note("WSOCK32.dll!htons"); c->eax=0; c->esp+=4+4; }
void hle_wsock32___WSAFDIsSet(CPU*c) __attribute__((weak));
static void stub_20(CPU*c){ stub_note("WSOCK32.dll!__WSAFDIsSet"); c->eax=0; c->esp+=4+8; }
void hle_wsock32_WSASetLastError(CPU*c) __attribute__((weak));
static void stub_21(CPU*c){ stub_note("WSOCK32.dll!WSASetLastError"); c->eax=0; c->esp+=4+4; }
void hle_wsock32_getpeername(CPU*c) __attribute__((weak));
static void stub_22(CPU*c){ stub_note("WSOCK32.dll!getpeername"); c->eax=0; c->esp+=4+12; }
void hle_wsock32_gethostbyname(CPU*c) __attribute__((weak));
static void stub_23(CPU*c){ stub_note("WSOCK32.dll!gethostbyname"); c->eax=0; c->esp+=4+4; }
void hle_wsock32_WSACleanup(CPU*c) __attribute__((weak));
static void stub_24(CPU*c){ stub_note("WSOCK32.dll!WSACleanup"); c->eax=0; c->esp+=4+0; }
void hle_wsock32_getsockopt(CPU*c) __attribute__((weak));
static void stub_25(CPU*c){ stub_note("WSOCK32.dll!getsockopt"); c->eax=0; c->esp+=4+20; }
void hle_wsock32_ntohl(CPU*c) __attribute__((weak));
static void stub_26(CPU*c){ stub_note("WSOCK32.dll!ntohl"); c->eax=0; c->esp+=4+4; }
void hle_wsock32_htonl(CPU*c) __attribute__((weak));
static void stub_27(CPU*c){ stub_note("WSOCK32.dll!htonl"); c->eax=0; c->esp+=4+4; }
void hle_wsock32_setsockopt(CPU*c) __attribute__((weak));
static void stub_28(CPU*c){ stub_note("WSOCK32.dll!setsockopt"); c->eax=0; c->esp+=4+20; }
void hle_wsock32_inet_addr(CPU*c) __attribute__((weak));
static void stub_29(CPU*c){ stub_note("WSOCK32.dll!inet_addr"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_SetCriticalSectionSpinCount(CPU*c) __attribute__((weak));
static void stub_30(CPU*c){ stub_note("KERNEL32.dll!SetCriticalSectionSpinCount"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_InitializeCriticalSection(CPU*c) __attribute__((weak));
static void stub_31(CPU*c){ stub_note("KERNEL32.dll!InitializeCriticalSection"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_EnterCriticalSection(CPU*c) __attribute__((weak));
static void stub_32(CPU*c){ stub_note("KERNEL32.dll!EnterCriticalSection"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_TryEnterCriticalSection(CPU*c) __attribute__((weak));
static void stub_33(CPU*c){ stub_note("KERNEL32.dll!TryEnterCriticalSection"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_LeaveCriticalSection(CPU*c) __attribute__((weak));
static void stub_34(CPU*c){ stub_note("KERNEL32.dll!LeaveCriticalSection"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_QueryPerformanceCounter(CPU*c) __attribute__((weak));
static void stub_35(CPU*c){ stub_note("KERNEL32.dll!QueryPerformanceCounter"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_InterlockedIncrement(CPU*c) __attribute__((weak));
static void stub_36(CPU*c){ stub_note("KERNEL32.dll!InterlockedIncrement"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_InterlockedDecrement(CPU*c) __attribute__((weak));
static void stub_37(CPU*c){ stub_note("KERNEL32.dll!InterlockedDecrement"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_InterlockedExchangeAdd(CPU*c) __attribute__((weak));
static void stub_38(CPU*c){ stub_note("KERNEL32.dll!InterlockedExchangeAdd"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_InterlockedExchange(CPU*c) __attribute__((weak));
static void stub_39(CPU*c){ stub_note("KERNEL32.dll!InterlockedExchange"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_InterlockedCompareExchange(CPU*c) __attribute__((weak));
static void stub_40(CPU*c){ stub_note("KERNEL32.dll!InterlockedCompareExchange"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_GetCurrentThread(CPU*c) __attribute__((weak));
static void stub_41(CPU*c){ stub_note("KERNEL32.dll!GetCurrentThread"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_GetCurrentThreadId(CPU*c) __attribute__((weak));
static void stub_42(CPU*c){ stub_note("KERNEL32.dll!GetCurrentThreadId"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_SetThreadIdealProcessor(CPU*c) __attribute__((weak));
static void stub_43(CPU*c){ stub_note("KERNEL32.dll!SetThreadIdealProcessor"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_TlsAlloc(CPU*c) __attribute__((weak));
static void stub_44(CPU*c){ stub_note("KERNEL32.dll!TlsAlloc"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_TlsSetValue(CPU*c) __attribute__((weak));
static void stub_45(CPU*c){ stub_note("KERNEL32.dll!TlsSetValue"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_TlsGetValue(CPU*c) __attribute__((weak));
static void stub_46(CPU*c){ stub_note("KERNEL32.dll!TlsGetValue"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_TlsFree(CPU*c) __attribute__((weak));
static void stub_47(CPU*c){ stub_note("KERNEL32.dll!TlsFree"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_CreateSemaphoreW(CPU*c) __attribute__((weak));
static void stub_48(CPU*c){ stub_note("KERNEL32.dll!CreateSemaphoreW"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_CloseHandle(CPU*c) __attribute__((weak));
static void stub_49(CPU*c){ stub_note("KERNEL32.dll!CloseHandle"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_WaitForSingleObject(CPU*c) __attribute__((weak));
static void stub_50(CPU*c){ stub_note("KERNEL32.dll!WaitForSingleObject"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_ReleaseSemaphore(CPU*c) __attribute__((weak));
static void stub_51(CPU*c){ stub_note("KERNEL32.dll!ReleaseSemaphore"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_SetCurrentDirectoryW(CPU*c) __attribute__((weak));
static void stub_52(CPU*c){ stub_note("KERNEL32.dll!SetCurrentDirectoryW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetFileSize(CPU*c) __attribute__((weak));
static void stub_53(CPU*c){ stub_note("KERNEL32.dll!GetFileSize"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_CreateFileW(CPU*c) __attribute__((weak));
static void stub_54(CPU*c){ stub_note("KERNEL32.dll!CreateFileW"); c->eax=0; c->esp+=4+28; }
void hle_kernel32_GetLastError(CPU*c) __attribute__((weak));
static void stub_55(CPU*c){ stub_note("KERNEL32.dll!GetLastError"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_GetFileAttributesW(CPU*c) __attribute__((weak));
static void stub_56(CPU*c){ stub_note("KERNEL32.dll!GetFileAttributesW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_CreateDirectoryW(CPU*c) __attribute__((weak));
static void stub_57(CPU*c){ stub_note("KERNEL32.dll!CreateDirectoryW"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GetFileTime(CPU*c) __attribute__((weak));
static void stub_58(CPU*c){ stub_note("KERNEL32.dll!GetFileTime"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_SystemTimeToFileTime(CPU*c) __attribute__((weak));
static void stub_59(CPU*c){ stub_note("KERNEL32.dll!SystemTimeToFileTime"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GetSystemTime(CPU*c) __attribute__((weak));
static void stub_60(CPU*c){ stub_note("KERNEL32.dll!GetSystemTime"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_SetFilePointer(CPU*c) __attribute__((weak));
static void stub_61(CPU*c){ stub_note("KERNEL32.dll!SetFilePointer"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_GetCurrentProcess(CPU*c) __attribute__((weak));
static void stub_62(CPU*c){ stub_note("KERNEL32.dll!GetCurrentProcess"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_VerifyVersionInfoW(CPU*c) __attribute__((weak));
static void stub_63(CPU*c){ stub_note("KERNEL32.dll!VerifyVersionInfoW"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_VerSetConditionMask(CPU*c) __attribute__((weak));
static void stub_64(CPU*c){ stub_note("KERNEL32.dll!VerSetConditionMask"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_RtlCaptureStackBackTrace(CPU*c) __attribute__((weak));
static void stub_65(CPU*c){ stub_note("KERNEL32.dll!RtlCaptureStackBackTrace"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_RtlCaptureContext(CPU*c) __attribute__((weak));
static void stub_66(CPU*c){ stub_note("KERNEL32.dll!RtlCaptureContext"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_ReadFile(CPU*c) __attribute__((weak));
static void stub_67(CPU*c){ stub_note("KERNEL32.dll!ReadFile"); c->eax=0; c->esp+=4+20; }
void hle_kernel32_RaiseException(CPU*c) __attribute__((weak));
static void stub_68(CPU*c){ stub_note("KERNEL32.dll!RaiseException"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_GetProcAddress(CPU*c) __attribute__((weak));
static void stub_69(CPU*c){ stub_note("KERNEL32.dll!GetProcAddress"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GetModuleHandleW(CPU*c) __attribute__((weak));
static void stub_70(CPU*c){ stub_note("KERNEL32.dll!GetModuleHandleW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_FlushFileBuffers(CPU*c) __attribute__((weak));
static void stub_71(CPU*c){ stub_note("KERNEL32.dll!FlushFileBuffers"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_ExitProcess(CPU*c) __attribute__((weak));
static void stub_72(CPU*c){ stub_note("KERNEL32.dll!ExitProcess"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_SetConsoleCtrlHandler(CPU*c) __attribute__((weak));
static void stub_73(CPU*c){ stub_note("KERNEL32.dll!SetConsoleCtrlHandler"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_Sleep(CPU*c) __attribute__((weak));
static void stub_74(CPU*c){ stub_note("KERNEL32.dll!Sleep"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_CreateEventW(CPU*c) __attribute__((weak));
static void stub_75(CPU*c){ stub_note("KERNEL32.dll!CreateEventW"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_SetEvent(CPU*c) __attribute__((weak));
static void stub_76(CPU*c){ stub_note("KERNEL32.dll!SetEvent"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_ResetEvent(CPU*c) __attribute__((weak));
static void stub_77(CPU*c){ stub_note("KERNEL32.dll!ResetEvent"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_ResumeThread(CPU*c) __attribute__((weak));
static void stub_78(CPU*c){ stub_note("KERNEL32.dll!ResumeThread"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_SuspendThread(CPU*c) __attribute__((weak));
static void stub_79(CPU*c){ stub_note("KERNEL32.dll!SuspendThread"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_SetThreadPriority(CPU*c) __attribute__((weak));
static void stub_80(CPU*c){ stub_note("KERNEL32.dll!SetThreadPriority"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_OutputDebugStringW(CPU*c) __attribute__((weak));
static void stub_81(CPU*c){ stub_note("KERNEL32.dll!OutputDebugStringW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_FormatMessageW(CPU*c) __attribute__((weak));
static void stub_82(CPU*c){ stub_note("KERNEL32.dll!FormatMessageW"); c->eax=0; c->esp+=4+28; }
void hle_kernel32_LoadLibraryW(CPU*c) __attribute__((weak));
static void stub_83(CPU*c){ stub_note("KERNEL32.dll!LoadLibraryW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_FreeLibrary(CPU*c) __attribute__((weak));
static void stub_84(CPU*c){ stub_note("KERNEL32.dll!FreeLibrary"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetVersionExW(CPU*c) __attribute__((weak));
static void stub_85(CPU*c){ stub_note("KERNEL32.dll!GetVersionExW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetLocalTime(CPU*c) __attribute__((weak));
static void stub_86(CPU*c){ stub_note("KERNEL32.dll!GetLocalTime"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_FileTimeToSystemTime(CPU*c) __attribute__((weak));
static void stub_87(CPU*c){ stub_note("KERNEL32.dll!FileTimeToSystemTime"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_FileTimeToLocalFileTime(CPU*c) __attribute__((weak));
static void stub_88(CPU*c){ stub_note("KERNEL32.dll!FileTimeToLocalFileTime"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GetSystemTimeAsFileTime(CPU*c) __attribute__((weak));
static void stub_89(CPU*c){ stub_note("KERNEL32.dll!GetSystemTimeAsFileTime"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_LocalFileTimeToFileTime(CPU*c) __attribute__((weak));
static void stub_90(CPU*c){ stub_note("KERNEL32.dll!LocalFileTimeToFileTime"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_TerminateProcess(CPU*c) __attribute__((weak));
static void stub_91(CPU*c){ stub_note("KERNEL32.dll!TerminateProcess"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GetCurrentProcessId(CPU*c) __attribute__((weak));
static void stub_92(CPU*c){ stub_note("KERNEL32.dll!GetCurrentProcessId"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_lstrlenW(CPU*c) __attribute__((weak));
static void stub_93(CPU*c){ stub_note("KERNEL32.dll!lstrlenW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetComputerNameW(CPU*c) __attribute__((weak));
static void stub_94(CPU*c){ stub_note("KERNEL32.dll!GetComputerNameW"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_WideCharToMultiByte(CPU*c) __attribute__((weak));
static void stub_95(CPU*c){ stub_note("KERNEL32.dll!WideCharToMultiByte"); c->eax=0; c->esp+=4+32; }
void hle_kernel32_QueryPerformanceFrequency(CPU*c) __attribute__((weak));
static void stub_96(CPU*c){ stub_note("KERNEL32.dll!QueryPerformanceFrequency"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_lstrlenA(CPU*c) __attribute__((weak));
static void stub_97(CPU*c){ stub_note("KERNEL32.dll!lstrlenA"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_WriteFile(CPU*c) __attribute__((weak));
static void stub_98(CPU*c){ stub_note("KERNEL32.dll!WriteFile"); c->eax=0; c->esp+=4+20; }
void hle_kernel32_LocalAlloc(CPU*c) __attribute__((weak));
static void stub_99(CPU*c){ stub_note("KERNEL32.dll!LocalAlloc"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_DeleteFileW(CPU*c) __attribute__((weak));
static void stub_100(CPU*c){ stub_note("KERNEL32.dll!DeleteFileW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_SetFileAttributesW(CPU*c) __attribute__((weak));
static void stub_101(CPU*c){ stub_note("KERNEL32.dll!SetFileAttributesW"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_SetThreadAffinityMask(CPU*c) __attribute__((weak));
static void stub_102(CPU*c){ stub_note("KERNEL32.dll!SetThreadAffinityMask"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_CreateThread(CPU*c) __attribute__((weak));
static void stub_103(CPU*c){ stub_note("KERNEL32.dll!CreateThread"); c->eax=0; c->esp+=4+24; }
void hle_kernel32_SetStdHandle(CPU*c) __attribute__((weak));
static void stub_104(CPU*c){ stub_note("KERNEL32.dll!SetStdHandle"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_OpenEventW(CPU*c) __attribute__((weak));
static void stub_105(CPU*c){ stub_note("KERNEL32.dll!OpenEventW"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_GlobalUnlock(CPU*c) __attribute__((weak));
static void stub_106(CPU*c){ stub_note("KERNEL32.dll!GlobalUnlock"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GlobalLock(CPU*c) __attribute__((weak));
static void stub_107(CPU*c){ stub_note("KERNEL32.dll!GlobalLock"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_FindClose(CPU*c) __attribute__((weak));
static void stub_108(CPU*c){ stub_note("KERNEL32.dll!FindClose"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_FindNextFileW(CPU*c) __attribute__((weak));
static void stub_109(CPU*c){ stub_note("KERNEL32.dll!FindNextFileW"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_FindFirstFileW(CPU*c) __attribute__((weak));
static void stub_110(CPU*c){ stub_note("KERNEL32.dll!FindFirstFileW"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_TerminateThread(CPU*c) __attribute__((weak));
static void stub_111(CPU*c){ stub_note("KERNEL32.dll!TerminateThread"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GlobalAlloc(CPU*c) __attribute__((weak));
static void stub_112(CPU*c){ stub_note("KERNEL32.dll!GlobalAlloc"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_RemoveDirectoryW(CPU*c) __attribute__((weak));
static void stub_113(CPU*c){ stub_note("KERNEL32.dll!RemoveDirectoryW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetCommandLineW(CPU*c) __attribute__((weak));
static void stub_114(CPU*c){ stub_note("KERNEL32.dll!GetCommandLineW"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_LoadLibraryA(CPU*c) __attribute__((weak));
static void stub_115(CPU*c){ stub_note("KERNEL32.dll!LoadLibraryA"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_CopyFileW(CPU*c) __attribute__((weak));
static void stub_116(CPU*c){ stub_note("KERNEL32.dll!CopyFileW"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_MoveFileW(CPU*c) __attribute__((weak));
static void stub_117(CPU*c){ stub_note("KERNEL32.dll!MoveFileW"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_WriteConsoleW(CPU*c) __attribute__((weak));
static void stub_118(CPU*c){ stub_note("KERNEL32.dll!WriteConsoleW"); c->eax=0; c->esp+=4+20; }
void hle_kernel32_SetConsoleTextAttribute(CPU*c) __attribute__((weak));
static void stub_119(CPU*c){ stub_note("KERNEL32.dll!SetConsoleTextAttribute"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_SetConsoleTitleW(CPU*c) __attribute__((weak));
static void stub_120(CPU*c){ stub_note("KERNEL32.dll!SetConsoleTitleW"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_LockResource(CPU*c) __attribute__((weak));
static void stub_121(CPU*c){ stub_note("KERNEL32.dll!LockResource"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_SizeofResource(CPU*c) __attribute__((weak));
static void stub_122(CPU*c){ stub_note("KERNEL32.dll!SizeofResource"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_LoadResource(CPU*c) __attribute__((weak));
static void stub_123(CPU*c){ stub_note("KERNEL32.dll!LoadResource"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_FindResourceW(CPU*c) __attribute__((weak));
static void stub_124(CPU*c){ stub_note("KERNEL32.dll!FindResourceW"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_GetConsoleScreenBufferInfo(CPU*c) __attribute__((weak));
static void stub_125(CPU*c){ stub_note("KERNEL32.dll!GetConsoleScreenBufferInfo"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GetConsoleWindow(CPU*c) __attribute__((weak));
static void stub_126(CPU*c){ stub_note("KERNEL32.dll!GetConsoleWindow"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_FreeConsole(CPU*c) __attribute__((weak));
static void stub_127(CPU*c){ stub_note("KERNEL32.dll!FreeConsole"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_SetConsoleScreenBufferSize(CPU*c) __attribute__((weak));
static void stub_128(CPU*c){ stub_note("KERNEL32.dll!SetConsoleScreenBufferSize"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_GetStdHandle(CPU*c) __attribute__((weak));
static void stub_129(CPU*c){ stub_note("KERNEL32.dll!GetStdHandle"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_AllocConsole(CPU*c) __attribute__((weak));
static void stub_130(CPU*c){ stub_note("KERNEL32.dll!AllocConsole"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_GlobalMemoryStatusEx(CPU*c) __attribute__((weak));
static void stub_131(CPU*c){ stub_note("KERNEL32.dll!GlobalMemoryStatusEx"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetSystemInfo(CPU*c) __attribute__((weak));
static void stub_132(CPU*c){ stub_note("KERNEL32.dll!GetSystemInfo"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetFileSizeEx(CPU*c) __attribute__((weak));
static void stub_133(CPU*c){ stub_note("KERNEL32.dll!GetFileSizeEx"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_SetErrorMode(CPU*c) __attribute__((weak));
static void stub_134(CPU*c){ stub_note("KERNEL32.dll!SetErrorMode"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_InterlockedCompareExchange64(CPU*c) __attribute__((weak));
static void stub_135(CPU*c){ stub_note("KERNEL32.dll!InterlockedCompareExchange64"); c->eax=0; c->esp+=4+20; }
void hle_kernel32_GetCurrentDirectoryA(CPU*c) __attribute__((weak));
static void stub_136(CPU*c){ stub_note("KERNEL32.dll!GetCurrentDirectoryA"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_VirtualAlloc(CPU*c) __attribute__((weak));
static void stub_137(CPU*c){ stub_note("KERNEL32.dll!VirtualAlloc"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_VirtualFree(CPU*c) __attribute__((weak));
static void stub_138(CPU*c){ stub_note("KERNEL32.dll!VirtualFree"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_ReleaseMutex(CPU*c) __attribute__((weak));
static void stub_139(CPU*c){ stub_note("KERNEL32.dll!ReleaseMutex"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_CreateMutexW(CPU*c) __attribute__((weak));
static void stub_140(CPU*c){ stub_note("KERNEL32.dll!CreateMutexW"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_SetUnhandledExceptionFilter(CPU*c) __attribute__((weak));
static void stub_141(CPU*c){ stub_note("KERNEL32.dll!SetUnhandledExceptionFilter"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_WaitForMultipleObjectsEx(CPU*c) __attribute__((weak));
static void stub_142(CPU*c){ stub_note("KERNEL32.dll!WaitForMultipleObjectsEx"); c->eax=0; c->esp+=4+20; }
void hle_kernel32_CreateMutexA(CPU*c) __attribute__((weak));
static void stub_143(CPU*c){ stub_note("KERNEL32.dll!CreateMutexA"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_OutputDebugStringA(CPU*c) __attribute__((weak));
static void stub_144(CPU*c){ stub_note("KERNEL32.dll!OutputDebugStringA"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_DebugBreak(CPU*c) __attribute__((weak));
static void stub_145(CPU*c){ stub_note("KERNEL32.dll!DebugBreak"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_ExitThread(CPU*c) __attribute__((weak));
static void stub_146(CPU*c){ stub_note("KERNEL32.dll!ExitThread"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_IsDebuggerPresent(CPU*c) __attribute__((weak));
static void stub_147(CPU*c){ stub_note("KERNEL32.dll!IsDebuggerPresent"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_UnhandledExceptionFilter(CPU*c) __attribute__((weak));
static void stub_148(CPU*c){ stub_note("KERNEL32.dll!UnhandledExceptionFilter"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetStartupInfoA(CPU*c) __attribute__((weak));
static void stub_149(CPU*c){ stub_note("KERNEL32.dll!GetStartupInfoA"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_CreateEventA(CPU*c) __attribute__((weak));
static void stub_150(CPU*c){ stub_note("KERNEL32.dll!CreateEventA"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_CreateSemaphoreA(CPU*c) __attribute__((weak));
static void stub_151(CPU*c){ stub_note("KERNEL32.dll!CreateSemaphoreA"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_GetModuleHandleA(CPU*c) __attribute__((weak));
static void stub_152(CPU*c){ stub_note("KERNEL32.dll!GetModuleHandleA"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_VerifyVersionInfoA(CPU*c) __attribute__((weak));
static void stub_153(CPU*c){ stub_note("KERNEL32.dll!VerifyVersionInfoA"); c->eax=0; c->esp+=4+16; }
void hle_kernel32_ExpandEnvironmentStringsA(CPU*c) __attribute__((weak));
static void stub_154(CPU*c){ stub_note("KERNEL32.dll!ExpandEnvironmentStringsA"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_SleepEx(CPU*c) __attribute__((weak));
static void stub_155(CPU*c){ stub_note("KERNEL32.dll!SleepEx"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_FormatMessageA(CPU*c) __attribute__((weak));
static void stub_156(CPU*c){ stub_note("KERNEL32.dll!FormatMessageA"); c->eax=0; c->esp+=4+28; }
void hle_kernel32_SetLastError(CPU*c) __attribute__((weak));
static void stub_157(CPU*c){ stub_note("KERNEL32.dll!SetLastError"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetTickCount(CPU*c) __attribute__((weak));
static void stub_158(CPU*c){ stub_note("KERNEL32.dll!GetTickCount"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_DeleteCriticalSection(CPU*c) __attribute__((weak));
static void stub_159(CPU*c){ stub_note("KERNEL32.dll!DeleteCriticalSection"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_SwitchToThread(CPU*c) __attribute__((weak));
static void stub_160(CPU*c){ stub_note("KERNEL32.dll!SwitchToThread"); c->eax=0; c->esp+=4+0; }
void hle_kernel32_MultiByteToWideChar(CPU*c) __attribute__((weak));
static void stub_161(CPU*c){ stub_note("KERNEL32.dll!MultiByteToWideChar"); c->eax=0; c->esp+=4+24; }
void hle_kernel32_GetThreadPriority(CPU*c) __attribute__((weak));
static void stub_162(CPU*c){ stub_note("KERNEL32.dll!GetThreadPriority"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_InitializeSListHead(CPU*c) __attribute__((weak));
static void stub_163(CPU*c){ stub_note("KERNEL32.dll!InitializeSListHead"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_InterlockedPushEntrySList(CPU*c) __attribute__((weak));
static void stub_164(CPU*c){ stub_note("KERNEL32.dll!InterlockedPushEntrySList"); c->eax=0; c->esp+=4+8; }
void hle_kernel32_InterlockedPopEntrySList(CPU*c) __attribute__((weak));
static void stub_165(CPU*c){ stub_note("KERNEL32.dll!InterlockedPopEntrySList"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_InterlockedFlushSList(CPU*c) __attribute__((weak));
static void stub_166(CPU*c){ stub_note("KERNEL32.dll!InterlockedFlushSList"); c->eax=0; c->esp+=4+4; }
void hle_kernel32_GetModuleFileNameW(CPU*c) __attribute__((weak));
static void stub_167(CPU*c){ stub_note("KERNEL32.dll!GetModuleFileNameW"); c->eax=0; c->esp+=4+12; }
void hle_kernel32_GetFullPathNameA(CPU*c) __attribute__((weak));
static void stub_168(CPU*c){ stub_note("KERNEL32.dll!GetFullPathNameA"); c->eax=0; c->esp+=4+16; }
void hle_user32_SetWindowsHookExW(CPU*c) __attribute__((weak));
static void stub_169(CPU*c){ stub_note("USER32.dll!SetWindowsHookExW"); c->eax=0; c->esp+=4+16; }
void hle_user32_ScreenToClient(CPU*c) __attribute__((weak));
static void stub_170(CPU*c){ stub_note("USER32.dll!ScreenToClient"); c->eax=0; c->esp+=4+8; }
void hle_user32_SetCursor(CPU*c) __attribute__((weak));
static void stub_171(CPU*c){ stub_note("USER32.dll!SetCursor"); c->eax=0; c->esp+=4+4; }
void hle_user32_SetCapture(CPU*c) __attribute__((weak));
static void stub_172(CPU*c){ stub_note("USER32.dll!SetCapture"); c->eax=0; c->esp+=4+4; }
void hle_user32_SetCursorPos(CPU*c) __attribute__((weak));
static void stub_173(CPU*c){ stub_note("USER32.dll!SetCursorPos"); c->eax=0; c->esp+=4+8; }
void hle_user32_GetCursorPos(CPU*c) __attribute__((weak));
static void stub_174(CPU*c){ stub_note("USER32.dll!GetCursorPos"); c->eax=0; c->esp+=4+4; }
void hle_user32_RegisterDeviceNotificationW(CPU*c) __attribute__((weak));
static void stub_175(CPU*c){ stub_note("USER32.dll!RegisterDeviceNotificationW"); c->eax=0; c->esp+=4+12; }
void hle_user32_UnregisterDeviceNotification(CPU*c) __attribute__((weak));
static void stub_176(CPU*c){ stub_note("USER32.dll!UnregisterDeviceNotification"); c->eax=0; c->esp+=4+4; }
void hle_user32_CallWindowProcW(CPU*c) __attribute__((weak));
static void stub_177(CPU*c){ stub_note("USER32.dll!CallWindowProcW"); c->eax=0; c->esp+=4+20; }
void hle_user32_GetMessageA(CPU*c) __attribute__((weak));
static void stub_178(CPU*c){ stub_note("USER32.dll!GetMessageA"); c->eax=0; c->esp+=4+16; }
void hle_user32_IsDialogMessageA(CPU*c) __attribute__((weak));
static void stub_179(CPU*c){ stub_note("USER32.dll!IsDialogMessageA"); c->eax=0; c->esp+=4+8; }
void hle_user32_DispatchMessageA(CPU*c) __attribute__((weak));
static void stub_180(CPU*c){ stub_note("USER32.dll!DispatchMessageA"); c->eax=0; c->esp+=4+4; }
void hle_user32_GetCursorInfo(CPU*c) __attribute__((weak));
static void stub_181(CPU*c){ stub_note("USER32.dll!GetCursorInfo"); c->eax=0; c->esp+=4+4; }
void hle_user32_GetClipCursor(CPU*c) __attribute__((weak));
static void stub_182(CPU*c){ stub_note("USER32.dll!GetClipCursor"); c->eax=0; c->esp+=4+4; }
void hle_user32_CallNextHookEx(CPU*c) __attribute__((weak));
static void stub_183(CPU*c){ stub_note("USER32.dll!CallNextHookEx"); c->eax=0; c->esp+=4+16; }
void hle_user32_GetForegroundWindow(CPU*c) __attribute__((weak));
static void stub_184(CPU*c){ stub_note("USER32.dll!GetForegroundWindow"); c->eax=0; c->esp+=4+0; }
void hle_user32_PeekMessageA(CPU*c) __attribute__((weak));
static void stub_185(CPU*c){ stub_note("USER32.dll!PeekMessageA"); c->eax=0; c->esp+=4+20; }
void hle_user32_DefWindowProcA(CPU*c) __attribute__((weak));
static void stub_186(CPU*c){ stub_note("USER32.dll!DefWindowProcA"); c->eax=0; c->esp+=4+16; }
void hle_user32_MapWindowPoints(CPU*c) __attribute__((weak));
static void stub_187(CPU*c){ stub_note("USER32.dll!MapWindowPoints"); c->eax=0; c->esp+=4+16; }
void hle_user32_MoveWindow(CPU*c) __attribute__((weak));
static void stub_188(CPU*c){ stub_note("USER32.dll!MoveWindow"); c->eax=0; c->esp+=4+24; }
void hle_user32_GetDesktopWindow(CPU*c) __attribute__((weak));
static void stub_189(CPU*c){ stub_note("USER32.dll!GetDesktopWindow"); c->eax=0; c->esp+=4+0; }
void hle_user32_SetWindowLongA(CPU*c) __attribute__((weak));
static void stub_190(CPU*c){ stub_note("USER32.dll!SetWindowLongA"); c->eax=0; c->esp+=4+12; }
void hle_user32_LoadCursorA(CPU*c) __attribute__((weak));
static void stub_191(CPU*c){ stub_note("USER32.dll!LoadCursorA"); c->eax=0; c->esp+=4+8; }
void hle_user32_RegisterClassExA(CPU*c) __attribute__((weak));
static void stub_192(CPU*c){ stub_note("USER32.dll!RegisterClassExA"); c->eax=0; c->esp+=4+4; }
void hle_user32_GetWindowLongA(CPU*c) __attribute__((weak));
static void stub_193(CPU*c){ stub_note("USER32.dll!GetWindowLongA"); c->eax=0; c->esp+=4+8; }
void hle_user32_ValidateRect(CPU*c) __attribute__((weak));
static void stub_194(CPU*c){ stub_note("USER32.dll!ValidateRect"); c->eax=0; c->esp+=4+8; }
void hle_user32_CreateWindowExA(CPU*c) __attribute__((weak));
static void stub_195(CPU*c){ stub_note("USER32.dll!CreateWindowExA"); c->eax=0; c->esp+=4+48; }
void hle_user32_PostQuitMessage(CPU*c) __attribute__((weak));
static void stub_196(CPU*c){ stub_note("USER32.dll!PostQuitMessage"); c->eax=0; c->esp+=4+4; }
void hle_user32_DefWindowProcW(CPU*c) __attribute__((weak));
static void stub_197(CPU*c){ stub_note("USER32.dll!DefWindowProcW"); c->eax=0; c->esp+=4+16; }
void hle_user32_TranslateMessage(CPU*c) __attribute__((weak));
static void stub_198(CPU*c){ stub_note("USER32.dll!TranslateMessage"); c->eax=0; c->esp+=4+4; }
void hle_user32_DispatchMessageW(CPU*c) __attribute__((weak));
static void stub_199(CPU*c){ stub_note("USER32.dll!DispatchMessageW"); c->eax=0; c->esp+=4+4; }
void hle_user32_PeekMessageW(CPU*c) __attribute__((weak));
static void stub_200(CPU*c){ stub_note("USER32.dll!PeekMessageW"); c->eax=0; c->esp+=4+20; }
void hle_user32_UpdateWindow(CPU*c) __attribute__((weak));
static void stub_201(CPU*c){ stub_note("USER32.dll!UpdateWindow"); c->eax=0; c->esp+=4+4; }
void hle_user32_SetWindowPlacement(CPU*c) __attribute__((weak));
static void stub_202(CPU*c){ stub_note("USER32.dll!SetWindowPlacement"); c->eax=0; c->esp+=4+8; }
void hle_user32_GetWindowPlacement(CPU*c) __attribute__((weak));
static void stub_203(CPU*c){ stub_note("USER32.dll!GetWindowPlacement"); c->eax=0; c->esp+=4+8; }
void hle_user32_SetWindowLongW(CPU*c) __attribute__((weak));
static void stub_204(CPU*c){ stub_note("USER32.dll!SetWindowLongW"); c->eax=0; c->esp+=4+12; }
void hle_user32_GetMonitorInfoW(CPU*c) __attribute__((weak));
static void stub_205(CPU*c){ stub_note("USER32.dll!GetMonitorInfoW"); c->eax=0; c->esp+=4+8; }
void hle_user32_MonitorFromWindow(CPU*c) __attribute__((weak));
static void stub_206(CPU*c){ stub_note("USER32.dll!MonitorFromWindow"); c->eax=0; c->esp+=4+8; }
void hle_user32_SendInput(CPU*c) __attribute__((weak));
static void stub_207(CPU*c){ stub_note("USER32.dll!SendInput"); c->eax=0; c->esp+=4+12; }
void hle_user32_GetWindowLongW(CPU*c) __attribute__((weak));
static void stub_208(CPU*c){ stub_note("USER32.dll!GetWindowLongW"); c->eax=0; c->esp+=4+8; }
void hle_user32_IsIconic(CPU*c) __attribute__((weak));
static void stub_209(CPU*c){ stub_note("USER32.dll!IsIconic"); c->eax=0; c->esp+=4+4; }
void hle_user32_UnregisterClassA(CPU*c) __attribute__((weak));
static void stub_210(CPU*c){ stub_note("USER32.dll!UnregisterClassA"); c->eax=0; c->esp+=4+8; }
void hle_user32_GetCapture(CPU*c) __attribute__((weak));
static void stub_211(CPU*c){ stub_note("USER32.dll!GetCapture"); c->eax=0; c->esp+=4+0; }
void hle_user32_ClientToScreen(CPU*c) __attribute__((weak));
static void stub_212(CPU*c){ stub_note("USER32.dll!ClientToScreen"); c->eax=0; c->esp+=4+8; }
void hle_user32_GetFocus(CPU*c) __attribute__((weak));
static void stub_213(CPU*c){ stub_note("USER32.dll!GetFocus"); c->eax=0; c->esp+=4+0; }
void hle_user32_CloseClipboard(CPU*c) __attribute__((weak));
static void stub_214(CPU*c){ stub_note("USER32.dll!CloseClipboard"); c->eax=0; c->esp+=4+0; }
void hle_user32_GetClipboardData(CPU*c) __attribute__((weak));
static void stub_215(CPU*c){ stub_note("USER32.dll!GetClipboardData"); c->eax=0; c->esp+=4+4; }
void hle_user32_OpenClipboard(CPU*c) __attribute__((weak));
static void stub_216(CPU*c){ stub_note("USER32.dll!OpenClipboard"); c->eax=0; c->esp+=4+4; }
void hle_user32_GetActiveWindow(CPU*c) __attribute__((weak));
static void stub_217(CPU*c){ stub_note("USER32.dll!GetActiveWindow"); c->eax=0; c->esp+=4+0; }
void hle_user32_MessageBoxW(CPU*c) __attribute__((weak));
static void stub_218(CPU*c){ stub_note("USER32.dll!MessageBoxW"); c->eax=0; c->esp+=4+16; }
void hle_user32_LoadIconW(CPU*c) __attribute__((weak));
static void stub_219(CPU*c){ stub_note("USER32.dll!LoadIconW"); c->eax=0; c->esp+=4+8; }
void hle_user32_DestroyWindow(CPU*c) __attribute__((weak));
static void stub_220(CPU*c){ stub_note("USER32.dll!DestroyWindow"); c->eax=0; c->esp+=4+4; }
void hle_user32_CreateWindowExW(CPU*c) __attribute__((weak));
static void stub_221(CPU*c){ stub_note("USER32.dll!CreateWindowExW"); c->eax=0; c->esp+=4+48; }
void hle_user32_RegisterClassW(CPU*c) __attribute__((weak));
static void stub_222(CPU*c){ stub_note("USER32.dll!RegisterClassW"); c->eax=0; c->esp+=4+4; }
void hle_user32_SetClipboardData(CPU*c) __attribute__((weak));
static void stub_223(CPU*c){ stub_note("USER32.dll!SetClipboardData"); c->eax=0; c->esp+=4+8; }
void hle_user32_EmptyClipboard(CPU*c) __attribute__((weak));
static void stub_224(CPU*c){ stub_note("USER32.dll!EmptyClipboard"); c->eax=0; c->esp+=4+0; }
void hle_user32_ShowWindow(CPU*c) __attribute__((weak));
static void stub_225(CPU*c){ stub_note("USER32.dll!ShowWindow"); c->eax=0; c->esp+=4+8; }
void hle_user32_RegisterClassExW(CPU*c) __attribute__((weak));
static void stub_226(CPU*c){ stub_note("USER32.dll!RegisterClassExW"); c->eax=0; c->esp+=4+4; }
void hle_user32_EnableWindow(CPU*c) __attribute__((weak));
static void stub_227(CPU*c){ stub_note("USER32.dll!EnableWindow"); c->eax=0; c->esp+=4+8; }
void hle_user32_IsWindowEnabled(CPU*c) __attribute__((weak));
static void stub_228(CPU*c){ stub_note("USER32.dll!IsWindowEnabled"); c->eax=0; c->esp+=4+4; }
void hle_user32_GetWindowThreadProcessId(CPU*c) __attribute__((weak));
static void stub_229(CPU*c){ stub_note("USER32.dll!GetWindowThreadProcessId"); c->eax=0; c->esp+=4+8; }
void hle_user32_EnumWindows(CPU*c) __attribute__((weak));
static void stub_230(CPU*c){ stub_note("USER32.dll!EnumWindows"); c->eax=0; c->esp+=4+8; }
void hle_user32_GetAncestor(CPU*c) __attribute__((weak));
static void stub_231(CPU*c){ stub_note("USER32.dll!GetAncestor"); c->eax=0; c->esp+=4+8; }
void hle_user32_ClipCursor(CPU*c) __attribute__((weak));
static void stub_232(CPU*c){ stub_note("USER32.dll!ClipCursor"); c->eax=0; c->esp+=4+4; }
void hle_user32_ReleaseCapture(CPU*c) __attribute__((weak));
static void stub_233(CPU*c){ stub_note("USER32.dll!ReleaseCapture"); c->eax=0; c->esp+=4+0; }
void hle_user32_ShowCursor(CPU*c) __attribute__((weak));
static void stub_234(CPU*c){ stub_note("USER32.dll!ShowCursor"); c->eax=0; c->esp+=4+4; }
void hle_user32_SystemParametersInfoW(CPU*c) __attribute__((weak));
static void stub_235(CPU*c){ stub_note("USER32.dll!SystemParametersInfoW"); c->eax=0; c->esp+=4+16; }
void hle_user32_GetSystemMetrics(CPU*c) __attribute__((weak));
static void stub_236(CPU*c){ stub_note("USER32.dll!GetSystemMetrics"); c->eax=0; c->esp+=4+4; }
void hle_user32_GetWindowRect(CPU*c) __attribute__((weak));
static void stub_237(CPU*c){ stub_note("USER32.dll!GetWindowRect"); c->eax=0; c->esp+=4+8; }
void hle_user32_DeleteMenu(CPU*c) __attribute__((weak));
static void stub_238(CPU*c){ stub_note("USER32.dll!DeleteMenu"); c->eax=0; c->esp+=4+12; }
void hle_user32_GetSystemMenu(CPU*c) __attribute__((weak));
static void stub_239(CPU*c){ stub_note("USER32.dll!GetSystemMenu"); c->eax=0; c->esp+=4+8; }
void hle_user32_SetWindowPos(CPU*c) __attribute__((weak));
static void stub_240(CPU*c){ stub_note("USER32.dll!SetWindowPos"); c->eax=0; c->esp+=4+28; }
void hle_user32_PostMessageW(CPU*c) __attribute__((weak));
static void stub_241(CPU*c){ stub_note("USER32.dll!PostMessageW"); c->eax=0; c->esp+=4+16; }
void hle_user32_EndPaint(CPU*c) __attribute__((weak));
static void stub_242(CPU*c){ stub_note("USER32.dll!EndPaint"); c->eax=0; c->esp+=4+8; }
void hle_user32_GetClientRect(CPU*c) __attribute__((weak));
static void stub_243(CPU*c){ stub_note("USER32.dll!GetClientRect"); c->eax=0; c->esp+=4+8; }
void hle_user32_DrawStateW(CPU*c) __attribute__((weak));
static void stub_244(CPU*c){ stub_note("USER32.dll!DrawStateW"); c->eax=0; c->esp+=4+40; }
void hle_user32_BeginPaint(CPU*c) __attribute__((weak));
static void stub_245(CPU*c){ stub_note("USER32.dll!BeginPaint"); c->eax=0; c->esp+=4+8; }
void hle_user32_InvalidateRect(CPU*c) __attribute__((weak));
static void stub_246(CPU*c){ stub_note("USER32.dll!InvalidateRect"); c->eax=0; c->esp+=4+12; }
void hle_user32_UnregisterClassW(CPU*c) __attribute__((weak));
static void stub_247(CPU*c){ stub_note("USER32.dll!UnregisterClassW"); c->eax=0; c->esp+=4+8; }
void hle_user32_GetMessageW(CPU*c) __attribute__((weak));
static void stub_248(CPU*c){ stub_note("USER32.dll!GetMessageW"); c->eax=0; c->esp+=4+16; }
void hle_user32_LoadImageW(CPU*c) __attribute__((weak));
static void stub_249(CPU*c){ stub_note("USER32.dll!LoadImageW"); c->eax=0; c->esp+=4+24; }
void hle_user32_GetKeyboardLayoutNameW(CPU*c) __attribute__((weak));
static void stub_250(CPU*c){ stub_note("USER32.dll!GetKeyboardLayoutNameW"); c->eax=0; c->esp+=4+4; }
void hle_user32_EnumDisplayDevicesW(CPU*c) __attribute__((weak));
static void stub_251(CPU*c){ stub_note("USER32.dll!EnumDisplayDevicesW"); c->eax=0; c->esp+=4+16; }
void hle_user32_SetActiveWindow(CPU*c) __attribute__((weak));
static void stub_252(CPU*c){ stub_note("USER32.dll!SetActiveWindow"); c->eax=0; c->esp+=4+4; }
void hle_user32_SetFocus(CPU*c) __attribute__((weak));
static void stub_253(CPU*c){ stub_note("USER32.dll!SetFocus"); c->eax=0; c->esp+=4+4; }
void hle_user32_AdjustWindowRect(CPU*c) __attribute__((weak));
static void stub_254(CPU*c){ stub_note("USER32.dll!AdjustWindowRect"); c->eax=0; c->esp+=4+12; }
void hle_user32_SetWindowTextW(CPU*c) __attribute__((weak));
static void stub_255(CPU*c){ stub_note("USER32.dll!SetWindowTextW"); c->eax=0; c->esp+=4+8; }
void hle_user32_PostThreadMessageW(CPU*c) __attribute__((weak));
static void stub_256(CPU*c){ stub_note("USER32.dll!PostThreadMessageW"); c->eax=0; c->esp+=4+16; }
void hle_user32_GetKeyState(CPU*c) __attribute__((weak));
static void stub_257(CPU*c){ stub_note("USER32.dll!GetKeyState"); c->eax=0; c->esp+=4+4; }
void hle_user32_UnhookWindowsHookEx(CPU*c) __attribute__((weak));
static void stub_258(CPU*c){ stub_note("USER32.dll!UnhookWindowsHookEx"); c->eax=0; c->esp+=4+4; }
void hle_user32_LoadCursorW(CPU*c) __attribute__((weak));
static void stub_259(CPU*c){ stub_note("USER32.dll!LoadCursorW"); c->eax=0; c->esp+=4+8; }
void hle_advapi32_GetUserNameW(CPU*c) __attribute__((weak));
static void stub_260(CPU*c){ stub_note("ADVAPI32.dll!GetUserNameW"); c->eax=0; c->esp+=4+8; }
void hle_advapi32_CryptGetHashParam(CPU*c) __attribute__((weak));
static void stub_261(CPU*c){ stub_note("ADVAPI32.dll!CryptGetHashParam"); c->eax=0; c->esp+=4+20; }
void hle_advapi32_CryptHashData(CPU*c) __attribute__((weak));
static void stub_262(CPU*c){ stub_note("ADVAPI32.dll!CryptHashData"); c->eax=0; c->esp+=4+16; }
void hle_advapi32_CryptReleaseContext(CPU*c) __attribute__((weak));
static void stub_263(CPU*c){ stub_note("ADVAPI32.dll!CryptReleaseContext"); c->eax=0; c->esp+=4+8; }
void hle_advapi32_CryptDestroyKey(CPU*c) __attribute__((weak));
static void stub_264(CPU*c){ stub_note("ADVAPI32.dll!CryptDestroyKey"); c->eax=0; c->esp+=4+4; }
void hle_advapi32_CryptDestroyHash(CPU*c) __attribute__((weak));
static void stub_265(CPU*c){ stub_note("ADVAPI32.dll!CryptDestroyHash"); c->eax=0; c->esp+=4+4; }
void hle_advapi32_CryptVerifySignatureW(CPU*c) __attribute__((weak));
static void stub_266(CPU*c){ stub_note("ADVAPI32.dll!CryptVerifySignatureW"); c->eax=0; c->esp+=4+24; }
void hle_advapi32_CryptCreateHash(CPU*c) __attribute__((weak));
static void stub_267(CPU*c){ stub_note("ADVAPI32.dll!CryptCreateHash"); c->eax=0; c->esp+=4+20; }
void hle_advapi32_CryptImportKey(CPU*c) __attribute__((weak));
static void stub_268(CPU*c){ stub_note("ADVAPI32.dll!CryptImportKey"); c->eax=0; c->esp+=4+24; }
void hle_advapi32_CryptAcquireContextW(CPU*c) __attribute__((weak));
static void stub_269(CPU*c){ stub_note("ADVAPI32.dll!CryptAcquireContextW"); c->eax=0; c->esp+=4+20; }
void hle_advapi32_CryptAcquireContextA(CPU*c) __attribute__((weak));
static void stub_270(CPU*c){ stub_note("ADVAPI32.dll!CryptAcquireContextA"); c->eax=0; c->esp+=4+20; }
void hle_advapi32_CryptGenRandom(CPU*c) __attribute__((weak));
static void stub_271(CPU*c){ stub_note("ADVAPI32.dll!CryptGenRandom"); c->eax=0; c->esp+=4+12; }
void hle_gdi32_SelectObject(CPU*c) __attribute__((weak));
static void stub_272(CPU*c){ stub_note("GDI32.dll!SelectObject"); c->eax=0; c->esp+=4+8; }
void hle_gdi32_TextOutW(CPU*c) __attribute__((weak));
static void stub_273(CPU*c){ stub_note("GDI32.dll!TextOutW"); c->eax=0; c->esp+=4+20; }
void hle_gdi32_SetTextColor(CPU*c) __attribute__((weak));
static void stub_274(CPU*c){ stub_note("GDI32.dll!SetTextColor"); c->eax=0; c->esp+=4+8; }
void hle_gdi32_SetBkMode(CPU*c) __attribute__((weak));
static void stub_275(CPU*c){ stub_note("GDI32.dll!SetBkMode"); c->eax=0; c->esp+=4+8; }
void hle_gdi32_GetStockObject(CPU*c) __attribute__((weak));
static void stub_276(CPU*c){ stub_note("GDI32.dll!GetStockObject"); c->eax=0; c->esp+=4+4; }
void hle_gdi32_GetObjectW(CPU*c) __attribute__((weak));
static void stub_277(CPU*c){ stub_note("GDI32.dll!GetObjectW"); c->eax=0; c->esp+=4+12; }
void hle_gdi32_CreateFontIndirectW(CPU*c) __attribute__((weak));
static void stub_278(CPU*c){ stub_note("GDI32.dll!CreateFontIndirectW"); c->eax=0; c->esp+=4+4; }
void hle_gdi32_DeleteObject(CPU*c) __attribute__((weak));
static void stub_279(CPU*c){ stub_note("GDI32.dll!DeleteObject"); c->eax=0; c->esp+=4+4; }
void hle_gdi32_SetBkColor(CPU*c) __attribute__((weak));
static void stub_280(CPU*c){ stub_note("GDI32.dll!SetBkColor"); c->eax=0; c->esp+=4+8; }
void hle_steam_api_SteamAPI_SetMiniDumpComment(CPU*c) __attribute__((weak));
static void stub_281(CPU*c){ stub_note("steam_api.dll!SteamAPI_SetMiniDumpComment"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_WriteMiniDump(CPU*c) __attribute__((weak));
static void stub_282(CPU*c){ stub_note("steam_api.dll!SteamAPI_WriteMiniDump"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_RunCallbacks(CPU*c) __attribute__((weak));
static void stub_283(CPU*c){ stub_note("steam_api.dll!SteamAPI_RunCallbacks"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamGameServer_RunCallbacks(CPU*c) __attribute__((weak));
static void stub_284(CPU*c){ stub_note("steam_api.dll!SteamGameServer_RunCallbacks"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_Init(CPU*c) __attribute__((weak));
static void stub_285(CPU*c){ stub_note("steam_api.dll!SteamAPI_Init"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_RegisterCallback(CPU*c) __attribute__((weak));
static void stub_286(CPU*c){ stub_note("steam_api.dll!SteamAPI_RegisterCallback"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_UnregisterCallResult(CPU*c) __attribute__((weak));
static void stub_287(CPU*c){ stub_note("steam_api.dll!SteamAPI_UnregisterCallResult"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_RegisterCallResult(CPU*c) __attribute__((weak));
static void stub_288(CPU*c){ stub_note("steam_api.dll!SteamAPI_RegisterCallResult"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_UnregisterCallback(CPU*c) __attribute__((weak));
static void stub_289(CPU*c){ stub_note("steam_api.dll!SteamAPI_UnregisterCallback"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamGameServer_Shutdown(CPU*c) __attribute__((weak));
static void stub_290(CPU*c){ stub_note("steam_api.dll!SteamGameServer_Shutdown"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_Shutdown(CPU*c) __attribute__((weak));
static void stub_291(CPU*c){ stub_note("steam_api.dll!SteamAPI_Shutdown"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamGameServer_BSecure(CPU*c) __attribute__((weak));
static void stub_292(CPU*c){ stub_note("steam_api.dll!SteamGameServer_BSecure"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamGameServer_GetSteamID(CPU*c) __attribute__((weak));
static void stub_293(CPU*c){ stub_note("steam_api.dll!SteamGameServer_GetSteamID"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamInternal_GameServer_Init(CPU*c) __attribute__((weak));
static void stub_294(CPU*c){ stub_note("steam_api.dll!SteamInternal_GameServer_Init"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamGameServer_GetHSteamUser(CPU*c) __attribute__((weak));
static void stub_295(CPU*c){ stub_note("steam_api.dll!SteamGameServer_GetHSteamUser"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamInternal_FindOrCreateGameServerInterface(CPU*c) __attribute__((weak));
static void stub_296(CPU*c){ stub_note("steam_api.dll!SteamInternal_FindOrCreateGameServerInterface"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamAPI_GetHSteamUser(CPU*c) __attribute__((weak));
static void stub_297(CPU*c){ stub_note("steam_api.dll!SteamAPI_GetHSteamUser"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamInternal_CreateInterface(CPU*c) __attribute__((weak));
static void stub_298(CPU*c){ stub_note("steam_api.dll!SteamInternal_CreateInterface"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamInternal_ContextInit(CPU*c) __attribute__((weak));
static void stub_299(CPU*c){ stub_note("steam_api.dll!SteamInternal_ContextInit"); c->eax=0; c->esp+=4+0; }
void hle_steam_api_SteamInternal_FindOrCreateUserInterface(CPU*c) __attribute__((weak));
static void stub_300(CPU*c){ stub_note("steam_api.dll!SteamInternal_FindOrCreateUserInterface"); c->eax=0; c->esp+=4+0; }
void hle_dinput8_DirectInput8Create(CPU*c) __attribute__((weak));
static void stub_301(CPU*c){ stub_note("DINPUT8.dll!DirectInput8Create"); c->eax=0; c->esp+=4+20; }
void hle_xinput1_3_ord2(CPU*c) __attribute__((weak));
static void stub_302(CPU*c){ stub_note("XINPUT1_3.dll!ord2"); c->eax=0; c->esp+=4+8; }
void hle_xinput1_3_ord3(CPU*c) __attribute__((weak));
static void stub_303(CPU*c){ stub_note("XINPUT1_3.dll!ord3"); c->eax=0; c->esp+=4+8; }
void hle_d3d9_D3DPERF_BeginEvent(CPU*c) __attribute__((weak));
static void stub_304(CPU*c){ stub_note("d3d9.dll!D3DPERF_BeginEvent"); c->eax=0; c->esp+=4+8; }
void hle_d3d9_D3DPERF_EndEvent(CPU*c) __attribute__((weak));
static void stub_305(CPU*c){ stub_note("d3d9.dll!D3DPERF_EndEvent"); c->eax=0; c->esp+=4+0; }
void hle_d3d9_Direct3DCreate9(CPU*c) __attribute__((weak));
static void stub_306(CPU*c){ stub_note("d3d9.dll!Direct3DCreate9"); c->eax=0; c->esp+=4+4; }
void hle_d3dx9_43_D3DXMatrixLookAtRH(CPU*c) __attribute__((weak));
static void stub_307(CPU*c){ stub_note("d3dx9_43.dll!D3DXMatrixLookAtRH"); c->eax=0; c->esp+=4+16; }
void hle_d3dx9_43_D3DXMatrixTranslation(CPU*c) __attribute__((weak));
static void stub_308(CPU*c){ stub_note("d3dx9_43.dll!D3DXMatrixTranslation"); c->eax=0; c->esp+=4+16; }
void hle_d3dx9_43_D3DXMatrixMultiply(CPU*c) __attribute__((weak));
static void stub_309(CPU*c){ stub_note("d3dx9_43.dll!D3DXMatrixMultiply"); c->eax=0; c->esp+=4+12; }
void hle_d3dx9_43_D3DXMatrixScaling(CPU*c) __attribute__((weak));
static void stub_310(CPU*c){ stub_note("d3dx9_43.dll!D3DXMatrixScaling"); c->eax=0; c->esp+=4+16; }
void hle_d3dx9_43_D3DXMatrixInverse(CPU*c) __attribute__((weak));
static void stub_311(CPU*c){ stub_note("d3dx9_43.dll!D3DXMatrixInverse"); c->eax=0; c->esp+=4+12; }
void hle_d3dx9_43_D3DXMatrixOrthoOffCenterRH(CPU*c) __attribute__((weak));
static void stub_312(CPU*c){ stub_note("d3dx9_43.dll!D3DXMatrixOrthoOffCenterRH"); c->eax=0; c->esp+=4+28; }
void hle_d3dx9_43_D3DXOptimizeFaces(CPU*c) __attribute__((weak));
static void stub_313(CPU*c){ stub_note("d3dx9_43.dll!D3DXOptimizeFaces"); c->eax=0; c->esp+=4+20; }
void hle_d3dx9_43_D3DXVec3Transform(CPU*c) __attribute__((weak));
static void stub_314(CPU*c){ stub_note("d3dx9_43.dll!D3DXVec3Transform"); c->eax=0; c->esp+=4+12; }
void hle_d3dx11_43_D3DX11CheckVersion(CPU*c) __attribute__((weak));
static void stub_315(CPU*c){ stub_note("d3dx11_43.dll!D3DX11CheckVersion"); c->eax=0; c->esp+=4+8; }
void hle_nvtt___0InputOptions_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_316(CPU*c){ stub_note("nvtt.dll!??0InputOptions@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__setTextureLayout_InputOptions_nvtt__QAEXW4TextureType_2_HHH_Z(CPU*c) __attribute__((weak));
static void stub_317(CPU*c){ stub_note("nvtt.dll!?setTextureLayout@InputOptions@nvtt@@QAEXW4TextureType@2@HHH@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__setMipmapGeneration_InputOptions_nvtt__QAEX_NH_Z(CPU*c) __attribute__((weak));
static void stub_318(CPU*c){ stub_note("nvtt.dll!?setMipmapGeneration@InputOptions@nvtt@@QAEX_NH@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__setMipmapData_InputOptions_nvtt__QAE_NPBXHHHHH_Z(CPU*c) __attribute__((weak));
static void stub_319(CPU*c){ stub_note("nvtt.dll!?setMipmapData@InputOptions@nvtt@@QAE_NPBXHHHHH@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt___0CompressionOptions_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_320(CPU*c){ stub_note("nvtt.dll!??0CompressionOptions@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__setQuality_CompressionOptions_nvtt__QAEXW4Quality_2__Z(CPU*c) __attribute__((weak));
static void stub_321(CPU*c){ stub_note("nvtt.dll!?setQuality@CompressionOptions@nvtt@@QAEXW4Quality@2@@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt___1CompressionOptions_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_322(CPU*c){ stub_note("nvtt.dll!??1CompressionOptions@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt___1Compressor_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_323(CPU*c){ stub_note("nvtt.dll!??1Compressor@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt___1OutputOptions_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_324(CPU*c){ stub_note("nvtt.dll!??1OutputOptions@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__process_Compressor_nvtt__QBE_NABUInputOptions_2_ABUCompressionOptions_2_ABUOutputOptions_2__Z(CPU*c) __attribute__((weak));
static void stub_325(CPU*c){ stub_note("nvtt.dll!?process@Compressor@nvtt@@QBE_NABUInputOptions@2@ABUCompressionOptions@2@ABUOutputOptions@2@@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__setErrorHandler_OutputOptions_nvtt__QAEXPAUErrorHandler_2__Z(CPU*c) __attribute__((weak));
static void stub_326(CPU*c){ stub_note("nvtt.dll!?setErrorHandler@OutputOptions@nvtt@@QAEXPAUErrorHandler@2@@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__setFormat_CompressionOptions_nvtt__QAEXW4Format_2__Z(CPU*c) __attribute__((weak));
static void stub_327(CPU*c){ stub_note("nvtt.dll!?setFormat@CompressionOptions@nvtt@@QAEXW4Format@2@@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt___1InputOptions_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_328(CPU*c){ stub_note("nvtt.dll!??1InputOptions@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt___0Compressor_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_329(CPU*c){ stub_note("nvtt.dll!??0Compressor@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__enableCudaAcceleration_Compressor_nvtt__QAEX_N_Z(CPU*c) __attribute__((weak));
static void stub_330(CPU*c){ stub_note("nvtt.dll!?enableCudaAcceleration@Compressor@nvtt@@QAEX_N@Z"); c->eax=0; c->esp+=4+0; }
void hle_nvtt___0OutputOptions_nvtt__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_331(CPU*c){ stub_note("nvtt.dll!??0OutputOptions@nvtt@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_nvtt__setOutputHandler_OutputOptions_nvtt__QAEXPAUOutputHandler_2__Z(CPU*c) __attribute__((weak));
static void stub_332(CPU*c){ stub_note("nvtt.dll!?setOutputHandler@OutputOptions@nvtt@@QAEXPAUOutputHandler@2@@Z"); c->eax=0; c->esp+=4+0; }
void hle_dbghelp_StackWalk64(CPU*c) __attribute__((weak));
static void stub_333(CPU*c){ stub_note("dbghelp.dll!StackWalk64"); c->eax=0; c->esp+=4+36; }
void hle_dbghelp_SymFunctionTableAccess64(CPU*c) __attribute__((weak));
static void stub_334(CPU*c){ stub_note("dbghelp.dll!SymFunctionTableAccess64"); c->eax=0; c->esp+=4+0; }
void hle_dbghelp_SymGetModuleBase64(CPU*c) __attribute__((weak));
static void stub_335(CPU*c){ stub_note("dbghelp.dll!SymGetModuleBase64"); c->eax=0; c->esp+=4+0; }
void hle_dbghelp_SymLoadModule64(CPU*c) __attribute__((weak));
static void stub_336(CPU*c){ stub_note("dbghelp.dll!SymLoadModule64"); c->eax=0; c->esp+=4+28; }
void hle_dbghelp_SymGetModuleInfo64(CPU*c) __attribute__((weak));
static void stub_337(CPU*c){ stub_note("dbghelp.dll!SymGetModuleInfo64"); c->eax=0; c->esp+=4+16; }
void hle_dbghelp_SymGetSymFromAddr64(CPU*c) __attribute__((weak));
static void stub_338(CPU*c){ stub_note("dbghelp.dll!SymGetSymFromAddr64"); c->eax=0; c->esp+=4+20; }
void hle_dbghelp_MiniDumpWriteDump(CPU*c) __attribute__((weak));
static void stub_339(CPU*c){ stub_note("dbghelp.dll!MiniDumpWriteDump"); c->eax=0; c->esp+=4+28; }
void hle_dbghelp_SymSetSearchPath(CPU*c) __attribute__((weak));
static void stub_340(CPU*c){ stub_note("dbghelp.dll!SymSetSearchPath"); c->eax=0; c->esp+=4+8; }
void hle_dbghelp_SymGetLineFromAddr64(CPU*c) __attribute__((weak));
static void stub_341(CPU*c){ stub_note("dbghelp.dll!SymGetLineFromAddr64"); c->eax=0; c->esp+=4+20; }
void hle_dbghelp_SymInitialize(CPU*c) __attribute__((weak));
static void stub_342(CPU*c){ stub_note("dbghelp.dll!SymInitialize"); c->eax=0; c->esp+=4+12; }
void hle_dbghelp_SymSetOptions(CPU*c) __attribute__((weak));
static void stub_343(CPU*c){ stub_note("dbghelp.dll!SymSetOptions"); c->eax=0; c->esp+=4+4; }
void hle_dbghelp_SymGetOptions(CPU*c) __attribute__((weak));
static void stub_344(CPU*c){ stub_note("dbghelp.dll!SymGetOptions"); c->eax=0; c->esp+=4+0; }
void hle_winmm_timeBeginPeriod(CPU*c) __attribute__((weak));
static void stub_345(CPU*c){ stub_note("WINMM.dll!timeBeginPeriod"); c->eax=0; c->esp+=4+4; }
void hle_winmm_timeGetTime(CPU*c) __attribute__((weak));
static void stub_346(CPU*c){ stub_note("WINMM.dll!timeGetTime"); c->eax=0; c->esp+=4+0; }
void hle_shell32_SHGetFolderPathW(CPU*c) __attribute__((weak));
static void stub_347(CPU*c){ stub_note("SHELL32.dll!SHGetFolderPathW"); c->eax=0; c->esp+=4+20; }
void hle_shell32_ShellExecuteW(CPU*c) __attribute__((weak));
static void stub_348(CPU*c){ stub_note("SHELL32.dll!ShellExecuteW"); c->eax=0; c->esp+=4+24; }
void hle_ole32_CoSetProxyBlanket(CPU*c) __attribute__((weak));
static void stub_349(CPU*c){ stub_note("ole32.dll!CoSetProxyBlanket"); c->eax=0; c->esp+=4+32; }
void hle_ole32_CoInitialize(CPU*c) __attribute__((weak));
static void stub_350(CPU*c){ stub_note("ole32.dll!CoInitialize"); c->eax=0; c->esp+=4+4; }
void hle_ole32_CoCreateInstance(CPU*c) __attribute__((weak));
static void stub_351(CPU*c){ stub_note("ole32.dll!CoCreateInstance"); c->eax=0; c->esp+=4+20; }
void hle_ole32_CoUninitialize(CPU*c) __attribute__((weak));
static void stub_352(CPU*c){ stub_note("ole32.dll!CoUninitialize"); c->eax=0; c->esp+=4+0; }
void hle_ole32_CoCreateGuid(CPU*c) __attribute__((weak));
static void stub_353(CPU*c){ stub_note("ole32.dll!CoCreateGuid"); c->eax=0; c->esp+=4+4; }
void hle_ole32_CoInitializeEx(CPU*c) __attribute__((weak));
static void stub_354(CPU*c){ stub_note("ole32.dll!CoInitializeEx"); c->eax=0; c->esp+=4+8; }
void hle_ole32_StringFromGUID2(CPU*c) __attribute__((weak));
static void stub_355(CPU*c){ stub_note("ole32.dll!StringFromGUID2"); c->eax=0; c->esp+=4+12; }
void hle_oleaut32_SysFreeString(CPU*c) __attribute__((weak));
static void stub_356(CPU*c){ stub_note("OLEAUT32.dll!SysFreeString"); c->eax=0; c->esp+=4+4; }
void hle_oleaut32_SysAllocString(CPU*c) __attribute__((weak));
static void stub_357(CPU*c){ stub_note("OLEAUT32.dll!SysAllocString"); c->eax=0; c->esp+=4+4; }
void hle_shlwapi_PathRemoveBlanksW(CPU*c) __attribute__((weak));
static void stub_358(CPU*c){ stub_note("SHLWAPI.dll!PathRemoveBlanksW"); c->eax=0; c->esp+=4+4; }
void hle_shlwapi_PathMatchSpecW(CPU*c) __attribute__((weak));
static void stub_359(CPU*c){ stub_note("SHLWAPI.dll!PathMatchSpecW"); c->eax=0; c->esp+=4+8; }
void hle_shlwapi_PathFindFileNameW(CPU*c) __attribute__((weak));
static void stub_360(CPU*c){ stub_note("SHLWAPI.dll!PathFindFileNameW"); c->eax=0; c->esp+=4+4; }
void hle_msvcp90___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_ABV01__Z(CPU*c) __attribute__((weak));
static void stub_361(CPU*c){ stub_note("MSVCP90.dll!??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90___1__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_362(CPU*c){ stub_note("MSVCP90.dll!??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90__find___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QBEIPBDII_Z(CPU*c) __attribute__((weak));
static void stub_363(CPU*c){ stub_note("MSVCP90.dll!?find@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEIPBDII@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90__npos___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__2IB(CPU*c) __attribute__((weak));
static void stub_364(CPU*c){ stub_note("MSVCP90.dll!?npos@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@2IB"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90__substr___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QBE_AV12_II_Z(CPU*c) __attribute__((weak));
static void stub_365(CPU*c){ stub_note("MSVCP90.dll!?substr@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBE?AV12@II@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90___4__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV01_ABV01__Z(CPU*c) __attribute__((weak));
static void stub_366(CPU*c){ stub_note("MSVCP90.dll!??4?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@ABV01@@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90__erase___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV12_II_Z(CPU*c) __attribute__((weak));
static void stub_367(CPU*c){ stub_note("MSVCP90.dll!?erase@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV12@II@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_PBD_Z(CPU*c) __attribute__((weak));
static void stub_368(CPU*c){ stub_note("MSVCP90.dll!??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcp90___Y__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV01_PBD_Z(CPU*c) __attribute__((weak));
static void stub_369(CPU*c){ stub_note("MSVCP90.dll!??Y?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@PBD@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__set_SSE2_enable(CPU*c) __attribute__((weak));
static void stub_370(CPU*c){ stub_note("MSVCR90.dll!_set_SSE2_enable"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__except_handler3(CPU*c) __attribute__((weak));
static void stub_371(CPU*c){ stub_note("MSVCR90.dll!_except_handler3"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIexp(CPU*c) __attribute__((weak));
static void stub_372(CPU*c){ stub_note("MSVCR90.dll!_CIexp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wstat64(CPU*c) __attribute__((weak));
static void stub_373(CPU*c){ stub_note("MSVCR90.dll!_wstat64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wfopen_s(CPU*c) __attribute__((weak));
static void stub_374(CPU*c){ stub_note("MSVCR90.dll!_wfopen_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_rewind(CPU*c) __attribute__((weak));
static void stub_375(CPU*c){ stub_note("MSVCR90.dll!rewind"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__itoa_s(CPU*c) __attribute__((weak));
static void stub_376(CPU*c){ stub_note("MSVCR90.dll!_itoa_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__controlfp_s(CPU*c) __attribute__((weak));
static void stub_377(CPU*c){ stub_note("MSVCR90.dll!_controlfp_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_localeconv(CPU*c) __attribute__((weak));
static void stub_378(CPU*c){ stub_note("MSVCR90.dll!localeconv"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strcspn(CPU*c) __attribute__((weak));
static void stub_379(CPU*c){ stub_note("MSVCR90.dll!strcspn"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strspn(CPU*c) __attribute__((weak));
static void stub_380(CPU*c){ stub_note("MSVCR90.dll!strspn"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcscoll(CPU*c) __attribute__((weak));
static void stub_381(CPU*c){ stub_note("MSVCR90.dll!wcscoll"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wcsicoll(CPU*c) __attribute__((weak));
static void stub_382(CPU*c){ stub_note("MSVCR90.dll!_wcsicoll"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CItan(CPU*c) __attribute__((weak));
static void stub_383(CPU*c){ stub_note("MSVCR90.dll!_CItan"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIatan2(CPU*c) __attribute__((weak));
static void stub_384(CPU*c){ stub_note("MSVCR90.dll!_CIatan2"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__strtoui64(CPU*c) __attribute__((weak));
static void stub_385(CPU*c){ stub_note("MSVCR90.dll!_strtoui64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strtod(CPU*c) __attribute__((weak));
static void stub_386(CPU*c){ stub_note("MSVCR90.dll!strtod"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__gmtime64(CPU*c) __attribute__((weak));
static void stub_387(CPU*c){ stub_note("MSVCR90.dll!_gmtime64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__stat64(CPU*c) __attribute__((weak));
static void stub_388(CPU*c){ stub_note("MSVCR90.dll!_stat64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_getenv(CPU*c) __attribute__((weak));
static void stub_389(CPU*c){ stub_note("MSVCR90.dll!getenv"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_sprintf(CPU*c) __attribute__((weak));
static void stub_390(CPU*c){ stub_note("MSVCR90.dll!sprintf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fputs(CPU*c) __attribute__((weak));
static void stub_391(CPU*c){ stub_note("MSVCR90.dll!fputs"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_memchr(CPU*c) __attribute__((weak));
static void stub_392(CPU*c){ stub_note("MSVCR90.dll!memchr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__strtoi64(CPU*c) __attribute__((weak));
static void stub_393(CPU*c){ stub_note("MSVCR90.dll!_strtoi64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_isdigit(CPU*c) __attribute__((weak));
static void stub_394(CPU*c){ stub_note("MSVCR90.dll!isdigit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strpbrk(CPU*c) __attribute__((weak));
static void stub_395(CPU*c){ stub_note("MSVCR90.dll!strpbrk"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strtoul(CPU*c) __attribute__((weak));
static void stub_396(CPU*c){ stub_note("MSVCR90.dll!strtoul"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___sys_nerr(CPU*c) __attribute__((weak));
static void stub_397(CPU*c){ stub_note("MSVCR90.dll!__sys_nerr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strerror(CPU*c) __attribute__((weak));
static void stub_398(CPU*c){ stub_note("MSVCR90.dll!strerror"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__unlock(CPU*c) __attribute__((weak));
static void stub_399(CPU*c){ stub_note("MSVCR90.dll!_unlock"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___dllonexit(CPU*c) __attribute__((weak));
static void stub_400(CPU*c){ stub_note("MSVCR90.dll!__dllonexit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__encode_pointer(CPU*c) __attribute__((weak));
static void stub_401(CPU*c){ stub_note("MSVCR90.dll!_encode_pointer"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__lock(CPU*c) __attribute__((weak));
static void stub_402(CPU*c){ stub_note("MSVCR90.dll!_lock"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__onexit(CPU*c) __attribute__((weak));
static void stub_403(CPU*c){ stub_note("MSVCR90.dll!_onexit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__decode_pointer(CPU*c) __attribute__((weak));
static void stub_404(CPU*c){ stub_note("MSVCR90.dll!_decode_pointer"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__except_handler4_common(CPU*c) __attribute__((weak));
static void stub_405(CPU*c){ stub_note("MSVCR90.dll!_except_handler4_common"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__terminate__YAXXZ(CPU*c) __attribute__((weak));
static void stub_406(CPU*c){ stub_note("MSVCR90.dll!?terminate@@YAXXZ"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__vsnprintf(CPU*c) __attribute__((weak));
static void stub_407(CPU*c){ stub_note("MSVCR90.dll!_vsnprintf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__amsg_exit(CPU*c) __attribute__((weak));
static void stub_408(CPU*c){ stub_note("MSVCR90.dll!_amsg_exit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___getmainargs(CPU*c) __attribute__((weak));
static void stub_409(CPU*c){ stub_note("MSVCR90.dll!__getmainargs"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__cexit(CPU*c) __attribute__((weak));
static void stub_410(CPU*c){ stub_note("MSVCR90.dll!_cexit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__exit(CPU*c) __attribute__((weak));
static void stub_411(CPU*c){ stub_note("MSVCR90.dll!_exit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__XcptFilter(CPU*c) __attribute__((weak));
static void stub_412(CPU*c){ stub_note("MSVCR90.dll!_XcptFilter"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__acmdln(CPU*c) __attribute__((weak));
static void stub_413(CPU*c){ stub_note("MSVCR90.dll!_acmdln"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__initterm(CPU*c) __attribute__((weak));
static void stub_414(CPU*c){ stub_note("MSVCR90.dll!_initterm"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__initterm_e(CPU*c) __attribute__((weak));
static void stub_415(CPU*c){ stub_note("MSVCR90.dll!_initterm_e"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__configthreadlocale(CPU*c) __attribute__((weak));
static void stub_416(CPU*c){ stub_note("MSVCR90.dll!_configthreadlocale"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___setusermatherr(CPU*c) __attribute__((weak));
static void stub_417(CPU*c){ stub_note("MSVCR90.dll!__setusermatherr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__adjust_fdiv(CPU*c) __attribute__((weak));
static void stub_418(CPU*c){ stub_note("MSVCR90.dll!_adjust_fdiv"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strrchr(CPU*c) __attribute__((weak));
static void stub_419(CPU*c){ stub_note("MSVCR90.dll!strrchr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_calloc(CPU*c) __attribute__((weak));
static void stub_420(CPU*c){ stub_note("MSVCR90.dll!calloc"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_isalnum(CPU*c) __attribute__((weak));
static void stub_421(CPU*c){ stub_note("MSVCR90.dll!isalnum"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_isalpha(CPU*c) __attribute__((weak));
static void stub_422(CPU*c){ stub_note("MSVCR90.dll!isalpha"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_atof(CPU*c) __attribute__((weak));
static void stub_423(CPU*c){ stub_note("MSVCR90.dll!atof"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_sscanf(CPU*c) __attribute__((weak));
static void stub_424(CPU*c){ stub_note("MSVCR90.dll!sscanf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fprintf(CPU*c) __attribute__((weak));
static void stub_425(CPU*c){ stub_note("MSVCR90.dll!fprintf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fputc(CPU*c) __attribute__((weak));
static void stub_426(CPU*c){ stub_note("MSVCR90.dll!fputc"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_ferror(CPU*c) __attribute__((weak));
static void stub_427(CPU*c){ stub_note("MSVCR90.dll!ferror"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__fpclass(CPU*c) __attribute__((weak));
static void stub_428(CPU*c){ stub_note("MSVCR90.dll!_fpclass"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_sprintf_s(CPU*c) __attribute__((weak));
static void stub_429(CPU*c){ stub_note("MSVCR90.dll!sprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strchr(CPU*c) __attribute__((weak));
static void stub_430(CPU*c){ stub_note("MSVCR90.dll!strchr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__aligned_free(CPU*c) __attribute__((weak));
static void stub_431(CPU*c){ stub_note("MSVCR90.dll!_aligned_free"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__aligned_malloc(CPU*c) __attribute__((weak));
static void stub_432(CPU*c){ stub_note("MSVCR90.dll!_aligned_malloc"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_printf(CPU*c) __attribute__((weak));
static void stub_433(CPU*c){ stub_note("MSVCR90.dll!printf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fgets(CPU*c) __attribute__((weak));
static void stub_434(CPU*c){ stub_note("MSVCR90.dll!fgets"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_vsprintf(CPU*c) __attribute__((weak));
static void stub_435(CPU*c){ stub_note("MSVCR90.dll!vsprintf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_isxdigit(CPU*c) __attribute__((weak));
static void stub_436(CPU*c){ stub_note("MSVCR90.dll!isxdigit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_tolower(CPU*c) __attribute__((weak));
static void stub_437(CPU*c){ stub_note("MSVCR90.dll!tolower"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIatan(CPU*c) __attribute__((weak));
static void stub_438(CPU*c){ stub_note("MSVCR90.dll!_CIatan"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___p__commode(CPU*c) __attribute__((weak));
static void stub_439(CPU*c){ stub_note("MSVCR90.dll!__p__commode"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___p__fmode(CPU*c) __attribute__((weak));
static void stub_440(CPU*c){ stub_note("MSVCR90.dll!__p__fmode"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIlog(CPU*c) __attribute__((weak));
static void stub_441(CPU*c){ stub_note("MSVCR90.dll!_CIlog"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__beginthreadex(CPU*c) __attribute__((weak));
static void stub_442(CPU*c){ stub_note("MSVCR90.dll!_beginthreadex"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__endthreadex(CPU*c) __attribute__((weak));
static void stub_443(CPU*c){ stub_note("MSVCR90.dll!_endthreadex"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIsin(CPU*c) __attribute__((weak));
static void stub_444(CPU*c){ stub_note("MSVCR90.dll!_CIsin"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIcos(CPU*c) __attribute__((weak));
static void stub_445(CPU*c){ stub_note("MSVCR90.dll!_CIcos"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIsqrt(CPU*c) __attribute__((weak));
static void stub_446(CPU*c){ stub_note("MSVCR90.dll!_CIsqrt"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__vswprintf_c_l(CPU*c) __attribute__((weak));
static void stub_447(CPU*c){ stub_note("MSVCR90.dll!_vswprintf_c_l"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcsncmp(CPU*c) __attribute__((weak));
static void stub_448(CPU*c){ stub_note("MSVCR90.dll!wcsncmp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__set_purecall_handler(CPU*c) __attribute__((weak));
static void stub_449(CPU*c){ stub_note("MSVCR90.dll!_set_purecall_handler"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__set_invalid_parameter_handler(CPU*c) __attribute__((weak));
static void stub_450(CPU*c){ stub_note("MSVCR90.dll!_set_invalid_parameter_handler"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___iob_func(CPU*c) __attribute__((weak));
static void stub_451(CPU*c){ stub_note("MSVCR90.dll!__iob_func"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fflush(CPU*c) __attribute__((weak));
static void stub_452(CPU*c){ stub_note("MSVCR90.dll!fflush"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fopen(CPU*c) __attribute__((weak));
static void stub_453(CPU*c){ stub_note("MSVCR90.dll!fopen"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fwrite(CPU*c) __attribute__((weak));
static void stub_454(CPU*c){ stub_note("MSVCR90.dll!fwrite"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__stat64i32(CPU*c) __attribute__((weak));
static void stub_455(CPU*c){ stub_note("MSVCR90.dll!_stat64i32"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__errno(CPU*c) __attribute__((weak));
static void stub_456(CPU*c){ stub_note("MSVCR90.dll!_errno"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strftime(CPU*c) __attribute__((weak));
static void stub_457(CPU*c){ stub_note("MSVCR90.dll!strftime"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strncpy(CPU*c) __attribute__((weak));
static void stub_458(CPU*c){ stub_note("MSVCR90.dll!strncpy"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__localtime64(CPU*c) __attribute__((weak));
static void stub_459(CPU*c){ stub_note("MSVCR90.dll!_localtime64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fclose(CPU*c) __attribute__((weak));
static void stub_460(CPU*c){ stub_note("MSVCR90.dll!fclose"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_ftell(CPU*c) __attribute__((weak));
static void stub_461(CPU*c){ stub_note("MSVCR90.dll!ftell"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strtol(CPU*c) __attribute__((weak));
static void stub_462(CPU*c){ stub_note("MSVCR90.dll!strtol"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fopen_s(CPU*c) __attribute__((weak));
static void stub_463(CPU*c){ stub_note("MSVCR90.dll!fopen_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_isspace(CPU*c) __attribute__((weak));
static void stub_464(CPU*c){ stub_note("MSVCR90.dll!isspace"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__getcwd(CPU*c) __attribute__((weak));
static void stub_465(CPU*c){ stub_note("MSVCR90.dll!_getcwd"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_realloc(CPU*c) __attribute__((weak));
static void stub_466(CPU*c){ stub_note("MSVCR90.dll!realloc"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_ldiv(CPU*c) __attribute__((weak));
static void stub_467(CPU*c){ stub_note("MSVCR90.dll!ldiv"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_feof(CPU*c) __attribute__((weak));
static void stub_468(CPU*c){ stub_note("MSVCR90.dll!feof"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fseek(CPU*c) __attribute__((weak));
static void stub_469(CPU*c){ stub_note("MSVCR90.dll!fseek"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_fread(CPU*c) __attribute__((weak));
static void stub_470(CPU*c){ stub_note("MSVCR90.dll!fread"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_vswprintf_s(CPU*c) __attribute__((weak));
static void stub_471(CPU*c){ stub_note("MSVCR90.dll!vswprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__snprintf(CPU*c) __attribute__((weak));
static void stub_472(CPU*c){ stub_note("MSVCR90.dll!_snprintf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wtoi64(CPU*c) __attribute__((weak));
static void stub_473(CPU*c){ stub_note("MSVCR90.dll!_wtoi64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strncmp(CPU*c) __attribute__((weak));
static void stub_474(CPU*c){ stub_note("MSVCR90.dll!strncmp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_ceil(CPU*c) __attribute__((weak));
static void stub_475(CPU*c){ stub_note("MSVCR90.dll!ceil"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strstr(CPU*c) __attribute__((weak));
static void stub_476(CPU*c){ stub_note("MSVCR90.dll!strstr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_memmove_s(CPU*c) __attribute__((weak));
static void stub_477(CPU*c){ stub_note("MSVCR90.dll!memmove_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___0exception_std__QAE_ABV01__Z(CPU*c) __attribute__((weak));
static void stub_478(CPU*c){ stub_note("MSVCR90.dll!??0exception@std@@QAE@ABV01@@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__invalid_parameter_noinfo(CPU*c) __attribute__((weak));
static void stub_479(CPU*c){ stub_note("MSVCR90.dll!_invalid_parameter_noinfo"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___0exception_std__QAE_XZ(CPU*c) __attribute__((weak));
static void stub_480(CPU*c){ stub_note("MSVCR90.dll!??0exception@std@@QAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_vsprintf_s(CPU*c) __attribute__((weak));
static void stub_481(CPU*c){ stub_note("MSVCR90.dll!vsprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___1exception_std__UAE_XZ(CPU*c) __attribute__((weak));
static void stub_482(CPU*c){ stub_note("MSVCR90.dll!??1exception@std@@UAE@XZ"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__what_exception_std__UBEPBDXZ(CPU*c) __attribute__((weak));
static void stub_483(CPU*c){ stub_note("MSVCR90.dll!?what@exception@std@@UBEPBDXZ"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___0exception_std__QAE_ABQBD_Z(CPU*c) __attribute__((weak));
static void stub_484(CPU*c){ stub_note("MSVCR90.dll!??0exception@std@@QAE@ABQBD@Z"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_exp(CPU*c) __attribute__((weak));
static void stub_485(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_exp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_log(CPU*c) __attribute__((weak));
static void stub_486(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_log"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__finite(CPU*c) __attribute__((weak));
static void stub_487(CPU*c){ stub_note("MSVCR90.dll!_finite"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__isnan(CPU*c) __attribute__((weak));
static void stub_488(CPU*c){ stub_note("MSVCR90.dll!_isnan"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_atan(CPU*c) __attribute__((weak));
static void stub_489(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_atan"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_tan(CPU*c) __attribute__((weak));
static void stub_490(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_tan"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_iswpunct(CPU*c) __attribute__((weak));
static void stub_491(CPU*c){ stub_note("MSVCR90.dll!iswpunct"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_srand(CPU*c) __attribute__((weak));
static void stub_492(CPU*c){ stub_note("MSVCR90.dll!srand"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_swscanf_s(CPU*c) __attribute__((weak));
static void stub_493(CPU*c){ stub_note("MSVCR90.dll!swscanf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_exit(CPU*c) __attribute__((weak));
static void stub_494(CPU*c){ stub_note("MSVCR90.dll!exit"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_free(CPU*c) __attribute__((weak));
static void stub_495(CPU*c){ stub_note("MSVCR90.dll!free"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_malloc(CPU*c) __attribute__((weak));
static void stub_496(CPU*c){ stub_note("MSVCR90.dll!malloc"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___set_app_type(CPU*c) __attribute__((weak));
static void stub_497(CPU*c){ stub_note("MSVCR90.dll!__set_app_type"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__crt_debugger_hook(CPU*c) __attribute__((weak));
static void stub_498(CPU*c){ stub_note("MSVCR90.dll!_crt_debugger_hook"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__vscwprintf_p(CPU*c) __attribute__((weak));
static void stub_499(CPU*c){ stub_note("MSVCR90.dll!_vscwprintf_p"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__vsnprintf_s(CPU*c) __attribute__((weak));
static void stub_500(CPU*c){ stub_note("MSVCR90.dll!_vsnprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__vsnwprintf_s(CPU*c) __attribute__((weak));
static void stub_501(CPU*c){ stub_note("MSVCR90.dll!_vsnwprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__set_abort_behavior(CPU*c) __attribute__((weak));
static void stub_502(CPU*c){ stub_note("MSVCR90.dll!_set_abort_behavior"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_abort(CPU*c) __attribute__((weak));
static void stub_503(CPU*c){ stub_note("MSVCR90.dll!abort"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wprintf(CPU*c) __attribute__((weak));
static void stub_504(CPU*c){ stub_note("MSVCR90.dll!wprintf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_toupper(CPU*c) __attribute__((weak));
static void stub_505(CPU*c){ stub_note("MSVCR90.dll!toupper"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_acos(CPU*c) __attribute__((weak));
static void stub_506(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_acos"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_asin(CPU*c) __attribute__((weak));
static void stub_507(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_asin"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_atan2(CPU*c) __attribute__((weak));
static void stub_508(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_atan2"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_qsort(CPU*c) __attribute__((weak));
static void stub_509(CPU*c){ stub_note("MSVCR90.dll!qsort"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strncpy_s(CPU*c) __attribute__((weak));
static void stub_510(CPU*c){ stub_note("MSVCR90.dll!strncpy_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcsncpy_s(CPU*c) __attribute__((weak));
static void stub_511(CPU*c){ stub_note("MSVCR90.dll!wcsncpy_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wstrtime_s(CPU*c) __attribute__((weak));
static void stub_512(CPU*c){ stub_note("MSVCR90.dll!_wstrtime_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wstrdate_s(CPU*c) __attribute__((weak));
static void stub_513(CPU*c){ stub_note("MSVCR90.dll!_wstrdate_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wstat64i32(CPU*c) __attribute__((weak));
static void stub_514(CPU*c){ stub_note("MSVCR90.dll!_wstat64i32"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___type_info_dtor_internal_method_type_info__QAEXXZ(CPU*c) __attribute__((weak));
static void stub_515(CPU*c){ stub_note("MSVCR90.dll!?_type_info_dtor_internal_method@type_info@@QAEXXZ"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__invoke_watson(CPU*c) __attribute__((weak));
static void stub_516(CPU*c){ stub_note("MSVCR90.dll!_invoke_watson"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__setjmp3(CPU*c) __attribute__((weak));
static void stub_517(CPU*c){ stub_note("MSVCR90.dll!_setjmp3"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_longjmp(CPU*c) __attribute__((weak));
static void stub_518(CPU*c){ stub_note("MSVCR90.dll!longjmp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__iob(CPU*c) __attribute__((weak));
static void stub_519(CPU*c){ stub_note("MSVCR90.dll!_iob"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strlen(CPU*c) __attribute__((weak));
static void stub_520(CPU*c){ stub_note("MSVCR90.dll!strlen"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wctob(CPU*c) __attribute__((weak));
static void stub_521(CPU*c){ stub_note("MSVCR90.dll!wctob"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_abs(CPU*c) __attribute__((weak));
static void stub_522(CPU*c){ stub_note("MSVCR90.dll!abs"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__strdup(CPU*c) __attribute__((weak));
static void stub_523(CPU*c){ stub_note("MSVCR90.dll!_strdup"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_vprintf(CPU*c) __attribute__((weak));
static void stub_524(CPU*c){ stub_note("MSVCR90.dll!vprintf"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__snprintf_s(CPU*c) __attribute__((weak));
static void stub_525(CPU*c){ stub_note("MSVCR90.dll!_snprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wprintf_s(CPU*c) __attribute__((weak));
static void stub_526(CPU*c){ stub_note("MSVCR90.dll!wprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__snwprintf_s(CPU*c) __attribute__((weak));
static void stub_527(CPU*c){ stub_note("MSVCR90.dll!_snwprintf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_printf_s(CPU*c) __attribute__((weak));
static void stub_528(CPU*c){ stub_note("MSVCR90.dll!printf_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__ismbblead(CPU*c) __attribute__((weak));
static void stub_529(CPU*c){ stub_note("MSVCR90.dll!_ismbblead"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wcsicmp(CPU*c) __attribute__((weak));
static void stub_530(CPU*c){ stub_note("MSVCR90.dll!_wcsicmp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__purecall(CPU*c) __attribute__((weak));
static void stub_531(CPU*c){ stub_note("MSVCR90.dll!_purecall"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___CxxFrameHandler3(CPU*c) __attribute__((weak));
static void stub_532(CPU*c){ stub_note("MSVCR90.dll!__CxxFrameHandler3"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_cos(CPU*c) __attribute__((weak));
static void stub_533(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_cos"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_memcpy(CPU*c) __attribute__((weak));
static void stub_534(CPU*c){ stub_note("MSVCR90.dll!memcpy"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_memset(CPU*c) __attribute__((weak));
static void stub_535(CPU*c){ stub_note("MSVCR90.dll!memset"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIacos(CPU*c) __attribute__((weak));
static void stub_536(CPU*c){ stub_note("MSVCR90.dll!_CIacos"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIasin(CPU*c) __attribute__((weak));
static void stub_537(CPU*c){ stub_note("MSVCR90.dll!_CIasin"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_floor(CPU*c) __attribute__((weak));
static void stub_538(CPU*c){ stub_note("MSVCR90.dll!floor"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIfmod(CPU*c) __attribute__((weak));
static void stub_539(CPU*c){ stub_note("MSVCR90.dll!_CIfmod"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CIpow(CPU*c) __attribute__((weak));
static void stub_540(CPU*c){ stub_note("MSVCR90.dll!_CIpow"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_rand(CPU*c) __attribute__((weak));
static void stub_541(CPU*c){ stub_note("MSVCR90.dll!rand"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_memmove(CPU*c) __attribute__((weak));
static void stub_542(CPU*c){ stub_note("MSVCR90.dll!memmove"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strcpy_s(CPU*c) __attribute__((weak));
static void stub_543(CPU*c){ stub_note("MSVCR90.dll!strcpy_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcscpy_s(CPU*c) __attribute__((weak));
static void stub_544(CPU*c){ stub_note("MSVCR90.dll!wcscpy_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcscat_s(CPU*c) __attribute__((weak));
static void stub_545(CPU*c){ stub_note("MSVCR90.dll!wcscat_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_strcat_s(CPU*c) __attribute__((weak));
static void stub_546(CPU*c){ stub_note("MSVCR90.dll!strcat_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcsstr(CPU*c) __attribute__((weak));
static void stub_547(CPU*c){ stub_note("MSVCR90.dll!wcsstr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcschr(CPU*c) __attribute__((weak));
static void stub_548(CPU*c){ stub_note("MSVCR90.dll!wcschr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcsrchr(CPU*c) __attribute__((weak));
static void stub_549(CPU*c){ stub_note("MSVCR90.dll!wcsrchr"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__stricmp(CPU*c) __attribute__((weak));
static void stub_550(CPU*c){ stub_note("MSVCR90.dll!_stricmp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wtoi(CPU*c) __attribute__((weak));
static void stub_551(CPU*c){ stub_note("MSVCR90.dll!_wtoi"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_atoi(CPU*c) __attribute__((weak));
static void stub_552(CPU*c){ stub_note("MSVCR90.dll!atoi"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wtof(CPU*c) __attribute__((weak));
static void stub_553(CPU*c){ stub_note("MSVCR90.dll!_wtof"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcstod(CPU*c) __attribute__((weak));
static void stub_554(CPU*c){ stub_note("MSVCR90.dll!wcstod"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90_wcstoul(CPU*c) __attribute__((weak));
static void stub_555(CPU*c){ stub_note("MSVCR90.dll!wcstoul"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wcstoui64(CPU*c) __attribute__((weak));
static void stub_556(CPU*c){ stub_note("MSVCR90.dll!_wcstoui64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wcsnicmp(CPU*c) __attribute__((weak));
static void stub_557(CPU*c){ stub_note("MSVCR90.dll!_wcsnicmp"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_sin(CPU*c) __attribute__((weak));
static void stub_558(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_sin"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90___libm_sse2_pow(CPU*c) __attribute__((weak));
static void stub_559(CPU*c){ stub_note("MSVCR90.dll!__libm_sse2_pow"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__CxxThrowException(CPU*c) __attribute__((weak));
static void stub_560(CPU*c){ stub_note("MSVCR90.dll!_CxxThrowException"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__wutime64(CPU*c) __attribute__((weak));
static void stub_561(CPU*c){ stub_note("MSVCR90.dll!_wutime64"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__gmtime64_s(CPU*c) __attribute__((weak));
static void stub_562(CPU*c){ stub_note("MSVCR90.dll!_gmtime64_s"); c->eax=0; c->esp+=4+0; }
void hle_msvcr90__time64(CPU*c) __attribute__((weak));
static void stub_563(CPU*c){ stub_note("MSVCR90.dll!_time64"); c->eax=0; c->esp+=4+0; }
void hle_apexframeworkshipping_x86_NxCreateApexSDK(CPU*c) __attribute__((weak));
static void stub_564(CPU*c){ stub_note("ApexFrameworkSHIPPING_x86.dll!NxCreateApexSDK"); c->eax=0; c->esp+=4+0; }
void hle_powrprof_CallNtPowerInformation(CPU*c) __attribute__((weak));
static void stub_565(CPU*c){ stub_note("POWRPROF.dll!CallNtPowerInformation"); c->eax=0; c->esp+=4+20; }
void hle_psapi_GetProcessMemoryInfo(CPU*c) __attribute__((weak));
static void stub_566(CPU*c){ stub_note("PSAPI.DLL!GetProcessMemoryInfo"); c->eax=0; c->esp+=4+12; }
void hle_faultrep_ReportFault(CPU*c) __attribute__((weak));
static void stub_567(CPU*c){ stub_note("faultrep.dll!ReportFault"); c->eax=0; c->esp+=4+8; }
void hle_ws2_32_WSAIoctl(CPU*c) __attribute__((weak));
static void stub_568(CPU*c){ stub_note("WS2_32.dll!WSAIoctl"); c->eax=0; c->esp+=4+36; }
void hle_ws2_32_freeaddrinfo(CPU*c) __attribute__((weak));
static void stub_569(CPU*c){ stub_note("WS2_32.dll!freeaddrinfo"); c->eax=0; c->esp+=4+4; }
void hle_ws2_32_getaddrinfo(CPU*c) __attribute__((weak));
static void stub_570(CPU*c){ stub_note("WS2_32.dll!getaddrinfo"); c->eax=0; c->esp+=4+16; }
Imp g_imps[]={
{0x10d403cu,"COMCTL32.dll!ord345",hle_comctl32_ord345},
{0x10d479cu,"WSOCK32.dll!bind",hle_wsock32_bind},
{0x10d47a0u,"WSOCK32.dll!WSAGetLastError",hle_wsock32_WSAGetLastError},
{0x10d47a4u,"WSOCK32.dll!connect",hle_wsock32_connect},
{0x10d47a8u,"WSOCK32.dll!listen",hle_wsock32_listen},
{0x10d47acu,"WSOCK32.dll!select",hle_wsock32_select},
{0x10d47b0u,"WSOCK32.dll!ioctlsocket",hle_wsock32_ioctlsocket},
{0x10d47b4u,"WSOCK32.dll!sendto",hle_wsock32_sendto},
{0x10d47b8u,"WSOCK32.dll!send",hle_wsock32_send},
{0x10d47bcu,"WSOCK32.dll!recvfrom",hle_wsock32_recvfrom},
{0x10d47c0u,"WSOCK32.dll!recv",hle_wsock32_recv},
{0x10d47c4u,"WSOCK32.dll!getsockname",hle_wsock32_getsockname},
{0x10d47c8u,"WSOCK32.dll!closesocket",hle_wsock32_closesocket},
{0x10d47ccu,"WSOCK32.dll!WSAStartup",hle_wsock32_WSAStartup},
{0x10d47d0u,"WSOCK32.dll!shutdown",hle_wsock32_shutdown},
{0x10d47d4u,"WSOCK32.dll!gethostname",hle_wsock32_gethostname},
{0x10d47d8u,"WSOCK32.dll!socket",hle_wsock32_socket},
{0x10d47dcu,"WSOCK32.dll!ntohs",hle_wsock32_ntohs},
{0x10d47e0u,"WSOCK32.dll!accept",hle_wsock32_accept},
{0x10d47e4u,"WSOCK32.dll!htons",hle_wsock32_htons},
{0x10d47e8u,"WSOCK32.dll!__WSAFDIsSet",hle_wsock32___WSAFDIsSet},
{0x10d47ecu,"WSOCK32.dll!WSASetLastError",hle_wsock32_WSASetLastError},
{0x10d47f0u,"WSOCK32.dll!getpeername",hle_wsock32_getpeername},
{0x10d47f4u,"WSOCK32.dll!gethostbyname",hle_wsock32_gethostbyname},
{0x10d47f8u,"WSOCK32.dll!WSACleanup",hle_wsock32_WSACleanup},
{0x10d47fcu,"WSOCK32.dll!getsockopt",hle_wsock32_getsockopt},
{0x10d4800u,"WSOCK32.dll!ntohl",hle_wsock32_ntohl},
{0x10d4804u,"WSOCK32.dll!htonl",hle_wsock32_htonl},
{0x10d4808u,"WSOCK32.dll!setsockopt",hle_wsock32_setsockopt},
{0x10d480cu,"WSOCK32.dll!inet_addr",hle_wsock32_inet_addr},
{0x10d4074u,"KERNEL32.dll!SetCriticalSectionSpinCount",hle_kernel32_SetCriticalSectionSpinCount},
{0x10d4078u,"KERNEL32.dll!InitializeCriticalSection",hle_kernel32_InitializeCriticalSection},
{0x10d407cu,"KERNEL32.dll!EnterCriticalSection",hle_kernel32_EnterCriticalSection},
{0x10d4080u,"KERNEL32.dll!TryEnterCriticalSection",hle_kernel32_TryEnterCriticalSection},
{0x10d4084u,"KERNEL32.dll!LeaveCriticalSection",hle_kernel32_LeaveCriticalSection},
{0x10d4088u,"KERNEL32.dll!QueryPerformanceCounter",hle_kernel32_QueryPerformanceCounter},
{0x10d408cu,"KERNEL32.dll!InterlockedIncrement",hle_kernel32_InterlockedIncrement},
{0x10d4090u,"KERNEL32.dll!InterlockedDecrement",hle_kernel32_InterlockedDecrement},
{0x10d4094u,"KERNEL32.dll!InterlockedExchangeAdd",hle_kernel32_InterlockedExchangeAdd},
{0x10d4098u,"KERNEL32.dll!InterlockedExchange",hle_kernel32_InterlockedExchange},
{0x10d409cu,"KERNEL32.dll!InterlockedCompareExchange",hle_kernel32_InterlockedCompareExchange},
{0x10d40a0u,"KERNEL32.dll!GetCurrentThread",hle_kernel32_GetCurrentThread},
{0x10d40a4u,"KERNEL32.dll!GetCurrentThreadId",hle_kernel32_GetCurrentThreadId},
{0x10d40a8u,"KERNEL32.dll!SetThreadIdealProcessor",hle_kernel32_SetThreadIdealProcessor},
{0x10d40acu,"KERNEL32.dll!TlsAlloc",hle_kernel32_TlsAlloc},
{0x10d40b0u,"KERNEL32.dll!TlsSetValue",hle_kernel32_TlsSetValue},
{0x10d40b4u,"KERNEL32.dll!TlsGetValue",hle_kernel32_TlsGetValue},
{0x10d40b8u,"KERNEL32.dll!TlsFree",hle_kernel32_TlsFree},
{0x10d40bcu,"KERNEL32.dll!CreateSemaphoreW",hle_kernel32_CreateSemaphoreW},
{0x10d40c0u,"KERNEL32.dll!CloseHandle",hle_kernel32_CloseHandle},
{0x10d40c4u,"KERNEL32.dll!WaitForSingleObject",hle_kernel32_WaitForSingleObject},
{0x10d40c8u,"KERNEL32.dll!ReleaseSemaphore",hle_kernel32_ReleaseSemaphore},
{0x10d40ccu,"KERNEL32.dll!SetCurrentDirectoryW",hle_kernel32_SetCurrentDirectoryW},
{0x10d40d0u,"KERNEL32.dll!GetFileSize",hle_kernel32_GetFileSize},
{0x10d40d4u,"KERNEL32.dll!CreateFileW",hle_kernel32_CreateFileW},
{0x10d40d8u,"KERNEL32.dll!GetLastError",hle_kernel32_GetLastError},
{0x10d40dcu,"KERNEL32.dll!GetFileAttributesW",hle_kernel32_GetFileAttributesW},
{0x10d40e0u,"KERNEL32.dll!CreateDirectoryW",hle_kernel32_CreateDirectoryW},
{0x10d40e4u,"KERNEL32.dll!GetFileTime",hle_kernel32_GetFileTime},
{0x10d40e8u,"KERNEL32.dll!SystemTimeToFileTime",hle_kernel32_SystemTimeToFileTime},
{0x10d40ecu,"KERNEL32.dll!GetSystemTime",hle_kernel32_GetSystemTime},
{0x10d40f0u,"KERNEL32.dll!SetFilePointer",hle_kernel32_SetFilePointer},
{0x10d40f4u,"KERNEL32.dll!GetCurrentProcess",hle_kernel32_GetCurrentProcess},
{0x10d40f8u,"KERNEL32.dll!VerifyVersionInfoW",hle_kernel32_VerifyVersionInfoW},
{0x10d40fcu,"KERNEL32.dll!VerSetConditionMask",hle_kernel32_VerSetConditionMask},
{0x10d4100u,"KERNEL32.dll!RtlCaptureStackBackTrace",hle_kernel32_RtlCaptureStackBackTrace},
{0x10d4104u,"KERNEL32.dll!RtlCaptureContext",hle_kernel32_RtlCaptureContext},
{0x10d4108u,"KERNEL32.dll!ReadFile",hle_kernel32_ReadFile},
{0x10d410cu,"KERNEL32.dll!RaiseException",hle_kernel32_RaiseException},
{0x10d4110u,"KERNEL32.dll!GetProcAddress",hle_kernel32_GetProcAddress},
{0x10d4114u,"KERNEL32.dll!GetModuleHandleW",hle_kernel32_GetModuleHandleW},
{0x10d4118u,"KERNEL32.dll!FlushFileBuffers",hle_kernel32_FlushFileBuffers},
{0x10d411cu,"KERNEL32.dll!ExitProcess",hle_kernel32_ExitProcess},
{0x10d4120u,"KERNEL32.dll!SetConsoleCtrlHandler",hle_kernel32_SetConsoleCtrlHandler},
{0x10d4124u,"KERNEL32.dll!Sleep",hle_kernel32_Sleep},
{0x10d4128u,"KERNEL32.dll!CreateEventW",hle_kernel32_CreateEventW},
{0x10d412cu,"KERNEL32.dll!SetEvent",hle_kernel32_SetEvent},
{0x10d4130u,"KERNEL32.dll!ResetEvent",hle_kernel32_ResetEvent},
{0x10d4134u,"KERNEL32.dll!ResumeThread",hle_kernel32_ResumeThread},
{0x10d4138u,"KERNEL32.dll!SuspendThread",hle_kernel32_SuspendThread},
{0x10d413cu,"KERNEL32.dll!SetThreadPriority",hle_kernel32_SetThreadPriority},
{0x10d4140u,"KERNEL32.dll!OutputDebugStringW",hle_kernel32_OutputDebugStringW},
{0x10d4144u,"KERNEL32.dll!FormatMessageW",hle_kernel32_FormatMessageW},
{0x10d4148u,"KERNEL32.dll!LoadLibraryW",hle_kernel32_LoadLibraryW},
{0x10d414cu,"KERNEL32.dll!FreeLibrary",hle_kernel32_FreeLibrary},
{0x10d4150u,"KERNEL32.dll!GetVersionExW",hle_kernel32_GetVersionExW},
{0x10d4154u,"KERNEL32.dll!GetLocalTime",hle_kernel32_GetLocalTime},
{0x10d4158u,"KERNEL32.dll!FileTimeToSystemTime",hle_kernel32_FileTimeToSystemTime},
{0x10d415cu,"KERNEL32.dll!FileTimeToLocalFileTime",hle_kernel32_FileTimeToLocalFileTime},
{0x10d4160u,"KERNEL32.dll!GetSystemTimeAsFileTime",hle_kernel32_GetSystemTimeAsFileTime},
{0x10d4164u,"KERNEL32.dll!LocalFileTimeToFileTime",hle_kernel32_LocalFileTimeToFileTime},
{0x10d4168u,"KERNEL32.dll!TerminateProcess",hle_kernel32_TerminateProcess},
{0x10d416cu,"KERNEL32.dll!GetCurrentProcessId",hle_kernel32_GetCurrentProcessId},
{0x10d4170u,"KERNEL32.dll!lstrlenW",hle_kernel32_lstrlenW},
{0x10d4174u,"KERNEL32.dll!GetComputerNameW",hle_kernel32_GetComputerNameW},
{0x10d4178u,"KERNEL32.dll!WideCharToMultiByte",hle_kernel32_WideCharToMultiByte},
{0x10d417cu,"KERNEL32.dll!QueryPerformanceFrequency",hle_kernel32_QueryPerformanceFrequency},
{0x10d4180u,"KERNEL32.dll!lstrlenA",hle_kernel32_lstrlenA},
{0x10d4184u,"KERNEL32.dll!WriteFile",hle_kernel32_WriteFile},
{0x10d4188u,"KERNEL32.dll!LocalAlloc",hle_kernel32_LocalAlloc},
{0x10d418cu,"KERNEL32.dll!DeleteFileW",hle_kernel32_DeleteFileW},
{0x10d4190u,"KERNEL32.dll!SetFileAttributesW",hle_kernel32_SetFileAttributesW},
{0x10d4194u,"KERNEL32.dll!SetThreadAffinityMask",hle_kernel32_SetThreadAffinityMask},
{0x10d4198u,"KERNEL32.dll!CreateThread",hle_kernel32_CreateThread},
{0x10d419cu,"KERNEL32.dll!SetStdHandle",hle_kernel32_SetStdHandle},
{0x10d41a0u,"KERNEL32.dll!OpenEventW",hle_kernel32_OpenEventW},
{0x10d41a4u,"KERNEL32.dll!GlobalUnlock",hle_kernel32_GlobalUnlock},
{0x10d41a8u,"KERNEL32.dll!GlobalLock",hle_kernel32_GlobalLock},
{0x10d41acu,"KERNEL32.dll!FindClose",hle_kernel32_FindClose},
{0x10d41b0u,"KERNEL32.dll!FindNextFileW",hle_kernel32_FindNextFileW},
{0x10d41b4u,"KERNEL32.dll!FindFirstFileW",hle_kernel32_FindFirstFileW},
{0x10d41b8u,"KERNEL32.dll!TerminateThread",hle_kernel32_TerminateThread},
{0x10d41bcu,"KERNEL32.dll!GlobalAlloc",hle_kernel32_GlobalAlloc},
{0x10d41c0u,"KERNEL32.dll!RemoveDirectoryW",hle_kernel32_RemoveDirectoryW},
{0x10d41c4u,"KERNEL32.dll!GetCommandLineW",hle_kernel32_GetCommandLineW},
{0x10d41c8u,"KERNEL32.dll!LoadLibraryA",hle_kernel32_LoadLibraryA},
{0x10d41ccu,"KERNEL32.dll!CopyFileW",hle_kernel32_CopyFileW},
{0x10d41d0u,"KERNEL32.dll!MoveFileW",hle_kernel32_MoveFileW},
{0x10d41d4u,"KERNEL32.dll!WriteConsoleW",hle_kernel32_WriteConsoleW},
{0x10d41d8u,"KERNEL32.dll!SetConsoleTextAttribute",hle_kernel32_SetConsoleTextAttribute},
{0x10d41dcu,"KERNEL32.dll!SetConsoleTitleW",hle_kernel32_SetConsoleTitleW},
{0x10d41e0u,"KERNEL32.dll!LockResource",hle_kernel32_LockResource},
{0x10d41e4u,"KERNEL32.dll!SizeofResource",hle_kernel32_SizeofResource},
{0x10d41e8u,"KERNEL32.dll!LoadResource",hle_kernel32_LoadResource},
{0x10d41ecu,"KERNEL32.dll!FindResourceW",hle_kernel32_FindResourceW},
{0x10d41f0u,"KERNEL32.dll!GetConsoleScreenBufferInfo",hle_kernel32_GetConsoleScreenBufferInfo},
{0x10d41f4u,"KERNEL32.dll!GetConsoleWindow",hle_kernel32_GetConsoleWindow},
{0x10d41f8u,"KERNEL32.dll!FreeConsole",hle_kernel32_FreeConsole},
{0x10d41fcu,"KERNEL32.dll!SetConsoleScreenBufferSize",hle_kernel32_SetConsoleScreenBufferSize},
{0x10d4200u,"KERNEL32.dll!GetStdHandle",hle_kernel32_GetStdHandle},
{0x10d4204u,"KERNEL32.dll!AllocConsole",hle_kernel32_AllocConsole},
{0x10d4208u,"KERNEL32.dll!GlobalMemoryStatusEx",hle_kernel32_GlobalMemoryStatusEx},
{0x10d420cu,"KERNEL32.dll!GetSystemInfo",hle_kernel32_GetSystemInfo},
{0x10d4210u,"KERNEL32.dll!GetFileSizeEx",hle_kernel32_GetFileSizeEx},
{0x10d4214u,"KERNEL32.dll!SetErrorMode",hle_kernel32_SetErrorMode},
{0x10d4218u,"KERNEL32.dll!InterlockedCompareExchange64",hle_kernel32_InterlockedCompareExchange64},
{0x10d421cu,"KERNEL32.dll!GetCurrentDirectoryA",hle_kernel32_GetCurrentDirectoryA},
{0x10d4220u,"KERNEL32.dll!VirtualAlloc",hle_kernel32_VirtualAlloc},
{0x10d4224u,"KERNEL32.dll!VirtualFree",hle_kernel32_VirtualFree},
{0x10d4228u,"KERNEL32.dll!ReleaseMutex",hle_kernel32_ReleaseMutex},
{0x10d422cu,"KERNEL32.dll!CreateMutexW",hle_kernel32_CreateMutexW},
{0x10d4230u,"KERNEL32.dll!SetUnhandledExceptionFilter",hle_kernel32_SetUnhandledExceptionFilter},
{0x10d4234u,"KERNEL32.dll!WaitForMultipleObjectsEx",hle_kernel32_WaitForMultipleObjectsEx},
{0x10d4238u,"KERNEL32.dll!CreateMutexA",hle_kernel32_CreateMutexA},
{0x10d423cu,"KERNEL32.dll!OutputDebugStringA",hle_kernel32_OutputDebugStringA},
{0x10d4240u,"KERNEL32.dll!DebugBreak",hle_kernel32_DebugBreak},
{0x10d4244u,"KERNEL32.dll!ExitThread",hle_kernel32_ExitThread},
{0x10d4248u,"KERNEL32.dll!IsDebuggerPresent",hle_kernel32_IsDebuggerPresent},
{0x10d424cu,"KERNEL32.dll!UnhandledExceptionFilter",hle_kernel32_UnhandledExceptionFilter},
{0x10d4250u,"KERNEL32.dll!GetStartupInfoA",hle_kernel32_GetStartupInfoA},
{0x10d4254u,"KERNEL32.dll!CreateEventA",hle_kernel32_CreateEventA},
{0x10d4258u,"KERNEL32.dll!CreateSemaphoreA",hle_kernel32_CreateSemaphoreA},
{0x10d425cu,"KERNEL32.dll!GetModuleHandleA",hle_kernel32_GetModuleHandleA},
{0x10d4260u,"KERNEL32.dll!VerifyVersionInfoA",hle_kernel32_VerifyVersionInfoA},
{0x10d4264u,"KERNEL32.dll!ExpandEnvironmentStringsA",hle_kernel32_ExpandEnvironmentStringsA},
{0x10d4268u,"KERNEL32.dll!SleepEx",hle_kernel32_SleepEx},
{0x10d426cu,"KERNEL32.dll!FormatMessageA",hle_kernel32_FormatMessageA},
{0x10d4270u,"KERNEL32.dll!SetLastError",hle_kernel32_SetLastError},
{0x10d4274u,"KERNEL32.dll!GetTickCount",hle_kernel32_GetTickCount},
{0x10d4278u,"KERNEL32.dll!DeleteCriticalSection",hle_kernel32_DeleteCriticalSection},
{0x10d427cu,"KERNEL32.dll!SwitchToThread",hle_kernel32_SwitchToThread},
{0x10d4280u,"KERNEL32.dll!MultiByteToWideChar",hle_kernel32_MultiByteToWideChar},
{0x10d4284u,"KERNEL32.dll!GetThreadPriority",hle_kernel32_GetThreadPriority},
{0x10d4288u,"KERNEL32.dll!InitializeSListHead",hle_kernel32_InitializeSListHead},
{0x10d428cu,"KERNEL32.dll!InterlockedPushEntrySList",hle_kernel32_InterlockedPushEntrySList},
{0x10d4290u,"KERNEL32.dll!InterlockedPopEntrySList",hle_kernel32_InterlockedPopEntrySList},
{0x10d4294u,"KERNEL32.dll!InterlockedFlushSList",hle_kernel32_InterlockedFlushSList},
{0x10d4298u,"KERNEL32.dll!GetModuleFileNameW",hle_kernel32_GetModuleFileNameW},
{0x10d429cu,"KERNEL32.dll!GetFullPathNameA",hle_kernel32_GetFullPathNameA},
{0x10d4610u,"USER32.dll!SetWindowsHookExW",hle_user32_SetWindowsHookExW},
{0x10d4614u,"USER32.dll!ScreenToClient",hle_user32_ScreenToClient},
{0x10d4618u,"USER32.dll!SetCursor",hle_user32_SetCursor},
{0x10d461cu,"USER32.dll!SetCapture",hle_user32_SetCapture},
{0x10d4620u,"USER32.dll!SetCursorPos",hle_user32_SetCursorPos},
{0x10d4624u,"USER32.dll!GetCursorPos",hle_user32_GetCursorPos},
{0x10d4628u,"USER32.dll!RegisterDeviceNotificationW",hle_user32_RegisterDeviceNotificationW},
{0x10d462cu,"USER32.dll!UnregisterDeviceNotification",hle_user32_UnregisterDeviceNotification},
{0x10d4630u,"USER32.dll!CallWindowProcW",hle_user32_CallWindowProcW},
{0x10d4634u,"USER32.dll!GetMessageA",hle_user32_GetMessageA},
{0x10d4638u,"USER32.dll!IsDialogMessageA",hle_user32_IsDialogMessageA},
{0x10d463cu,"USER32.dll!DispatchMessageA",hle_user32_DispatchMessageA},
{0x10d4640u,"USER32.dll!GetCursorInfo",hle_user32_GetCursorInfo},
{0x10d4644u,"USER32.dll!GetClipCursor",hle_user32_GetClipCursor},
{0x10d4648u,"USER32.dll!CallNextHookEx",hle_user32_CallNextHookEx},
{0x10d464cu,"USER32.dll!GetForegroundWindow",hle_user32_GetForegroundWindow},
{0x10d4650u,"USER32.dll!PeekMessageA",hle_user32_PeekMessageA},
{0x10d4654u,"USER32.dll!DefWindowProcA",hle_user32_DefWindowProcA},
{0x10d4658u,"USER32.dll!MapWindowPoints",hle_user32_MapWindowPoints},
{0x10d465cu,"USER32.dll!MoveWindow",hle_user32_MoveWindow},
{0x10d4660u,"USER32.dll!GetDesktopWindow",hle_user32_GetDesktopWindow},
{0x10d4664u,"USER32.dll!SetWindowLongA",hle_user32_SetWindowLongA},
{0x10d4668u,"USER32.dll!LoadCursorA",hle_user32_LoadCursorA},
{0x10d466cu,"USER32.dll!RegisterClassExA",hle_user32_RegisterClassExA},
{0x10d4670u,"USER32.dll!GetWindowLongA",hle_user32_GetWindowLongA},
{0x10d4674u,"USER32.dll!ValidateRect",hle_user32_ValidateRect},
{0x10d4678u,"USER32.dll!CreateWindowExA",hle_user32_CreateWindowExA},
{0x10d467cu,"USER32.dll!PostQuitMessage",hle_user32_PostQuitMessage},
{0x10d4680u,"USER32.dll!DefWindowProcW",hle_user32_DefWindowProcW},
{0x10d4684u,"USER32.dll!TranslateMessage",hle_user32_TranslateMessage},
{0x10d4688u,"USER32.dll!DispatchMessageW",hle_user32_DispatchMessageW},
{0x10d468cu,"USER32.dll!PeekMessageW",hle_user32_PeekMessageW},
{0x10d4690u,"USER32.dll!UpdateWindow",hle_user32_UpdateWindow},
{0x10d4694u,"USER32.dll!SetWindowPlacement",hle_user32_SetWindowPlacement},
{0x10d4698u,"USER32.dll!GetWindowPlacement",hle_user32_GetWindowPlacement},
{0x10d469cu,"USER32.dll!SetWindowLongW",hle_user32_SetWindowLongW},
{0x10d46a0u,"USER32.dll!GetMonitorInfoW",hle_user32_GetMonitorInfoW},
{0x10d46a4u,"USER32.dll!MonitorFromWindow",hle_user32_MonitorFromWindow},
{0x10d46a8u,"USER32.dll!SendInput",hle_user32_SendInput},
{0x10d46acu,"USER32.dll!GetWindowLongW",hle_user32_GetWindowLongW},
{0x10d46b0u,"USER32.dll!IsIconic",hle_user32_IsIconic},
{0x10d46b4u,"USER32.dll!UnregisterClassA",hle_user32_UnregisterClassA},
{0x10d46b8u,"USER32.dll!GetCapture",hle_user32_GetCapture},
{0x10d46bcu,"USER32.dll!ClientToScreen",hle_user32_ClientToScreen},
{0x10d46c0u,"USER32.dll!GetFocus",hle_user32_GetFocus},
{0x10d46c4u,"USER32.dll!CloseClipboard",hle_user32_CloseClipboard},
{0x10d46c8u,"USER32.dll!GetClipboardData",hle_user32_GetClipboardData},
{0x10d46ccu,"USER32.dll!OpenClipboard",hle_user32_OpenClipboard},
{0x10d46d0u,"USER32.dll!GetActiveWindow",hle_user32_GetActiveWindow},
{0x10d46d4u,"USER32.dll!MessageBoxW",hle_user32_MessageBoxW},
{0x10d46d8u,"USER32.dll!LoadIconW",hle_user32_LoadIconW},
{0x10d46dcu,"USER32.dll!DestroyWindow",hle_user32_DestroyWindow},
{0x10d46e0u,"USER32.dll!CreateWindowExW",hle_user32_CreateWindowExW},
{0x10d46e4u,"USER32.dll!RegisterClassW",hle_user32_RegisterClassW},
{0x10d46e8u,"USER32.dll!SetClipboardData",hle_user32_SetClipboardData},
{0x10d46ecu,"USER32.dll!EmptyClipboard",hle_user32_EmptyClipboard},
{0x10d46f0u,"USER32.dll!ShowWindow",hle_user32_ShowWindow},
{0x10d46f4u,"USER32.dll!RegisterClassExW",hle_user32_RegisterClassExW},
{0x10d46f8u,"USER32.dll!EnableWindow",hle_user32_EnableWindow},
{0x10d46fcu,"USER32.dll!IsWindowEnabled",hle_user32_IsWindowEnabled},
{0x10d4700u,"USER32.dll!GetWindowThreadProcessId",hle_user32_GetWindowThreadProcessId},
{0x10d4704u,"USER32.dll!EnumWindows",hle_user32_EnumWindows},
{0x10d4708u,"USER32.dll!GetAncestor",hle_user32_GetAncestor},
{0x10d470cu,"USER32.dll!ClipCursor",hle_user32_ClipCursor},
{0x10d4710u,"USER32.dll!ReleaseCapture",hle_user32_ReleaseCapture},
{0x10d4714u,"USER32.dll!ShowCursor",hle_user32_ShowCursor},
{0x10d4718u,"USER32.dll!SystemParametersInfoW",hle_user32_SystemParametersInfoW},
{0x10d471cu,"USER32.dll!GetSystemMetrics",hle_user32_GetSystemMetrics},
{0x10d4720u,"USER32.dll!GetWindowRect",hle_user32_GetWindowRect},
{0x10d4724u,"USER32.dll!DeleteMenu",hle_user32_DeleteMenu},
{0x10d4728u,"USER32.dll!GetSystemMenu",hle_user32_GetSystemMenu},
{0x10d472cu,"USER32.dll!SetWindowPos",hle_user32_SetWindowPos},
{0x10d4730u,"USER32.dll!PostMessageW",hle_user32_PostMessageW},
{0x10d4734u,"USER32.dll!EndPaint",hle_user32_EndPaint},
{0x10d4738u,"USER32.dll!GetClientRect",hle_user32_GetClientRect},
{0x10d473cu,"USER32.dll!DrawStateW",hle_user32_DrawStateW},
{0x10d4740u,"USER32.dll!BeginPaint",hle_user32_BeginPaint},
{0x10d4744u,"USER32.dll!InvalidateRect",hle_user32_InvalidateRect},
{0x10d4748u,"USER32.dll!UnregisterClassW",hle_user32_UnregisterClassW},
{0x10d474cu,"USER32.dll!GetMessageW",hle_user32_GetMessageW},
{0x10d4750u,"USER32.dll!LoadImageW",hle_user32_LoadImageW},
{0x10d4754u,"USER32.dll!GetKeyboardLayoutNameW",hle_user32_GetKeyboardLayoutNameW},
{0x10d4758u,"USER32.dll!EnumDisplayDevicesW",hle_user32_EnumDisplayDevicesW},
{0x10d475cu,"USER32.dll!SetActiveWindow",hle_user32_SetActiveWindow},
{0x10d4760u,"USER32.dll!SetFocus",hle_user32_SetFocus},
{0x10d4764u,"USER32.dll!AdjustWindowRect",hle_user32_AdjustWindowRect},
{0x10d4768u,"USER32.dll!SetWindowTextW",hle_user32_SetWindowTextW},
{0x10d476cu,"USER32.dll!PostThreadMessageW",hle_user32_PostThreadMessageW},
{0x10d4770u,"USER32.dll!GetKeyState",hle_user32_GetKeyState},
{0x10d4774u,"USER32.dll!UnhookWindowsHookEx",hle_user32_UnhookWindowsHookEx},
{0x10d4778u,"USER32.dll!LoadCursorW",hle_user32_LoadCursorW},
{0x10d4000u,"ADVAPI32.dll!GetUserNameW",hle_advapi32_GetUserNameW},
{0x10d4004u,"ADVAPI32.dll!CryptGetHashParam",hle_advapi32_CryptGetHashParam},
{0x10d4008u,"ADVAPI32.dll!CryptHashData",hle_advapi32_CryptHashData},
{0x10d400cu,"ADVAPI32.dll!CryptReleaseContext",hle_advapi32_CryptReleaseContext},
{0x10d4010u,"ADVAPI32.dll!CryptDestroyKey",hle_advapi32_CryptDestroyKey},
{0x10d4014u,"ADVAPI32.dll!CryptDestroyHash",hle_advapi32_CryptDestroyHash},
{0x10d4018u,"ADVAPI32.dll!CryptVerifySignatureW",hle_advapi32_CryptVerifySignatureW},
{0x10d401cu,"ADVAPI32.dll!CryptCreateHash",hle_advapi32_CryptCreateHash},
{0x10d4020u,"ADVAPI32.dll!CryptImportKey",hle_advapi32_CryptImportKey},
{0x10d4024u,"ADVAPI32.dll!CryptAcquireContextW",hle_advapi32_CryptAcquireContextW},
{0x10d4028u,"ADVAPI32.dll!CryptAcquireContextA",hle_advapi32_CryptAcquireContextA},
{0x10d402cu,"ADVAPI32.dll!CryptGenRandom",hle_advapi32_CryptGenRandom},
{0x10d404cu,"GDI32.dll!SelectObject",hle_gdi32_SelectObject},
{0x10d4050u,"GDI32.dll!TextOutW",hle_gdi32_TextOutW},
{0x10d4054u,"GDI32.dll!SetTextColor",hle_gdi32_SetTextColor},
{0x10d4058u,"GDI32.dll!SetBkMode",hle_gdi32_SetBkMode},
{0x10d405cu,"GDI32.dll!GetStockObject",hle_gdi32_GetStockObject},
{0x10d4060u,"GDI32.dll!GetObjectW",hle_gdi32_GetObjectW},
{0x10d4064u,"GDI32.dll!CreateFontIndirectW",hle_gdi32_CreateFontIndirectW},
{0x10d4068u,"GDI32.dll!DeleteObject",hle_gdi32_DeleteObject},
{0x10d406cu,"GDI32.dll!SetBkColor",hle_gdi32_SetBkColor},
{0x10d4900u,"steam_api.dll!SteamAPI_SetMiniDumpComment",hle_steam_api_SteamAPI_SetMiniDumpComment},
{0x10d4904u,"steam_api.dll!SteamAPI_WriteMiniDump",hle_steam_api_SteamAPI_WriteMiniDump},
{0x10d4908u,"steam_api.dll!SteamAPI_RunCallbacks",hle_steam_api_SteamAPI_RunCallbacks},
{0x10d490cu,"steam_api.dll!SteamGameServer_RunCallbacks",hle_steam_api_SteamGameServer_RunCallbacks},
{0x10d4910u,"steam_api.dll!SteamAPI_Init",hle_steam_api_SteamAPI_Init},
{0x10d4914u,"steam_api.dll!SteamAPI_RegisterCallback",hle_steam_api_SteamAPI_RegisterCallback},
{0x10d4918u,"steam_api.dll!SteamAPI_UnregisterCallResult",hle_steam_api_SteamAPI_UnregisterCallResult},
{0x10d491cu,"steam_api.dll!SteamAPI_RegisterCallResult",hle_steam_api_SteamAPI_RegisterCallResult},
{0x10d4920u,"steam_api.dll!SteamAPI_UnregisterCallback",hle_steam_api_SteamAPI_UnregisterCallback},
{0x10d4924u,"steam_api.dll!SteamGameServer_Shutdown",hle_steam_api_SteamGameServer_Shutdown},
{0x10d4928u,"steam_api.dll!SteamAPI_Shutdown",hle_steam_api_SteamAPI_Shutdown},
{0x10d492cu,"steam_api.dll!SteamGameServer_BSecure",hle_steam_api_SteamGameServer_BSecure},
{0x10d4930u,"steam_api.dll!SteamGameServer_GetSteamID",hle_steam_api_SteamGameServer_GetSteamID},
{0x10d4934u,"steam_api.dll!SteamInternal_GameServer_Init",hle_steam_api_SteamInternal_GameServer_Init},
{0x10d4938u,"steam_api.dll!SteamGameServer_GetHSteamUser",hle_steam_api_SteamGameServer_GetHSteamUser},
{0x10d493cu,"steam_api.dll!SteamInternal_FindOrCreateGameServerInterface",hle_steam_api_SteamInternal_FindOrCreateGameServerInterface},
{0x10d4940u,"steam_api.dll!SteamAPI_GetHSteamUser",hle_steam_api_SteamAPI_GetHSteamUser},
{0x10d4944u,"steam_api.dll!SteamInternal_CreateInterface",hle_steam_api_SteamInternal_CreateInterface},
{0x10d4948u,"steam_api.dll!SteamInternal_ContextInit",hle_steam_api_SteamInternal_ContextInit},
{0x10d494cu,"steam_api.dll!SteamInternal_FindOrCreateUserInterface",hle_steam_api_SteamInternal_FindOrCreateUserInterface},
{0x10d4044u,"DINPUT8.dll!DirectInput8Create",hle_dinput8_DirectInput8Create},
{0x10d4814u,"XINPUT1_3.dll!ord2",hle_xinput1_3_ord2},
{0x10d4818u,"XINPUT1_3.dll!ord3",hle_xinput1_3_ord3},
{0x10d4820u,"d3d9.dll!D3DPERF_BeginEvent",hle_d3d9_D3DPERF_BeginEvent},
{0x10d4824u,"d3d9.dll!D3DPERF_EndEvent",hle_d3d9_D3DPERF_EndEvent},
{0x10d4828u,"d3d9.dll!Direct3DCreate9",hle_d3d9_Direct3DCreate9},
{0x10d4838u,"d3dx9_43.dll!D3DXMatrixLookAtRH",hle_d3dx9_43_D3DXMatrixLookAtRH},
{0x10d483cu,"d3dx9_43.dll!D3DXMatrixTranslation",hle_d3dx9_43_D3DXMatrixTranslation},
{0x10d4840u,"d3dx9_43.dll!D3DXMatrixMultiply",hle_d3dx9_43_D3DXMatrixMultiply},
{0x10d4844u,"d3dx9_43.dll!D3DXMatrixScaling",hle_d3dx9_43_D3DXMatrixScaling},
{0x10d4848u,"d3dx9_43.dll!D3DXMatrixInverse",hle_d3dx9_43_D3DXMatrixInverse},
{0x10d484cu,"d3dx9_43.dll!D3DXMatrixOrthoOffCenterRH",hle_d3dx9_43_D3DXMatrixOrthoOffCenterRH},
{0x10d4850u,"d3dx9_43.dll!D3DXOptimizeFaces",hle_d3dx9_43_D3DXOptimizeFaces},
{0x10d4854u,"d3dx9_43.dll!D3DXVec3Transform",hle_d3dx9_43_D3DXVec3Transform},
{0x10d4830u,"d3dx11_43.dll!D3DX11CheckVersion",hle_d3dx11_43_D3DX11CheckVersion},
{0x10d4898u,"nvtt.dll!??0InputOptions@nvtt@@QAE@XZ",hle_nvtt___0InputOptions_nvtt__QAE_XZ},
{0x10d489cu,"nvtt.dll!?setTextureLayout@InputOptions@nvtt@@QAEXW4TextureType@2@HHH@Z",hle_nvtt__setTextureLayout_InputOptions_nvtt__QAEXW4TextureType_2_HHH_Z},
{0x10d48a0u,"nvtt.dll!?setMipmapGeneration@InputOptions@nvtt@@QAEX_NH@Z",hle_nvtt__setMipmapGeneration_InputOptions_nvtt__QAEX_NH_Z},
{0x10d48a4u,"nvtt.dll!?setMipmapData@InputOptions@nvtt@@QAE_NPBXHHHHH@Z",hle_nvtt__setMipmapData_InputOptions_nvtt__QAE_NPBXHHHHH_Z},
{0x10d48a8u,"nvtt.dll!??0CompressionOptions@nvtt@@QAE@XZ",hle_nvtt___0CompressionOptions_nvtt__QAE_XZ},
{0x10d48acu,"nvtt.dll!?setQuality@CompressionOptions@nvtt@@QAEXW4Quality@2@@Z",hle_nvtt__setQuality_CompressionOptions_nvtt__QAEXW4Quality_2__Z},
{0x10d48b0u,"nvtt.dll!??1CompressionOptions@nvtt@@QAE@XZ",hle_nvtt___1CompressionOptions_nvtt__QAE_XZ},
{0x10d48b4u,"nvtt.dll!??1Compressor@nvtt@@QAE@XZ",hle_nvtt___1Compressor_nvtt__QAE_XZ},
{0x10d48b8u,"nvtt.dll!??1OutputOptions@nvtt@@QAE@XZ",hle_nvtt___1OutputOptions_nvtt__QAE_XZ},
{0x10d48bcu,"nvtt.dll!?process@Compressor@nvtt@@QBE_NABUInputOptions@2@ABUCompressionOptions@2@ABUOutputOptions@2@@Z",hle_nvtt__process_Compressor_nvtt__QBE_NABUInputOptions_2_ABUCompressionOptions_2_ABUOutputOptions_2__Z},
{0x10d48c0u,"nvtt.dll!?setErrorHandler@OutputOptions@nvtt@@QAEXPAUErrorHandler@2@@Z",hle_nvtt__setErrorHandler_OutputOptions_nvtt__QAEXPAUErrorHandler_2__Z},
{0x10d48c4u,"nvtt.dll!?setFormat@CompressionOptions@nvtt@@QAEXW4Format@2@@Z",hle_nvtt__setFormat_CompressionOptions_nvtt__QAEXW4Format_2__Z},
{0x10d48c8u,"nvtt.dll!??1InputOptions@nvtt@@QAE@XZ",hle_nvtt___1InputOptions_nvtt__QAE_XZ},
{0x10d48ccu,"nvtt.dll!??0Compressor@nvtt@@QAE@XZ",hle_nvtt___0Compressor_nvtt__QAE_XZ},
{0x10d48d0u,"nvtt.dll!?enableCudaAcceleration@Compressor@nvtt@@QAEX_N@Z",hle_nvtt__enableCudaAcceleration_Compressor_nvtt__QAEX_N_Z},
{0x10d48d4u,"nvtt.dll!??0OutputOptions@nvtt@@QAE@XZ",hle_nvtt___0OutputOptions_nvtt__QAE_XZ},
{0x10d48d8u,"nvtt.dll!?setOutputHandler@OutputOptions@nvtt@@QAEXPAUOutputHandler@2@@Z",hle_nvtt__setOutputHandler_OutputOptions_nvtt__QAEXPAUOutputHandler_2__Z},
{0x10d485cu,"dbghelp.dll!StackWalk64",hle_dbghelp_StackWalk64},
{0x10d4860u,"dbghelp.dll!SymFunctionTableAccess64",hle_dbghelp_SymFunctionTableAccess64},
{0x10d4864u,"dbghelp.dll!SymGetModuleBase64",hle_dbghelp_SymGetModuleBase64},
{0x10d4868u,"dbghelp.dll!SymLoadModule64",hle_dbghelp_SymLoadModule64},
{0x10d486cu,"dbghelp.dll!SymGetModuleInfo64",hle_dbghelp_SymGetModuleInfo64},
{0x10d4870u,"dbghelp.dll!SymGetSymFromAddr64",hle_dbghelp_SymGetSymFromAddr64},
{0x10d4874u,"dbghelp.dll!MiniDumpWriteDump",hle_dbghelp_MiniDumpWriteDump},
{0x10d4878u,"dbghelp.dll!SymSetSearchPath",hle_dbghelp_SymSetSearchPath},
{0x10d487cu,"dbghelp.dll!SymGetLineFromAddr64",hle_dbghelp_SymGetLineFromAddr64},
{0x10d4880u,"dbghelp.dll!SymInitialize",hle_dbghelp_SymInitialize},
{0x10d4884u,"dbghelp.dll!SymSetOptions",hle_dbghelp_SymSetOptions},
{0x10d4888u,"dbghelp.dll!SymGetOptions",hle_dbghelp_SymGetOptions},
{0x10d4780u,"WINMM.dll!timeBeginPeriod",hle_winmm_timeBeginPeriod},
{0x10d4784u,"WINMM.dll!timeGetTime",hle_winmm_timeGetTime},
{0x10d45f4u,"SHELL32.dll!SHGetFolderPathW",hle_shell32_SHGetFolderPathW},
{0x10d45f8u,"SHELL32.dll!ShellExecuteW",hle_shell32_ShellExecuteW},
{0x10d48e0u,"ole32.dll!CoSetProxyBlanket",hle_ole32_CoSetProxyBlanket},
{0x10d48e4u,"ole32.dll!CoInitialize",hle_ole32_CoInitialize},
{0x10d48e8u,"ole32.dll!CoCreateInstance",hle_ole32_CoCreateInstance},
{0x10d48ecu,"ole32.dll!CoUninitialize",hle_ole32_CoUninitialize},
{0x10d48f0u,"ole32.dll!CoCreateGuid",hle_ole32_CoCreateGuid},
{0x10d48f4u,"ole32.dll!CoInitializeEx",hle_ole32_CoInitializeEx},
{0x10d48f8u,"ole32.dll!StringFromGUID2",hle_ole32_StringFromGUID2},
{0x10d45d8u,"OLEAUT32.dll!SysFreeString",hle_oleaut32_SysFreeString},
{0x10d45dcu,"OLEAUT32.dll!SysAllocString",hle_oleaut32_SysAllocString},
{0x10d4600u,"SHLWAPI.dll!PathRemoveBlanksW",hle_shlwapi_PathRemoveBlanksW},
{0x10d4604u,"SHLWAPI.dll!PathMatchSpecW",hle_shlwapi_PathMatchSpecW},
{0x10d4608u,"SHLWAPI.dll!PathFindFileNameW",hle_shlwapi_PathFindFileNameW},
{0x10d42a4u,"MSVCP90.dll!??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z",hle_msvcp90___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_ABV01__Z},
{0x10d42a8u,"MSVCP90.dll!??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ",hle_msvcp90___1__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_XZ},
{0x10d42acu,"MSVCP90.dll!?find@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEIPBDII@Z",hle_msvcp90__find___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QBEIPBDII_Z},
{0x10d42b0u,"MSVCP90.dll!?npos@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@2IB",hle_msvcp90__npos___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__2IB},
{0x10d42b4u,"MSVCP90.dll!?substr@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBE?AV12@II@Z",hle_msvcp90__substr___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QBE_AV12_II_Z},
{0x10d42b8u,"MSVCP90.dll!??4?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@ABV01@@Z",hle_msvcp90___4__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV01_ABV01__Z},
{0x10d42bcu,"MSVCP90.dll!?erase@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV12@II@Z",hle_msvcp90__erase___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV12_II_Z},
{0x10d42c0u,"MSVCP90.dll!??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z",hle_msvcp90___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_PBD_Z},
{0x10d42c4u,"MSVCP90.dll!??Y?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@PBD@Z",hle_msvcp90___Y__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV01_PBD_Z},
{0x10d42ccu,"MSVCR90.dll!_set_SSE2_enable",hle_msvcr90__set_SSE2_enable},
{0x10d42d0u,"MSVCR90.dll!_except_handler3",hle_msvcr90__except_handler3},
{0x10d42d4u,"MSVCR90.dll!_CIexp",hle_msvcr90__CIexp},
{0x10d42d8u,"MSVCR90.dll!_wstat64",hle_msvcr90__wstat64},
{0x10d42dcu,"MSVCR90.dll!_wfopen_s",hle_msvcr90__wfopen_s},
{0x10d42e0u,"MSVCR90.dll!rewind",hle_msvcr90_rewind},
{0x10d42e4u,"MSVCR90.dll!_itoa_s",hle_msvcr90__itoa_s},
{0x10d42e8u,"MSVCR90.dll!_controlfp_s",hle_msvcr90__controlfp_s},
{0x10d42ecu,"MSVCR90.dll!localeconv",hle_msvcr90_localeconv},
{0x10d42f0u,"MSVCR90.dll!strcspn",hle_msvcr90_strcspn},
{0x10d42f4u,"MSVCR90.dll!strspn",hle_msvcr90_strspn},
{0x10d42f8u,"MSVCR90.dll!wcscoll",hle_msvcr90_wcscoll},
{0x10d42fcu,"MSVCR90.dll!_wcsicoll",hle_msvcr90__wcsicoll},
{0x10d4300u,"MSVCR90.dll!_CItan",hle_msvcr90__CItan},
{0x10d4304u,"MSVCR90.dll!_CIatan2",hle_msvcr90__CIatan2},
{0x10d4308u,"MSVCR90.dll!_strtoui64",hle_msvcr90__strtoui64},
{0x10d430cu,"MSVCR90.dll!strtod",hle_msvcr90_strtod},
{0x10d4310u,"MSVCR90.dll!_gmtime64",hle_msvcr90__gmtime64},
{0x10d4314u,"MSVCR90.dll!_stat64",hle_msvcr90__stat64},
{0x10d4318u,"MSVCR90.dll!getenv",hle_msvcr90_getenv},
{0x10d431cu,"MSVCR90.dll!sprintf",hle_msvcr90_sprintf},
{0x10d4320u,"MSVCR90.dll!fputs",hle_msvcr90_fputs},
{0x10d4324u,"MSVCR90.dll!memchr",hle_msvcr90_memchr},
{0x10d4328u,"MSVCR90.dll!_strtoi64",hle_msvcr90__strtoi64},
{0x10d432cu,"MSVCR90.dll!isdigit",hle_msvcr90_isdigit},
{0x10d4330u,"MSVCR90.dll!strpbrk",hle_msvcr90_strpbrk},
{0x10d4334u,"MSVCR90.dll!strtoul",hle_msvcr90_strtoul},
{0x10d4338u,"MSVCR90.dll!__sys_nerr",hle_msvcr90___sys_nerr},
{0x10d433cu,"MSVCR90.dll!strerror",hle_msvcr90_strerror},
{0x10d4340u,"MSVCR90.dll!_unlock",hle_msvcr90__unlock},
{0x10d4344u,"MSVCR90.dll!__dllonexit",hle_msvcr90___dllonexit},
{0x10d4348u,"MSVCR90.dll!_encode_pointer",hle_msvcr90__encode_pointer},
{0x10d434cu,"MSVCR90.dll!_lock",hle_msvcr90__lock},
{0x10d4350u,"MSVCR90.dll!_onexit",hle_msvcr90__onexit},
{0x10d4354u,"MSVCR90.dll!_decode_pointer",hle_msvcr90__decode_pointer},
{0x10d4358u,"MSVCR90.dll!_except_handler4_common",hle_msvcr90__except_handler4_common},
{0x10d435cu,"MSVCR90.dll!?terminate@@YAXXZ",hle_msvcr90__terminate__YAXXZ},
{0x10d4360u,"MSVCR90.dll!_vsnprintf",hle_msvcr90__vsnprintf},
{0x10d4364u,"MSVCR90.dll!_amsg_exit",hle_msvcr90__amsg_exit},
{0x10d4368u,"MSVCR90.dll!__getmainargs",hle_msvcr90___getmainargs},
{0x10d436cu,"MSVCR90.dll!_cexit",hle_msvcr90__cexit},
{0x10d4370u,"MSVCR90.dll!_exit",hle_msvcr90__exit},
{0x10d4374u,"MSVCR90.dll!_XcptFilter",hle_msvcr90__XcptFilter},
{0x10d4378u,"MSVCR90.dll!_acmdln",hle_msvcr90__acmdln},
{0x10d437cu,"MSVCR90.dll!_initterm",hle_msvcr90__initterm},
{0x10d4380u,"MSVCR90.dll!_initterm_e",hle_msvcr90__initterm_e},
{0x10d4384u,"MSVCR90.dll!_configthreadlocale",hle_msvcr90__configthreadlocale},
{0x10d4388u,"MSVCR90.dll!__setusermatherr",hle_msvcr90___setusermatherr},
{0x10d438cu,"MSVCR90.dll!_adjust_fdiv",hle_msvcr90__adjust_fdiv},
{0x10d4390u,"MSVCR90.dll!strrchr",hle_msvcr90_strrchr},
{0x10d4394u,"MSVCR90.dll!calloc",hle_msvcr90_calloc},
{0x10d4398u,"MSVCR90.dll!isalnum",hle_msvcr90_isalnum},
{0x10d439cu,"MSVCR90.dll!isalpha",hle_msvcr90_isalpha},
{0x10d43a0u,"MSVCR90.dll!atof",hle_msvcr90_atof},
{0x10d43a4u,"MSVCR90.dll!sscanf",hle_msvcr90_sscanf},
{0x10d43a8u,"MSVCR90.dll!fprintf",hle_msvcr90_fprintf},
{0x10d43acu,"MSVCR90.dll!fputc",hle_msvcr90_fputc},
{0x10d43b0u,"MSVCR90.dll!ferror",hle_msvcr90_ferror},
{0x10d43b4u,"MSVCR90.dll!_fpclass",hle_msvcr90__fpclass},
{0x10d43b8u,"MSVCR90.dll!sprintf_s",hle_msvcr90_sprintf_s},
{0x10d43bcu,"MSVCR90.dll!strchr",hle_msvcr90_strchr},
{0x10d43c0u,"MSVCR90.dll!_aligned_free",hle_msvcr90__aligned_free},
{0x10d43c4u,"MSVCR90.dll!_aligned_malloc",hle_msvcr90__aligned_malloc},
{0x10d43c8u,"MSVCR90.dll!printf",hle_msvcr90_printf},
{0x10d43ccu,"MSVCR90.dll!fgets",hle_msvcr90_fgets},
{0x10d43d0u,"MSVCR90.dll!vsprintf",hle_msvcr90_vsprintf},
{0x10d43d4u,"MSVCR90.dll!isxdigit",hle_msvcr90_isxdigit},
{0x10d43d8u,"MSVCR90.dll!tolower",hle_msvcr90_tolower},
{0x10d43dcu,"MSVCR90.dll!_CIatan",hle_msvcr90__CIatan},
{0x10d43e0u,"MSVCR90.dll!__p__commode",hle_msvcr90___p__commode},
{0x10d43e4u,"MSVCR90.dll!__p__fmode",hle_msvcr90___p__fmode},
{0x10d43e8u,"MSVCR90.dll!_CIlog",hle_msvcr90__CIlog},
{0x10d43ecu,"MSVCR90.dll!_beginthreadex",hle_msvcr90__beginthreadex},
{0x10d43f0u,"MSVCR90.dll!_endthreadex",hle_msvcr90__endthreadex},
{0x10d43f4u,"MSVCR90.dll!_CIsin",hle_msvcr90__CIsin},
{0x10d43f8u,"MSVCR90.dll!_CIcos",hle_msvcr90__CIcos},
{0x10d43fcu,"MSVCR90.dll!_CIsqrt",hle_msvcr90__CIsqrt},
{0x10d4400u,"MSVCR90.dll!_vswprintf_c_l",hle_msvcr90__vswprintf_c_l},
{0x10d4404u,"MSVCR90.dll!wcsncmp",hle_msvcr90_wcsncmp},
{0x10d4408u,"MSVCR90.dll!_set_purecall_handler",hle_msvcr90__set_purecall_handler},
{0x10d440cu,"MSVCR90.dll!_set_invalid_parameter_handler",hle_msvcr90__set_invalid_parameter_handler},
{0x10d4410u,"MSVCR90.dll!__iob_func",hle_msvcr90___iob_func},
{0x10d4414u,"MSVCR90.dll!fflush",hle_msvcr90_fflush},
{0x10d4418u,"MSVCR90.dll!fopen",hle_msvcr90_fopen},
{0x10d441cu,"MSVCR90.dll!fwrite",hle_msvcr90_fwrite},
{0x10d4420u,"MSVCR90.dll!_stat64i32",hle_msvcr90__stat64i32},
{0x10d4424u,"MSVCR90.dll!_errno",hle_msvcr90__errno},
{0x10d4428u,"MSVCR90.dll!strftime",hle_msvcr90_strftime},
{0x10d442cu,"MSVCR90.dll!strncpy",hle_msvcr90_strncpy},
{0x10d4430u,"MSVCR90.dll!_localtime64",hle_msvcr90__localtime64},
{0x10d4434u,"MSVCR90.dll!fclose",hle_msvcr90_fclose},
{0x10d4438u,"MSVCR90.dll!ftell",hle_msvcr90_ftell},
{0x10d443cu,"MSVCR90.dll!strtol",hle_msvcr90_strtol},
{0x10d4440u,"MSVCR90.dll!fopen_s",hle_msvcr90_fopen_s},
{0x10d4444u,"MSVCR90.dll!isspace",hle_msvcr90_isspace},
{0x10d4448u,"MSVCR90.dll!_getcwd",hle_msvcr90__getcwd},
{0x10d444cu,"MSVCR90.dll!realloc",hle_msvcr90_realloc},
{0x10d4450u,"MSVCR90.dll!ldiv",hle_msvcr90_ldiv},
{0x10d4454u,"MSVCR90.dll!feof",hle_msvcr90_feof},
{0x10d4458u,"MSVCR90.dll!fseek",hle_msvcr90_fseek},
{0x10d445cu,"MSVCR90.dll!fread",hle_msvcr90_fread},
{0x10d4460u,"MSVCR90.dll!vswprintf_s",hle_msvcr90_vswprintf_s},
{0x10d4464u,"MSVCR90.dll!_snprintf",hle_msvcr90__snprintf},
{0x10d4468u,"MSVCR90.dll!_wtoi64",hle_msvcr90__wtoi64},
{0x10d446cu,"MSVCR90.dll!strncmp",hle_msvcr90_strncmp},
{0x10d4470u,"MSVCR90.dll!ceil",hle_msvcr90_ceil},
{0x10d4474u,"MSVCR90.dll!strstr",hle_msvcr90_strstr},
{0x10d4478u,"MSVCR90.dll!memmove_s",hle_msvcr90_memmove_s},
{0x10d447cu,"MSVCR90.dll!??0exception@std@@QAE@ABV01@@Z",hle_msvcr90___0exception_std__QAE_ABV01__Z},
{0x10d4480u,"MSVCR90.dll!_invalid_parameter_noinfo",hle_msvcr90__invalid_parameter_noinfo},
{0x10d4484u,"MSVCR90.dll!??0exception@std@@QAE@XZ",hle_msvcr90___0exception_std__QAE_XZ},
{0x10d4488u,"MSVCR90.dll!vsprintf_s",hle_msvcr90_vsprintf_s},
{0x10d448cu,"MSVCR90.dll!??1exception@std@@UAE@XZ",hle_msvcr90___1exception_std__UAE_XZ},
{0x10d4490u,"MSVCR90.dll!?what@exception@std@@UBEPBDXZ",hle_msvcr90__what_exception_std__UBEPBDXZ},
{0x10d4494u,"MSVCR90.dll!??0exception@std@@QAE@ABQBD@Z",hle_msvcr90___0exception_std__QAE_ABQBD_Z},
{0x10d4498u,"MSVCR90.dll!__libm_sse2_exp",hle_msvcr90___libm_sse2_exp},
{0x10d449cu,"MSVCR90.dll!__libm_sse2_log",hle_msvcr90___libm_sse2_log},
{0x10d44a0u,"MSVCR90.dll!_finite",hle_msvcr90__finite},
{0x10d44a4u,"MSVCR90.dll!_isnan",hle_msvcr90__isnan},
{0x10d44a8u,"MSVCR90.dll!__libm_sse2_atan",hle_msvcr90___libm_sse2_atan},
{0x10d44acu,"MSVCR90.dll!__libm_sse2_tan",hle_msvcr90___libm_sse2_tan},
{0x10d44b0u,"MSVCR90.dll!iswpunct",hle_msvcr90_iswpunct},
{0x10d44b4u,"MSVCR90.dll!srand",hle_msvcr90_srand},
{0x10d44b8u,"MSVCR90.dll!swscanf_s",hle_msvcr90_swscanf_s},
{0x10d44bcu,"MSVCR90.dll!exit",hle_msvcr90_exit},
{0x10d44c0u,"MSVCR90.dll!free",hle_msvcr90_free},
{0x10d44c4u,"MSVCR90.dll!malloc",hle_msvcr90_malloc},
{0x10d44c8u,"MSVCR90.dll!__set_app_type",hle_msvcr90___set_app_type},
{0x10d44ccu,"MSVCR90.dll!_crt_debugger_hook",hle_msvcr90__crt_debugger_hook},
{0x10d44d0u,"MSVCR90.dll!_vscwprintf_p",hle_msvcr90__vscwprintf_p},
{0x10d44d4u,"MSVCR90.dll!_vsnprintf_s",hle_msvcr90__vsnprintf_s},
{0x10d44d8u,"MSVCR90.dll!_vsnwprintf_s",hle_msvcr90__vsnwprintf_s},
{0x10d44dcu,"MSVCR90.dll!_set_abort_behavior",hle_msvcr90__set_abort_behavior},
{0x10d44e0u,"MSVCR90.dll!abort",hle_msvcr90_abort},
{0x10d44e4u,"MSVCR90.dll!wprintf",hle_msvcr90_wprintf},
{0x10d44e8u,"MSVCR90.dll!toupper",hle_msvcr90_toupper},
{0x10d44ecu,"MSVCR90.dll!__libm_sse2_acos",hle_msvcr90___libm_sse2_acos},
{0x10d44f0u,"MSVCR90.dll!__libm_sse2_asin",hle_msvcr90___libm_sse2_asin},
{0x10d44f4u,"MSVCR90.dll!__libm_sse2_atan2",hle_msvcr90___libm_sse2_atan2},
{0x10d44f8u,"MSVCR90.dll!qsort",hle_msvcr90_qsort},
{0x10d44fcu,"MSVCR90.dll!strncpy_s",hle_msvcr90_strncpy_s},
{0x10d4500u,"MSVCR90.dll!wcsncpy_s",hle_msvcr90_wcsncpy_s},
{0x10d4504u,"MSVCR90.dll!_wstrtime_s",hle_msvcr90__wstrtime_s},
{0x10d4508u,"MSVCR90.dll!_wstrdate_s",hle_msvcr90__wstrdate_s},
{0x10d450cu,"MSVCR90.dll!_wstat64i32",hle_msvcr90__wstat64i32},
{0x10d4510u,"MSVCR90.dll!?_type_info_dtor_internal_method@type_info@@QAEXXZ",hle_msvcr90___type_info_dtor_internal_method_type_info__QAEXXZ},
{0x10d4514u,"MSVCR90.dll!_invoke_watson",hle_msvcr90__invoke_watson},
{0x10d4518u,"MSVCR90.dll!_setjmp3",hle_msvcr90__setjmp3},
{0x10d451cu,"MSVCR90.dll!longjmp",hle_msvcr90_longjmp},
{0x10d4520u,"MSVCR90.dll!_iob",hle_msvcr90__iob},
{0x10d4524u,"MSVCR90.dll!strlen",hle_msvcr90_strlen},
{0x10d4528u,"MSVCR90.dll!wctob",hle_msvcr90_wctob},
{0x10d452cu,"MSVCR90.dll!abs",hle_msvcr90_abs},
{0x10d4530u,"MSVCR90.dll!_strdup",hle_msvcr90__strdup},
{0x10d4534u,"MSVCR90.dll!vprintf",hle_msvcr90_vprintf},
{0x10d4538u,"MSVCR90.dll!_snprintf_s",hle_msvcr90__snprintf_s},
{0x10d453cu,"MSVCR90.dll!wprintf_s",hle_msvcr90_wprintf_s},
{0x10d4540u,"MSVCR90.dll!_snwprintf_s",hle_msvcr90__snwprintf_s},
{0x10d4544u,"MSVCR90.dll!printf_s",hle_msvcr90_printf_s},
{0x10d4548u,"MSVCR90.dll!_ismbblead",hle_msvcr90__ismbblead},
{0x10d454cu,"MSVCR90.dll!_wcsicmp",hle_msvcr90__wcsicmp},
{0x10d4550u,"MSVCR90.dll!_purecall",hle_msvcr90__purecall},
{0x10d4554u,"MSVCR90.dll!__CxxFrameHandler3",hle_msvcr90___CxxFrameHandler3},
{0x10d4558u,"MSVCR90.dll!__libm_sse2_cos",hle_msvcr90___libm_sse2_cos},
{0x10d455cu,"MSVCR90.dll!memcpy",hle_msvcr90_memcpy},
{0x10d4560u,"MSVCR90.dll!memset",hle_msvcr90_memset},
{0x10d4564u,"MSVCR90.dll!_CIacos",hle_msvcr90__CIacos},
{0x10d4568u,"MSVCR90.dll!_CIasin",hle_msvcr90__CIasin},
{0x10d456cu,"MSVCR90.dll!floor",hle_msvcr90_floor},
{0x10d4570u,"MSVCR90.dll!_CIfmod",hle_msvcr90__CIfmod},
{0x10d4574u,"MSVCR90.dll!_CIpow",hle_msvcr90__CIpow},
{0x10d4578u,"MSVCR90.dll!rand",hle_msvcr90_rand},
{0x10d457cu,"MSVCR90.dll!memmove",hle_msvcr90_memmove},
{0x10d4580u,"MSVCR90.dll!strcpy_s",hle_msvcr90_strcpy_s},
{0x10d4584u,"MSVCR90.dll!wcscpy_s",hle_msvcr90_wcscpy_s},
{0x10d4588u,"MSVCR90.dll!wcscat_s",hle_msvcr90_wcscat_s},
{0x10d458cu,"MSVCR90.dll!strcat_s",hle_msvcr90_strcat_s},
{0x10d4590u,"MSVCR90.dll!wcsstr",hle_msvcr90_wcsstr},
{0x10d4594u,"MSVCR90.dll!wcschr",hle_msvcr90_wcschr},
{0x10d4598u,"MSVCR90.dll!wcsrchr",hle_msvcr90_wcsrchr},
{0x10d459cu,"MSVCR90.dll!_stricmp",hle_msvcr90__stricmp},
{0x10d45a0u,"MSVCR90.dll!_wtoi",hle_msvcr90__wtoi},
{0x10d45a4u,"MSVCR90.dll!atoi",hle_msvcr90_atoi},
{0x10d45a8u,"MSVCR90.dll!_wtof",hle_msvcr90__wtof},
{0x10d45acu,"MSVCR90.dll!wcstod",hle_msvcr90_wcstod},
{0x10d45b0u,"MSVCR90.dll!wcstoul",hle_msvcr90_wcstoul},
{0x10d45b4u,"MSVCR90.dll!_wcstoui64",hle_msvcr90__wcstoui64},
{0x10d45b8u,"MSVCR90.dll!_wcsnicmp",hle_msvcr90__wcsnicmp},
{0x10d45bcu,"MSVCR90.dll!__libm_sse2_sin",hle_msvcr90___libm_sse2_sin},
{0x10d45c0u,"MSVCR90.dll!__libm_sse2_pow",hle_msvcr90___libm_sse2_pow},
{0x10d45c4u,"MSVCR90.dll!_CxxThrowException",hle_msvcr90__CxxThrowException},
{0x10d45c8u,"MSVCR90.dll!_wutime64",hle_msvcr90__wutime64},
{0x10d45ccu,"MSVCR90.dll!_gmtime64_s",hle_msvcr90__gmtime64_s},
{0x10d45d0u,"MSVCR90.dll!_time64",hle_msvcr90__time64},
{0x10d4034u,"ApexFrameworkSHIPPING_x86.dll!NxCreateApexSDK",hle_apexframeworkshipping_x86_NxCreateApexSDK},
{0x10d45e4u,"POWRPROF.dll!CallNtPowerInformation",hle_powrprof_CallNtPowerInformation},
{0x10d45ecu,"PSAPI.DLL!GetProcessMemoryInfo",hle_psapi_GetProcessMemoryInfo},
{0x10d4890u,"faultrep.dll!ReportFault",hle_faultrep_ReportFault},
{0x10d478cu,"WS2_32.dll!WSAIoctl",hle_ws2_32_WSAIoctl},
{0x10d4790u,"WS2_32.dll!freeaddrinfo",hle_ws2_32_freeaddrinfo},
{0x10d4794u,"WS2_32.dll!getaddrinfo",hle_ws2_32_getaddrinfo},
};
int g_nimps=571;
static hle_fn stubs[]={stub_0,stub_1,stub_2,stub_3,stub_4,stub_5,stub_6,stub_7,stub_8,stub_9,stub_10,stub_11,stub_12,stub_13,stub_14,stub_15,stub_16,stub_17,stub_18,stub_19,stub_20,stub_21,stub_22,stub_23,stub_24,stub_25,stub_26,stub_27,stub_28,stub_29,stub_30,stub_31,stub_32,stub_33,stub_34,stub_35,stub_36,stub_37,stub_38,stub_39,stub_40,stub_41,stub_42,stub_43,stub_44,stub_45,stub_46,stub_47,stub_48,stub_49,stub_50,stub_51,stub_52,stub_53,stub_54,stub_55,stub_56,stub_57,stub_58,stub_59,stub_60,stub_61,stub_62,stub_63,stub_64,stub_65,stub_66,stub_67,stub_68,stub_69,stub_70,stub_71,stub_72,stub_73,stub_74,stub_75,stub_76,stub_77,stub_78,stub_79,stub_80,stub_81,stub_82,stub_83,stub_84,stub_85,stub_86,stub_87,stub_88,stub_89,stub_90,stub_91,stub_92,stub_93,stub_94,stub_95,stub_96,stub_97,stub_98,stub_99,stub_100,stub_101,stub_102,stub_103,stub_104,stub_105,stub_106,stub_107,stub_108,stub_109,stub_110,stub_111,stub_112,stub_113,stub_114,stub_115,stub_116,stub_117,stub_118,stub_119,stub_120,stub_121,stub_122,stub_123,stub_124,stub_125,stub_126,stub_127,stub_128,stub_129,stub_130,stub_131,stub_132,stub_133,stub_134,stub_135,stub_136,stub_137,stub_138,stub_139,stub_140,stub_141,stub_142,stub_143,stub_144,stub_145,stub_146,stub_147,stub_148,stub_149,stub_150,stub_151,stub_152,stub_153,stub_154,stub_155,stub_156,stub_157,stub_158,stub_159,stub_160,stub_161,stub_162,stub_163,stub_164,stub_165,stub_166,stub_167,stub_168,stub_169,stub_170,stub_171,stub_172,stub_173,stub_174,stub_175,stub_176,stub_177,stub_178,stub_179,stub_180,stub_181,stub_182,stub_183,stub_184,stub_185,stub_186,stub_187,stub_188,stub_189,stub_190,stub_191,stub_192,stub_193,stub_194,stub_195,stub_196,stub_197,stub_198,stub_199,stub_200,stub_201,stub_202,stub_203,stub_204,stub_205,stub_206,stub_207,stub_208,stub_209,stub_210,stub_211,stub_212,stub_213,stub_214,stub_215,stub_216,stub_217,stub_218,stub_219,stub_220,stub_221,stub_222,stub_223,stub_224,stub_225,stub_226,stub_227,stub_228,stub_229,stub_230,stub_231,stub_232,stub_233,stub_234,stub_235,stub_236,stub_237,stub_238,stub_239,stub_240,stub_241,stub_242,stub_243,stub_244,stub_245,stub_246,stub_247,stub_248,stub_249,stub_250,stub_251,stub_252,stub_253,stub_254,stub_255,stub_256,stub_257,stub_258,stub_259,stub_260,stub_261,stub_262,stub_263,stub_264,stub_265,stub_266,stub_267,stub_268,stub_269,stub_270,stub_271,stub_272,stub_273,stub_274,stub_275,stub_276,stub_277,stub_278,stub_279,stub_280,stub_281,stub_282,stub_283,stub_284,stub_285,stub_286,stub_287,stub_288,stub_289,stub_290,stub_291,stub_292,stub_293,stub_294,stub_295,stub_296,stub_297,stub_298,stub_299,stub_300,stub_301,stub_302,stub_303,stub_304,stub_305,stub_306,stub_307,stub_308,stub_309,stub_310,stub_311,stub_312,stub_313,stub_314,stub_315,stub_316,stub_317,stub_318,stub_319,stub_320,stub_321,stub_322,stub_323,stub_324,stub_325,stub_326,stub_327,stub_328,stub_329,stub_330,stub_331,stub_332,stub_333,stub_334,stub_335,stub_336,stub_337,stub_338,stub_339,stub_340,stub_341,stub_342,stub_343,stub_344,stub_345,stub_346,stub_347,stub_348,stub_349,stub_350,stub_351,stub_352,stub_353,stub_354,stub_355,stub_356,stub_357,stub_358,stub_359,stub_360,stub_361,stub_362,stub_363,stub_364,stub_365,stub_366,stub_367,stub_368,stub_369,stub_370,stub_371,stub_372,stub_373,stub_374,stub_375,stub_376,stub_377,stub_378,stub_379,stub_380,stub_381,stub_382,stub_383,stub_384,stub_385,stub_386,stub_387,stub_388,stub_389,stub_390,stub_391,stub_392,stub_393,stub_394,stub_395,stub_396,stub_397,stub_398,stub_399,stub_400,stub_401,stub_402,stub_403,stub_404,stub_405,stub_406,stub_407,stub_408,stub_409,stub_410,stub_411,stub_412,stub_413,stub_414,stub_415,stub_416,stub_417,stub_418,stub_419,stub_420,stub_421,stub_422,stub_423,stub_424,stub_425,stub_426,stub_427,stub_428,stub_429,stub_430,stub_431,stub_432,stub_433,stub_434,stub_435,stub_436,stub_437,stub_438,stub_439,stub_440,stub_441,stub_442,stub_443,stub_444,stub_445,stub_446,stub_447,stub_448,stub_449,stub_450,stub_451,stub_452,stub_453,stub_454,stub_455,stub_456,stub_457,stub_458,stub_459,stub_460,stub_461,stub_462,stub_463,stub_464,stub_465,stub_466,stub_467,stub_468,stub_469,stub_470,stub_471,stub_472,stub_473,stub_474,stub_475,stub_476,stub_477,stub_478,stub_479,stub_480,stub_481,stub_482,stub_483,stub_484,stub_485,stub_486,stub_487,stub_488,stub_489,stub_490,stub_491,stub_492,stub_493,stub_494,stub_495,stub_496,stub_497,stub_498,stub_499,stub_500,stub_501,stub_502,stub_503,stub_504,stub_505,stub_506,stub_507,stub_508,stub_509,stub_510,stub_511,stub_512,stub_513,stub_514,stub_515,stub_516,stub_517,stub_518,stub_519,stub_520,stub_521,stub_522,stub_523,stub_524,stub_525,stub_526,stub_527,stub_528,stub_529,stub_530,stub_531,stub_532,stub_533,stub_534,stub_535,stub_536,stub_537,stub_538,stub_539,stub_540,stub_541,stub_542,stub_543,stub_544,stub_545,stub_546,stub_547,stub_548,stub_549,stub_550,stub_551,stub_552,stub_553,stub_554,stub_555,stub_556,stub_557,stub_558,stub_559,stub_560,stub_561,stub_562,stub_563,stub_564,stub_565,stub_566,stub_567,stub_568,stub_569,stub_570};
void imps_fill_stubs(void){ for(int k=0;k<g_nimps;k++) if(!g_imps[k].fn) g_imps[k].fn=stubs[k]; }
