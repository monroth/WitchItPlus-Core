#include "core.hpp"
#include "vendor/miniz.h"
#include <set>
namespace wip {
void extract(const fs::path& zip,const fs::path& dst,const Json& m){
 
#ifdef _WIN32
 FILE* file=_wfopen(zip.c_str(),L"rb");
#else
 FILE* file=fopen(zip.c_str(),"rb");
#endif
 require(file!=nullptr,"ZIP open");struct FileClose{FILE* f;~FileClose(){fclose(f);}}file_close{file};
 mz_zip_archive z{};require(mz_zip_reader_init_cfile(&z,file,fs::file_size(zip),0)!=0,"ZIP directory");
 struct Close{mz_zip_archive* z;~Close(){mz_zip_reader_end(z);}}close{&z};
 require(mz_zip_reader_get_num_files(&z)==m["files"].size(),"ZIP inventory count");std::set<std::string> seen;
 for(mz_uint i=0;i<mz_zip_reader_get_num_files(&z);++i){mz_zip_archive_file_stat s{};require(mz_zip_reader_file_stat(&z,i,&s)!=0,"ZIP central directory");
  std::string p=s.m_filename;require(p.rfind("Win64/",0)==0,"ZIP Win64 root required");p.erase(0,6);safe_path(p);require(seen.insert(lower(p)).second,"ZIP case collision");
  require(!s.m_is_directory&&!s.m_is_encrypted&&s.m_is_supported,"ZIP entry type/encryption");
  unsigned mode=s.m_external_attr>>16;require((mode&0170000)==0||(mode&0170000)==0100000,"ZIP symlink/special entry");
  const Json* f=nullptr;for(auto& row:m["files"])if(row["path"]==p){f=&row;break;}require(f&&s.m_uncomp_size==(*f)["bytes"].get<uint64_t>(),"ZIP file not in manifest or wrong size");
  size_t n=0;void* data=mz_zip_reader_extract_to_heap(&z,i,&n,0);require(data!=nullptr,"ZIP incomplete/CRC failure");
  std::string bytes(static_cast<char*>(data),n);mz_free(data);require(n==s.m_uncomp_size&&hash(bytes)==(*f)["sha256"],"extracted file hash mismatch");atomic_write(dst/p,bytes);
 }
}
}
