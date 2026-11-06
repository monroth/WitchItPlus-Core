#include <CrashDumper.hpp>
#include <CrashDiagnostics.hpp>
#include <MinidumpFallback.hpp>
#include <string>
#include <format>
#include <bit>
#include <UE4SSProgram.hpp>
#include <Unreal/Core/Windows/WindowsHWrapper.hpp>

#include <polyhook2/PE/IatHook.hpp>
#include <dbghelp.h>
#include <Helpers/SysError.hpp>
#include <Helpers/Time.hpp>
#include <String/StringType.hpp>

namespace fs = std::filesystem;

using std::chrono::seconds;
using std::chrono::system_clock;
using std::chrono::time_point_cast;

namespace RC
{
    const int DumpType =
            MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithModuleHeaders | MiniDumpWithAvxXStateContext;

    static bool FullMemoryDump = false;

    LONG WINAPI ExceptionHandler(_EXCEPTION_POINTERS* exception_pointers)
    {
        // Do not consult UE4SSProgram, Output, fmt, filesystem or timezone
        // statics here: an exit-time fault can arrive after their destruction.
        SYSTEMTIME now{};GetLocalTime(&now);
        wchar_t dump_path[2200]{};
        swprintf_s(dump_path,L"%s\\crash_%04u_%02u_%02u_%02u_%02u_%02u_%03u.dmp",
            CrashDiagnostics::directory,now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
        char detail[512]{};
        sprintf_s(detail,"crash: code=0x%08lx address=%p thread=%lu phase=%s\r\n",
            exception_pointers->ExceptionRecord->ExceptionCode,exception_pointers->ExceptionRecord->ExceptionAddress,
            GetCurrentThreadId(),CrashDiagnostics::last_phase.load(std::memory_order_acquire));
        CrashDiagnostics::line(detail);
        const HANDLE file=CreateFileW(dump_path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) {
            sprintf_s(detail,"crash: create dump failed error=%lu\r\n",GetLastError());CrashDiagnostics::line(detail);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(),exception_pointers,FALSE};
        unsigned long first{},last{};
        const bool ok=WriteMinidumpWithFallback([&](bool minimal) {
            const auto flags=minimal ? MiniDumpNormal : DumpType | (FullMemoryDump ? MiniDumpWithFullMemory | MiniDumpIgnoreInaccessibleMemory : 0);
            return MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file,static_cast<MINIDUMP_TYPE>(flags),&info,nullptr,nullptr)!=FALSE;
        },[&] {LARGE_INTEGER zero{};return SetFilePointerEx(file,zero,nullptr,FILE_BEGIN) && SetEndOfFile(file);},
          [] {return GetLastError();},first,last);
        LARGE_INTEGER bytes{};GetFileSizeEx(file,&bytes);FlushFileBuffers(file);CloseHandle(file);
        sprintf_s(detail,"crash: dump ok=%d bytes=%lld first_error=%lu final_error=%lu\r\n",ok,bytes.QuadPart,first,last);
        CrashDiagnostics::line(detail);
        return ok && bytes.QuadPart>0 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookedSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
    {
        return nullptr;
    }

    CrashDumper::CrashDumper()
    {
    }

    CrashDumper::~CrashDumper()
    {
        // A program can be destroyed before crash dumping was enabled (the
        // standalone lifecycle gate, or a failed early program constructor).
        if (m_set_unhandled_exception_filter_hook)
        {
            m_set_unhandled_exception_filter_hook->unHook();
            SetUnhandledExceptionFilter(reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(m_previous_exception_filter));
        }
    }

    void CrashDumper::phase(const char* value) noexcept { CrashDiagnostics::phase(value); }

    void CrashDumper::enable()
    {
        CrashDiagnostics::initialize(UE4SSProgram::get_program().get_working_directory().data());
        SetErrorMode(SEM_FAILCRITICALERRORS);
        m_previous_exception_filter = SetUnhandledExceptionFilter(ExceptionHandler);

        m_set_unhandled_exception_filter_hook = std::make_unique<PLH::IatHook>("kernel32.dll",
                                                                               "SetUnhandledExceptionFilter",
                                                                               std::bit_cast<uint64_t>(&HookedSetUnhandledExceptionFilter),
                                                                               &m_hook_trampoline_set_unhandled_exception_filter_hook,
                                                                               L"");
        m_set_unhandled_exception_filter_hook->hook();
        this->enabled = true;
    }

    void CrashDumper::set_full_memory_dump(bool enabled)
    {
        FullMemoryDump = enabled;
    }

} // namespace RC
