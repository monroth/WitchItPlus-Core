#pragma once
#include <algorithm>
// Cancellation uses registration identity, never a dying UFunction's fields or
// a global UObject name lookup. Callable destructors must run before DLL unload.
namespace RC::Unreal::Internal {
template<class Functions,class Function,class Id>
bool RemoveFunctionHookCallback(Functions& functions,Function function,Id id) {
 auto entry=functions.find(function);
 return entry!=functions.end() && entry->second.RemoveCallback(id);
}
// Script IDs are globally unique. No GetFunc/GetFullName on an expired object.
template<class Scripts,class Pair>
void RemoveScriptHookCallbacks(Scripts& scripts,Pair pair) {
 for(auto& [name,data]:scripts) {
  auto& callbacks=data.Callables;
  callbacks.erase(std::remove_if(callbacks.begin(),callbacks.end(),[&](const auto& cb){return cb.PreId==pair.first && cb.PostId==pair.second;}),callbacks.end());
 }
}
template<class Functions,class Ids,class Function,class Pair>
bool RemoveNativeHookCallbacks(Functions& functions,Ids& ids,Function function,Pair pair) {
 bool native=false;
 for(auto id:{pair.first,pair.second}) {
  auto registered=ids.find(id);if(registered==ids.end())continue;
  native=true;
  if(auto entry=functions.find(function);entry!=functions.end())entry->second.RemoveCallback(registered->second);
  ids.erase(registered);
 }
 return native;
}
}
