#include <vector>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <stdexcept>
#include <iostream>
namespace Round7Fixed {
#include "SerialAllocationFixture.inl"
#include "../../deps/first/Unreal/src/SerialNumberAllocation.inl"
}
namespace Round7Baseline {
#include "SerialAllocationFixture.inl"
#include "SerialNumberBaseline.inl"
}
#undef STR
#undef checkSlow
int run_round7_serial_gate() {
 try {
  bool reproduced=false;
  try{Round7Baseline::FUObjectArray::AllocateSerialNumber(0);}catch(const std::runtime_error& e){reproduced=std::string(e.what())=="FORBIDDEN_SOFT_REFERENCE_COPY";}
  if(!reproduced)throw std::runtime_error("serial allocation baseline did not reproduce");
  using namespace Round7Fixed;
  for(const int size:{24,32,40}){
   serial=0;result_size=size;result_offset=8;const int before=converted;
   if(FUObjectArray::AllocateSerialNumber(0)!=71 || converted!=before+1 || copies || initialized!=destroyed)throw std::runtime_error("reflected serial allocation failed");
   FUObjectArray::AllocateSerialNumber(0);if(converted!=before+1)throw std::runtime_error("already allocated serial entered engine");
  }
  serial=0;fail_call=true;bool threw=false;
  try{FUObjectArray::AllocateSerialNumber(0);}catch(const std::runtime_error& e){threw=std::string(e.what())=="engine error";}
  if(!threw || initialized!=destroyed)throw std::runtime_error("return cleanup on exception failed");
  fail_call=false;result_offset=60;const int before=converted;threw=false;
  try{FUObjectArray::AllocateSerialNumber(0);}catch(const std::runtime_error& e){threw=std::string(e.what()).find("invalid reflected parameters")!=std::string::npos;}
  if(!threw || converted!=before)throw std::runtime_error("invalid return bounds reached engine");
  std::cout<<"ROUND7 PASS serial allocation: reflected return lifecycle; no SDK soft-reference copy; baseline reproduced\n";return 0;
 }catch(const std::exception& e){std::cerr<<"ROUND7 FAIL "<<e.what()<<'\n';return 1;}
}
#ifdef ROUND7_SERIAL_STANDALONE
int main(){return run_round7_serial_gate();}
#endif
