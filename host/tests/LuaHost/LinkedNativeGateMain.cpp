#include <Windows.h>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
extern "C" __declspec(dllimport) int UE4SS_RunNative4Linked(const wchar_t*,const wchar_t*,const wchar_t*,bool);
int wmain(int argc,wchar_t** argv){
 if(argc!=5)return 2;const bool negative=std::wstring(argv[4])==L"negative";
 std::jthread watchdog{[negative](std::stop_token stop){for(int i=0;i<(negative?50:1200)&&!stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));if(!stop.stop_requested()){std::fputs("NATIVE4 LINKED WATCHDOG held-gate control\n",stderr);std::_Exit(86);}}};
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 return UE4SS_RunNative4Linked(argv[1],argv[2],argv[3],negative);
}
