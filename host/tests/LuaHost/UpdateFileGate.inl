// Real Win32 publication, no game/engine and no writes to a game installation.
#include "../../../../consumer/native/include/NativeUpdateFiles.hpp"
#include <fstream>
static int run_update_file_gate() {
    wchar_t temporary[32768];
    if(!GetTempPathW(32768,temporary))return 1;
    auto root=std::filesystem::path(temporary)/(L"WitchItUpdateGate-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    HANDLE held=INVALID_HANDLE_VALUE;
    try {
        std::filesystem::create_directory(root);
        using witchit::update_files::execute_at;
        auto require=[](bool v){if(!v)throw std::runtime_error("update file gate assertion");};
        require(!execute_at(root,"read_settings",std::nullopt));
        execute_at(root,"settings",std::string("{\"auto_update\":false}"));
        require(execute_at(root,"read_settings",std::nullopt)=="{\"auto_update\":false}");
        auto destination=root/L"witchit-update-request.json";
        execute_at(root,"request",std::string("first"));
        execute_at(root,"request",std::string("second"));
        auto contents=[&](){std::ifstream file(destination,std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});};
        require(contents()=="second");
        held=CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(held!=INVALID_HANDLE_VALUE);
        bool refused=false;
        try{execute_at(root,"request",std::string("must not replace"));}catch(const std::exception& e){refused=std::string(e.what()).find("path=")!=std::string::npos;}
        require(refused && contents()=="second");
        CloseHandle(held);held=INVALID_HANDLE_VALUE;
        auto claimed=root/L"inflight.json";std::filesystem::rename(destination,claimed);
        execute_at(root,"request",std::string("new request"));
        require(contents()=="new request" && std::filesystem::file_size(claimed)==6);
        size_t count=0;for(auto& entry:std::filesystem::directory_iterator(root))++count;
        require(count==3); // settings, claimed request, newly published request; no temp leaks
        bool invalid=false;try{execute_at(root,"../arbitrary",std::string("x"));}catch(...){invalid=true;}
        require(invalid);
        std::filesystem::remove_all(root);
        std::cerr<<"UPDATE FILE PASS Win32 replace/flush, sharing refusal preserves prior bytes, claimed/new request isolation, no temporary leaks\n";
        return 0;
    }catch(const std::exception& e){
        if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
        std::error_code ignored;std::filesystem::remove_all(root,ignored);
        std::cerr<<"UPDATE FILE FAIL "<<e.what()<<'\n';return 1;
    }
}
