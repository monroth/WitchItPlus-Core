#pragma once
#include <set>
#include <string>
#include <stdexcept>
namespace witchit::debug {
// Process-local diagnostics configured before Core installs any engine services.
// No UE objects, callbacks, RHI or engine resources are created by configuration.
inline std::set<std::string> disabled;
inline bool enabled(const char* name){return !disabled.contains(name);}
inline void configure(const std::string& csv,bool activated=false){
 const std::set<std::string> names={"native_source_hooks","native_object_listener","native_metadata","native_tick","native_viewport","native_boundary_hooks"};
 std::set<std::string> next;std::size_t at=0;
 while(at<csv.size()) {auto end=csv.find(',',at);auto name=csv.substr(at,end==std::string::npos?end:end-at);
  if(!names.contains(name))throw std::runtime_error("unknown native diagnostic switch: "+name);
  next.insert(name);
  if(end==std::string::npos)break;
  at=end+1;
 }
 if(activated && next!=disabled)throw std::runtime_error("native diagnostics must precede primitive activation; restart the game");
 disabled=std::move(next);
}
// Shared by the Producer entry and the real Windows host gate. This operation
// only uses scalars; invalid configuration is a normal, named Lua result, not
// an exception hidden by the LMS callback dispatcher's generic failure message.
template<class Stack> inline int configure_stack(const Stack& stack,bool activated=false) {
 try {
  const auto csv=stack.get_string(1,1024);
  if(!csv)throw std::runtime_error("native diagnostic list required");
  configure(*csv,activated);stack.boolean(true);return 1;
 }catch(const std::exception& e){stack.boolean(false);stack.string(e.what());return 2;}
}

}
