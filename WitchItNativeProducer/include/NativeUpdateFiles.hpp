#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace witchit::update_files {
inline std::runtime_error error(const char* operation) {
    return std::runtime_error(std::string("Native.update_file: ")+operation+" (Win32 "+std::to_string(GetLastError())+")");
}
inline std::runtime_error error(const char* operation,const std::filesystem::path& path) {
    DWORD cause=GetLastError();auto u8=path.u8string();SetLastError(cause);
    return error((std::string(operation)+" path="+std::string(u8.begin(),u8.end())).c_str());
}
inline std::filesystem::path directory() {
    wchar_t executable[32768];
    auto n=GetModuleFileNameW(nullptr,executable,32768);
    if(!n || n>=32768)throw error("locate game executable");
    return std::filesystem::path(std::wstring(executable,n)).parent_path()/L"ue4ss";
}
// The Lua caller chooses only an operation, never a directory or filename.
inline std::optional<std::string> execute_at(const std::filesystem::path& root,const std::string& operation,const std::optional<std::string>& bytes) {
    const bool read=operation=="read_settings";
    if(!read && operation!="settings" && operation!="request")
        throw std::runtime_error("Native.update_file: unknown operation");
    const auto destination=root/(operation=="request"?L"witchit-update-request.json":L"witchit-update-settings.json");
    if(read) {
        HANDLE file=CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) {
            if(GetLastError()==ERROR_FILE_NOT_FOUND)return std::nullopt;
            throw error("read settings",destination);
        }
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(file,&size) || size.QuadPart<0 || size.QuadPart>16384) {CloseHandle(file);throw std::runtime_error("Native.update_file: settings exceeds 16 KiB");}
        std::string result(static_cast<size_t>(size.QuadPart),'\0');DWORD count=0;
        auto success=ReadFile(file,result.data(),static_cast<DWORD>(result.size()),&count,nullptr);
        CloseHandle(file);
        if(!success || count!=result.size())throw error("read settings bytes",destination);
        return result;
    }
    if(!bytes || bytes->empty() || bytes->size()>16384)
        throw std::runtime_error("Native.update_file: payload must be 1..16384 bytes");
    static std::atomic<unsigned long long> serial{};
    auto temporary=destination;
    temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial);
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw error("create request temporary file",destination);
    DWORD count=0;
    bool success=WriteFile(file,bytes->data(),static_cast<DWORD>(bytes->size()),&count,nullptr)!=FALSE && count==bytes->size();
    if(success)success=FlushFileBuffers(file)!=FALSE;
    DWORD cause=GetLastError();
    if(!CloseHandle(file) && success){success=false;cause=GetLastError();}
    if(success && !MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {success=false;cause=GetLastError();}
    if(!success){DeleteFileW(temporary.c_str());SetLastError(cause);throw error("publish file (previous file preserved)",destination);}
    auto u8=destination.u8string();return std::string(u8.begin(),u8.end());
}
inline std::optional<std::string> execute(const std::string& operation,const std::optional<std::string>& bytes) {
    return execute_at(directory(),operation,bytes);
}
}
