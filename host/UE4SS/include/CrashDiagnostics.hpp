#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace RC::CrashDiagnostics {
// POD/process-owned storage: available even after Output/program/CRT teardown.
// The OS closes this handle at process exit; no destructor or background work.
inline HANDLE trace=INVALID_HANDLE_VALUE;
inline wchar_t directory[2048]{};
inline std::atomic<const char*> last_phase{"runtime: startup"};
inline void line(const char* text) noexcept {
    if(trace==INVALID_HANDLE_VALUE)return;
    DWORD written{};
    WriteFile(trace,text,static_cast<DWORD>(std::strlen(text)),&written,nullptr);
    FlushFileBuffers(trace);
}
inline void phase(const char* text) noexcept {
    last_phase.store(text,std::memory_order_release);
    line(text);line("\r\n");
}
inline void initialize(const wchar_t* root) noexcept {
    if(wcslen(root)>=std::size(directory))return;
    wcscpy_s(directory,root);
    wchar_t path[2200]{};swprintf_s(path,L"%s\\UE4SS-shutdown.log",directory);
    trace=CreateFileW(path,GENERIC_WRITE|FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    phase("runtime: diagnostics ready");
}
}
