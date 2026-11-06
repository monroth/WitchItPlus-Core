#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <string>
#include <stdexcept>
#ifdef _MSC_VER
#pragma comment(lib,"bcrypt.lib")
#endif
namespace witchit::native {
inline std::string sha256(const std::string& bytes) {
 if(bytes.size()>4194304)throw std::runtime_error("Native.sha256 input exceeds limit");
 std::array<unsigned char,32> result{};
 // Windows 10+ CNG pseudo handle: no provider/session lifetime or engine calls.
 auto status=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
  reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),result.data(),static_cast<ULONG>(result.size()));
 if(status<0)throw std::runtime_error("Native.sha256 BCryptHash failed: "+std::to_string(status));
 constexpr char hex[]="0123456789abcdef";std::string out(64,'0');
 for(std::size_t i=0;i<result.size();++i){out[i*2]=hex[result[i]>>4];out[i*2+1]=hex[result[i]&15];}
 return out;
}
// Same host-stack boundary in production and the real LuaMod/ABI regression.
template<class Stack> int sha256_stack(Stack& stack) {
 auto bytes=stack.get_string(2,4194304);
 if(!bytes)throw std::runtime_error("Native.sha256 requires a string");
 stack.string(sha256(*bytes));return 1;
}
}
