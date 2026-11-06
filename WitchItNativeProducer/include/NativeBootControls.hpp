#pragma once
#include "SourceRecipe.hpp"
#include <set>
namespace witchit::boot {
// Read once on Producer construction, before Unreal-init installs any observer.
// Pure bounded JSON parsing; no engine objects, Lua, hooks or rendering calls.
struct Controls {
 std::set<std::string> disabled;
 inline static const std::set<std::string> names={"writer_safety","save_load_startup","viewport","travel","local_world","ship_binding","primitives","file_bridge","schemas","clipboard","update_file"};
 bool enabled(const char* name)const{return !disabled.contains("all")&&!disabled.contains(name);}
 static Controls parse(const std::string& bytes){
  if(bytes.size()>16384)throw std::runtime_error("Producer boot diagnostics exceeds 16 KiB");
  auto root=actors::recipe::Parser(bytes).parse();if(root.kind!=actors::recipe::Value::Object)throw std::runtime_error("Producer boot diagnostic object expected");
  Controls out;if(auto list=root.get("producer_disable"))for(auto& value:list->list()){
   auto name=value.text();if(name!="all"&&!names.contains(name))throw std::runtime_error("unknown Producer boot diagnostic: "+name);out.disabled.insert(name);
  }
  return out;
 }
};
}
