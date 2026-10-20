// Real Windows clipboard calls inside the build VM, without a game or engine.
#include "../../../../consumer/native/include/NativeClipboard.hpp"
static int run_clipboard_gate() {
    HWND window=CreateWindowExW(0,L"STATIC",L"WitchIt Clipboard Gate",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    try {
        if(!window)throw std::runtime_error("clipboard test window unavailable");
        using witchit::clipboard::execute_for;
        const std::string text="MRPP1:ASCII-code + UTF8 \xc3\xa9 \xe2\x9c\x93";
        execute_for(window,"write",text);
        if(execute_for(window,"read",std::nullopt)!=text)throw std::runtime_error("clipboard round-trip mismatch");
        for(auto bad:{std::string("\xff"),std::string(65537,'x')}) {
            bool refused=false;try{execute_for(window,"write",bad);}catch(...){refused=true;}
            if(!refused || execute_for(window,"read",std::nullopt)!=text)throw std::runtime_error("clipboard validation lost existing text");
        }
        DestroyWindow(window);std::cerr<<"CLIPBOARD PASS real Win32 UTF8 round-trip and invalid/oversized write preserves prior text\n";return 0;
    }catch(const std::exception& e){if(window)DestroyWindow(window);std::cerr<<"CLIPBOARD FAIL "<<e.what()<<'\n';return 1;}
}
