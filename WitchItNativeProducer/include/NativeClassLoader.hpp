#pragma once
#include "SourceRecipe.hpp"
#include <Unreal/Core/Containers/FString.hpp>
#include <DynamicOutput/Output.hpp>
#include <SigScanner/SinglePassSigScanner.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <UE4SSProgram.hpp>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <stdexcept>

namespace witchit::actors {
// Witch It's 5.7.1 FStreamableManager, also used by LoadAssetClass. The SDK
// doesn't expose this API. Bind its native functions, not the latent wrapper.
// See review-tests/class_load_binding.py for unique signatures and call ABI.
class ClassLoader final {
 struct Handle {void* object{};void* controller{};};
 using Construct=void*(*)(void*);
 using Destruct=void(*)(void*);
 using Release=void(*)(void*);
 using Request=Handle*(*)(void*,Handle*,const void*,void*,int,bool,bool,RC::Unreal::FString*,bool);
 struct API {Construct construct;Destruct destruct;Release release;Request request;void* check;};
 struct Load {Handle handle;bool complete{};std::string error;std::shared_ptr<std::atomic_bool> live{std::make_shared<std::atomic_bool>(true)};};
 alignas(16) std::array<std::byte,0xe8> manager_{};
 std::map<std::wstring,Load> loads_;
 API api_{};bool initialized_{};
 RC::Unreal::Hook::GlobalCallbackId completion_hook_{};
 std::mutex pending_mutex_;
 std::unordered_map<void*,std::function<void()>> pending_;
 std::function<bool(const std::wstring&)> resident_;
 std::function<void(const std::wstring&,const std::string&)> completed_;
 static API resolve() {
  // Extend the former entry-byte checks to unique signatures. Wildcard the
  // vtable RIP displacement; code relocation never selects a fixed address.
  const std::array<const char*,5> names={"FStreamableManager constructor","FStreamableManager destructor","shared handle release","RequestAsyncLoad","FStreamableHandle CompleteLoad"};
  const std::array<const char*,5> patterns={
   "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 48 8D 05 ?? ?? ?? ?? 66 C7 41 08 01 00 48 89 01",
   "48 8B C4 48 89 58 10 48 89 70 18 55 57 41 54 41 56 41 57 48 8D 68 C8 48 81 EC 10 01 00 00 0F 29 70 C8",
   "48 85 C9 74 42 53 48 83 EC 20 48 89 7C 24 30 48 8B D9 BF FF FF FF FF 8B C7 F0 0F C1 41 08 83 F8 01",
   "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 64 24 20 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC E0 00 00 00 41 0F 10 00",
   // CompleteLoad marks a single request complete. CheckCompletedRequests is
   // only the combined-handle path and never observes an ordinary cold load.
   "40 57 48 83 EC 50 80 B9 E3 00 00 00 00 48 8B F9 0F 85 ?? ?? ?? ?? 80 B9 E2 00 00 00 00 0F 85 ?? ?? ?? ?? 48 89 5C 24 60"};
  std::array<void*,5> addresses{};std::array<unsigned,5> matches{};
  RC::SinglePassScanner::SignatureContainerMap scan;
  auto& containers=scan[RC::ScanTarget::MainExe];
  for(std::size_t i=0;i<patterns.size();++i) {
   containers.emplace_back(std::vector<RC::SignatureData>{{patterns[i]}},[&,i](RC::SignatureContainer& hit) {
    // Scalar workers can overlap a VirtualQuery region. Count distinct addresses.
    if(addresses[i]==hit.get_match_address())return false;
    addresses[i]=hit.get_match_address();return ++matches[i]>=2;
   },[](RC::SignatureContainer&){});
  }
  RC::SinglePassScanner::start_scan(scan);
  for(std::size_t i=0;i<matches.size();++i)if(matches[i]!=1)
   throw std::runtime_error(std::string("Native.load_class: ")+(matches[i]?"ambiguous":"missing")+" signature for "+names[i]);
  RC::Output::send(STR("[WIR] Native.load_class: streamable signatures resolved 5/5\n"));
  return {reinterpret_cast<Construct>(addresses[0]),reinterpret_cast<Destruct>(addresses[1]),
   reinterpret_cast<Release>(addresses[2]),reinterpret_cast<Request>(addresses[3]),addresses[4]};
 }
 void initialize() {
  if(initialized_)return;
  static const API resolved=resolve();api_=resolved;
  api_.construct(manager_.data());initialized_=true;
 }
 static std::string narrow(const std::wstring& value){return {value.begin(),value.end()};}
 static void failed(const std::wstring& path,Load& load,const std::string& why) {
  load.complete=true;load.error=why;
  RC::Output::send(STR("[WIR] Native.load_class: failed {} reason={}\n"),path,std::wstring(why.begin(),why.end()));
 }
 // This engine method runs because a streamable request changed. It is not
 // a frame hook. Filter retained handles before touching any load state; only
 // the matching request posts an already-owned, one-shot game-thread event.
 void completion(void* handle) {
  std::function<void()> signal;
  {
   std::lock_guard lock(pending_mutex_);
   auto found=pending_.find(handle);if(found==pending_.end())return;
   const auto* flags=static_cast<const unsigned char*>(handle);
   if(!flags[0xe1] && !flags[0xe3])return;
   signal=std::move(found->second);pending_.erase(found);
  }
  signal();
 }
 void finish(const std::wstring& path) {
  auto found=loads_.find(path);if(found==loads_.end()||found->second.complete)return;
  auto& load=found->second;
  const auto* flags=static_cast<const unsigned char*>(load.handle.object);
  if(flags[0xe3])failed(path,load,"engine canceled streamable request");
  else if(!flags[0xe1])failed(path,load,"completion event arrived without completed request");
  else try {
   if(!resident_ || !resident_(path))failed(path,load,"streamable request completed without the requested postloaded asset");
   else {load.complete=true;RC::Output::send(STR("[WIR] Native.load_class: resident {}\n"),path);}
  }catch(const std::exception& error){failed(path,load,error.what());}
  bool idle;
  {std::lock_guard lock(pending_mutex_);idle=pending_.empty();}
  if(idle && completion_hook_){RC::Unreal::Hook::UnregisterCallback(completion_hook_);completion_hook_=0;}
  if(completed_)completed_(path,load.error);
 }
 void observe_completion(const std::wstring& path,Load& load) {
  auto signal=RC::UE4SSProgram::get_program().make_game_thread_event([this,path,live=load.live]{if(live->load())finish(path);});
  if(!signal)throw std::runtime_error("Native.load_class: game-thread completion unavailable");
  {
   std::lock_guard lock(pending_mutex_);pending_.emplace(load.handle.object,std::move(signal));
  }
  if(!completion_hook_) {
   completion_hook_=RC::Unreal::Hook::RegisterStreamableCheckPostCallback(api_.check,
    [this](auto&,void* handle){completion(handle);},
    {false,true,STR("WitchItNativeProducer"),STR("streamable request changed")});
   if(completion_hook_==RC::Unreal::Hook::ERROR_ID){completion_hook_=0;throw std::runtime_error("Native.load_class: completion subscription refused");}
  }
  // RequestAsyncLoad can complete synchronously for an already-resident asset.
  // Inspect its return once, not on subsequent frames.
  completion(load.handle.object);
 }
public:
 ClassLoader()=default;~ClassLoader(){stop();}
 void completed(std::function<bool(const std::wstring&)> resident,
                std::function<void(const std::wstring&,const std::string&)> notify) {
  resident_=std::move(resident);completed_=std::move(notify);
 }
 bool holds(const std::wstring& path)const{return loads_.contains(path);}
 bool ready(const std::wstring& path)const{auto i=loads_.find(path);return i!=loads_.end() && i->second.complete && i->second.error.empty();}
 void release(const std::wstring& path){auto i=loads_.find(path);if(i==loads_.end())return;i->second.live->store(false);
  {std::lock_guard lock(pending_mutex_);pending_.erase(i->second.handle.object);}
  api_.release(i->second.handle.controller);loads_.erase(i);
  bool idle;{std::lock_guard lock(pending_mutex_);idle=pending_.empty();}
  if(idle && completion_hook_){RC::Unreal::Hook::UnregisterCallback(completion_hook_);completion_hook_=0;}
 }
 ClassLoader(const ClassLoader&)=delete;ClassLoader& operator=(const ClassLoader&)=delete;
 // SoftObjectPath is initialized by the engine's reflected MakeSoftClassPath.
 // The request copies it before returning. No FSoftObjectPtr ABI or Lua state.
 void request(const std::wstring& path,const void* softObjectPath,bool pending_error=true) {
  if(!recipe::leaf_asset(path))throw std::runtime_error("map package requests are forbidden; use offline source recipes");
  auto found=loads_.find(path);
  if(found==loads_.end()) {
   initialize();auto& load=loads_[path];
   alignas(8) std::array<std::byte,16> unboundDelegate{};
   RC::Unreal::FString debugName(path.c_str());
   RC::Output::send(STR("[WIR] Native.load_class: requesting async {} backend=FStreamableManager\n"),path);
   // Retain the returned shared handle here, including after completion. It
   // keeps the class resident until the central producer releases its manager.
   api_.request(manager_.data(),&load.handle,softObjectPath,unboundDelegate.data(),0,false,false,&debugName,false);
   if(!load.handle.object)failed(path,load,"engine returned no streamable handle");
   else try {observe_completion(path,load);}catch(const std::exception& error){
    {std::lock_guard lock(pending_mutex_);pending_.erase(load.handle.object);}
    failed(path,load,error.what());
   }
   found=loads_.find(path);
  }
  if(!found->second.error.empty())throw std::runtime_error("Native.load_class: "+found->second.error+": "+narrow(path));
  if(pending_error)throw std::runtime_error("async class load pending; wait for Native.load_class: resident, then retry command: "+narrow(path));
 }
 void stop() noexcept {
  if(completion_hook_){RC::Unreal::Hook::UnregisterCallback(completion_hook_);completion_hook_=0;}
  {std::lock_guard lock(pending_mutex_);pending_.clear();}
  completed_={};resident_={};
  if(!initialized_)return;
  for(auto& [path,load]:loads_){load.live->store(false);api_.release(load.handle.controller);}
  loads_.clear();api_.destruct(manager_.data());initialized_=false;
 }
};
}
