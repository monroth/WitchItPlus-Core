#pragma once
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib,"User32.lib")
#endif
#include <optional>
#include <stdexcept>
#include <string>
namespace witchit::clipboard {
inline std::optional<std::string> execute_for(HWND owner,const std::string& operation,const std::optional<std::string>& bytes) {
    constexpr size_t limit=65536;
    if(operation!="read" && operation!="write")throw std::runtime_error("Native.clipboard: unknown operation");
    // https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-openclipboard
    // A non-null owner is required by EmptyClipboard / SetClipboardData.
    if(!owner || !OpenClipboard(owner))throw std::runtime_error("Native.clipboard: clipboard busy; retry");
    struct Close {~Close(){CloseClipboard();}} close;
    if(operation=="write") {
        if(!bytes || bytes->empty() || bytes->size()>limit || bytes->find('\0')!=std::string::npos)throw std::runtime_error("Native.clipboard: invalid text size");
        int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes->data(),static_cast<int>(bytes->size()),nullptr,0);
        if(!count)throw std::runtime_error("Native.clipboard: invalid UTF-8");
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(count+1)*sizeof(wchar_t));
        if(!memory)throw std::runtime_error("Native.clipboard: allocation failed");
        auto* text=static_cast<wchar_t*>(GlobalLock(memory));
        if(!text){GlobalFree(memory);throw std::runtime_error("Native.clipboard: lock failed");}
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes->data(),static_cast<int>(bytes->size()),text,count);text[count]=0;GlobalUnlock(memory);
        if(!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT,memory)){GlobalFree(memory);throw std::runtime_error("Native.clipboard: publish failed");}
        return std::string("copied"); // ownership of memory passed to Windows
    }
    auto memory=GetClipboardData(CF_UNICODETEXT);
    if(!memory)throw std::runtime_error("Native.clipboard: clipboard has no text");
    const auto size=GlobalSize(memory);
    if(!size || size>(limit+1)*sizeof(wchar_t))throw std::runtime_error("Native.clipboard: text exceeds 64 KiB");
    const auto* text=static_cast<const wchar_t*>(GlobalLock(memory));
    if(!text)throw std::runtime_error("Native.clipboard: lock failed");
    size_t count=0;while(count<size/sizeof(wchar_t) && text[count])++count;
    if(count==size/sizeof(wchar_t)){GlobalUnlock(memory);throw std::runtime_error("Native.clipboard: unterminated text");}
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,static_cast<int>(count),nullptr,0,nullptr,nullptr);
    if(!n || n>limit){GlobalUnlock(memory);throw std::runtime_error("Native.clipboard: invalid or oversized text");}
    std::string result(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,static_cast<int>(count),result.data(),n,nullptr,nullptr);GlobalUnlock(memory);return result;
}
inline std::optional<std::string> execute(const std::string& op,const std::optional<std::string>& bytes) {
    HWND owner=GetActiveWindow();if(!owner)owner=GetForegroundWindow();
    return execute_for(owner,op,bytes);
}
}
