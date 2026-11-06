#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
namespace witchit::native {
// Directory metadata only. A page advances at most 32 entries; no save object is
// deserialized. Native save/delete invalidates the directory's write stamp.
class PresetIndex {
 struct Entry {std::filesystem::file_time_type stamp;std::filesystem::directory_iterator cursor,end;std::vector<std::string> names;bool complete{};};
 std::map<std::filesystem::path,Entry> entries_;
public:
 struct Page {std::vector<std::string> names;bool complete{};std::string version;};
 Page page(const std::filesystem::path& root,std::size_t offset) {
  if(offset>65535)throw std::runtime_error("preset offset exceeds cap");
  if(!std::filesystem::exists(root))return{{},true,"absent"};
  auto stamp=std::filesystem::last_write_time(root);auto i=entries_.find(root);
  if(i==entries_.end()||i->second.stamp!=stamp){Entry fresh;fresh.stamp=stamp;fresh.cursor=std::filesystem::directory_iterator(root);i=entries_.insert_or_assign(root,std::move(fresh)).first;}
  auto& e=i->second;
  for(unsigned n=0;n<32&&!e.complete;++n){
   if(e.cursor==e.end){e.complete=true;break;}
   auto file=*e.cursor;++e.cursor;
   if(file.is_regular_file()&&!file.is_symlink()&&file.path().extension()==L".sav"){
    auto name=file.path().stem().u8string();e.names.emplace_back(reinterpret_cast<const char*>(name.data()),name.size());
    if(e.names.size()>65535)throw std::runtime_error("preset inventory exceeds cap");
   }
  }
  Page out;out.version=std::to_string(e.stamp.time_since_epoch().count());for(std::size_t n=offset;n<e.names.size()&&out.names.size()<32;++n)out.names.push_back(e.names[n]);
  out.complete=e.complete&&offset+out.names.size()>=e.names.size();return out;
 }
 void clear(){entries_.clear();}
};
}
