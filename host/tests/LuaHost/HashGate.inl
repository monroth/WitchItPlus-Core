#include "../../../../consumer/native/include/NativeHash.hpp"
static int run_hash_gate() {
 try {
  using witchit::native::sha256;
  if(sha256("")!="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" || sha256("abc")!="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" || sha256(std::string(1000000,'a'))!="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")throw std::runtime_error("SHA256 known-vector mismatch");
  if(sha256(std::string(4194304,'x')).size()!=64)throw std::runtime_error("SHA256 boundary input failed");
  bool refused=false;try{sha256(std::string(4194305,'x'));}catch(...){refused=true;}if(!refused)throw std::runtime_error("SHA256 size limit missing");
  std::cerr<<"HASH PASS real Windows CNG SHA256 empty/abc/million-byte vectors and bounded input\n";return 0;
 }catch(const std::exception& e){std::cerr<<"HASH FAIL "<<e.what()<<'\n';return 1;}
}
