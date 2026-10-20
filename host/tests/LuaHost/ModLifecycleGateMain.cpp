#include <Windows.h>
#include <iostream>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
extern "C" __declspec(dllimport) int UE4SS_RunLuaModLifecycleGate(const wchar_t*, bool);
extern "C" __declspec(dllimport) int UE4SS_RunLuaModDispatchGate(const wchar_t*,int);
#include "UpdateFileGate.inl"
#include "ClipboardGate.inl"
extern int run_round7_serial_gate();
static int report_fault(EXCEPTION_POINTERS* fault) {
    auto* base = GetModuleHandleW(L"UE4SS.dll");
    std::fprintf(stderr, "LIFECYCLE fault UE4SS offset=0x%llx\n",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fault->ExceptionRecord->ExceptionAddress)-reinterpret_cast<uintptr_t>(base)));
    return EXCEPTION_EXECUTE_HANDLER;
}
static int invoke_gate(const wchar_t* root, bool negative) {
    __try { return UE4SS_RunLuaModLifecycleGate(root, negative); }
    __except(report_fault(GetExceptionInformation())) {
        std::fprintf(stderr, "LIFECYCLE GATE SEH: 0x%08lx\n", GetExceptionCode());
        return 3;
    }
}
int wmain(int argc, wchar_t** argv) {
    // A lock regression must fail the build, including when the blocked thread
    // cannot reach the fixture's own bounded callback-count polling loop.
    std::jthread timeout{[](std::stop_token stop) {
        for(int i=0;i<1200 && !stop.stop_requested();++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(!stop.stop_requested()) {
            std::fputs("LIFECYCLE GATE TIMEOUT: linked host did not return within 120 seconds\n",stderr);
            std::_Exit(87);
        }
    }};
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if(run_round7_serial_gate() || run_update_file_gate() || run_clipboard_gate())return 1;
    std::cerr << "LIFECYCLE executable entered\n";
    if(argc != 2 && argc != 3) { std::cerr << "usage: UE4SSLuaLifecycleGate candidate-root [--expect-baseline-failure]\n"; return 2; }
    if(argc==3) {
        const std::wstring option{argv[2]};
        if(option==L"--round6-main") return UE4SS_RunLuaModDispatchGate(argv[1],15);
        if(option==L"--round6-coroutine") return UE4SS_RunLuaModDispatchGate(argv[1],16);
        if(option==L"--round6-bindings") return UE4SS_RunLuaModDispatchGate(argv[1],17);
        if(option==L"--round5-delayed") return UE4SS_RunLuaModDispatchGate(argv[1],14);
        if(option==L"--round5-producer") return UE4SS_RunLuaModDispatchGate(argv[1],12);
        if(option==L"--round5-first-tick") return UE4SS_RunLuaModDispatchGate(argv[1],13);
        if(option==L"--round4-worker") return UE4SS_RunLuaModDispatchGate(argv[1],9);
        if(option==L"--round4-tick-fallback") return UE4SS_RunLuaModDispatchGate(argv[1],10);
        if(option==L"--round4-event-fallback") return UE4SS_RunLuaModDispatchGate(argv[1],11);
        if(option==L"--round3-notify") return UE4SS_RunLuaModDispatchGate(argv[1],4);
        if(option==L"--round3-tick") return UE4SS_RunLuaModDispatchGate(argv[1],5);
        if(option==L"--round3-event") return UE4SS_RunLuaModDispatchGate(argv[1],6);
        if(option==L"--round3-viewport") return UE4SS_RunLuaModDispatchGate(argv[1],7);
        if(option==L"--round3-global") return UE4SS_RunLuaModDispatchGate(argv[1],8);
        if(option==L"--round2-close") return UE4SS_RunLuaModDispatchGate(argv[1],1);
        if(option==L"--round2-script") return UE4SS_RunLuaModDispatchGate(argv[1],2);
        if(option==L"--round2-notify") return UE4SS_RunLuaModDispatchGate(argv[1],3);
    }
    return invoke_gate(argv[1], argc == 3 && std::wstring(argv[2]) == L"--expect-baseline-failure");
}
