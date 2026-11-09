// glslang_os_win.cpp — glslang's OS layer for the PC test build (scripts/test-vk.sh) on Windows.
//
// The NDK's glslang copy carries only OSDependent/Unix (the phone uses that one). These are the seven functions
// glslang/OSDependent/osinclude.h declares, on Win32 thread-local storage and a critical section.
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

#include "glslang/OSDependent/osinclude.h"

namespace glslang {

// Index + 1, so a valid index is never OS_INVALID_TLS_INDEX (null).
OS_TLSIndex OS_AllocTLSIndex() {
    const DWORD i = TlsAlloc();
    return i == TLS_OUT_OF_INDEXES ? OS_INVALID_TLS_INDEX : (OS_TLSIndex)(uintptr_t)(i + 1);
}
bool OS_SetTLSValue(OS_TLSIndex n, void* v) {
    return n != OS_INVALID_TLS_INDEX && TlsSetValue((DWORD)((uintptr_t)n - 1), v) != 0;
}
bool OS_FreeTLSIndex(OS_TLSIndex n) {
    return n != OS_INVALID_TLS_INDEX && TlsFree((DWORD)((uintptr_t)n - 1)) != 0;
}
void* OS_GetTLSValue(OS_TLSIndex n) {
    return n == OS_INVALID_TLS_INDEX ? nullptr : TlsGetValue((DWORD)((uintptr_t)n - 1));
}

static CRITICAL_SECTION g_lock;
void InitGlobalLock() { InitializeCriticalSection(&g_lock); }
void GetGlobalLock() { EnterCriticalSection(&g_lock); }
void ReleaseGlobalLock() { LeaveCriticalSection(&g_lock); }

void OS_DumpMemoryCounters() {}

}  // namespace glslang
#endif
