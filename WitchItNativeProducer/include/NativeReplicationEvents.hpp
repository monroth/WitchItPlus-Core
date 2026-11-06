#pragma once
#include <Unreal/UObject.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <windows.h>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <Unreal/UObjectGlobals.hpp>
// PostNetReceive is the engine's property-batch boundary, including properties
// without RepNotify. Patch each constructed class's virtual slot before its
// network initialization; never infer property arrival from BeginPlay/movement.
namespace witchit {
class NativeReplicationEvents {
 using Object=RC::Unreal::UObject;using Function=void(*)(Object*);
 struct Slot {void** address;Function original;};
 inline static std::mutex mutex_;
 inline static std::condition_variable drained_;
 inline static std::map<void**,Slot> slots_;
 inline static std::function<void(Object*)> callback_;
 inline static RC::Unreal::UClass* actor_class_{};
 inline static RC::Unreal::UClass* component_class_{};
 // A virtual-call target may already be in an engine register when the slot
 // is restored. Its executable and original records therefore have PROCESS
 // lifetime, independently of the shorter subscriber lifetime. Pin once;
 // never erase a published original or unload its forwarding code on reload.
 inline static bool pinned_{};
 inline static unsigned executing_{};
 inline static unsigned generation_{};
 static void write(void** address,void* value) {
  DWORD old{};if(!VirtualProtect(address,sizeof(void*),PAGE_READWRITE,&old))throw std::runtime_error("replication vtable protection refused");
  InterlockedExchangePointer(reinterpret_cast<void* volatile*>(address),value);
  DWORD ignored{};if(!VirtualProtect(address,sizeof(void*),old,&ignored))throw std::runtime_error("replication vtable protection restore refused");
 }
 static void received(Object* object) {
  Function original{};unsigned generation;
  {
   std::lock_guard lock(mutex_);
   original=slots_.at(*reinterpret_cast<void***>(object)).original;
   generation=generation_;++executing_;
  }
  struct Exit {~Exit(){
   std::lock_guard lock(mutex_);
   if(--executing_==0)drained_.notify_all();
  }} exit;
  RC::Unreal::FWeakObjectPtr life{object};
  original(object);
  std::function<void(Object*)> callback;
  {std::lock_guard lock(mutex_);if(generation==generation_)callback=callback_;}
  // The entered executor pins a copied subscriber through drain. A wrapper
  // selected before restoration may enter later and still calls the original;
  // stopped generations have no feature callback to acquire.
  if(callback && life.Get()==object)callback(object);
 }

public:
 static void start(std::function<void(Object*)> callback){
  std::lock_guard lock(mutex_);
  actor_class_=RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr,nullptr,L"/Script/Engine.Actor");
  component_class_=RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr,nullptr,L"/Script/Engine.ActorComponent");
  if(!actor_class_||!component_class_)throw std::runtime_error("replication classes unavailable");
  auto offset=Object::VTableLayoutMap.find(L"PostNetReceive");
  if(offset==Object::VTableLayoutMap.end()||offset->second%sizeof(void*)||offset->second>4096)throw std::runtime_error("PostNetReceive layout unavailable");
  if(!pinned_){
   HMODULE module{};
   if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
       reinterpret_cast<LPCWSTR>(&received),&module))throw std::runtime_error("replication forwarder lifetime pin refused");
   pinned_=true;
  }
  ++generation_;callback_=std::move(callback);
 }
 static void watch(Object* object) {
  if(!object)return;
  if(!object->IsA(actor_class_)&&!object->IsA(component_class_))return;
  auto offset=Object::VTableLayoutMap.find(L"PostNetReceive");
  if(offset==Object::VTableLayoutMap.end() || (offset->second%sizeof(void*)||offset->second>4096))throw std::runtime_error("PostNetReceive layout unavailable");
  auto table=*reinterpret_cast<void***>(object);std::lock_guard lock(mutex_);
  if(!callback_)return;
  auto address=table+offset->second/sizeof(void*);auto original=reinterpret_cast<Function>(*address);
  auto found=slots_.find(table);
  if(found!=slots_.end()){
   if(original==received)return;
   if(original!=found->second.original)throw std::runtime_error("replication virtual slot changed owner");
  }else{
   if(!original || original==received)throw std::runtime_error("replication virtual slot differs");
   slots_.emplace(table,Slot{address,original});
  }
  write(address,reinterpret_cast<void*>(&received));
 }
 static void fail()noexcept {
  try{std::lock_guard lock(mutex_);callback_={};++generation_;}catch(...){}
 }
 static void stop(){std::lock_guard lock(mutex_);callback_={};++generation_;}
 // Called outside the Lua gate, after detaching the construction callback.
 static void drain() {
  std::unique_lock lock(mutex_);
  callback_={};++generation_;
  for(auto& [table,slot]:slots_)if(*slot.address==reinterpret_cast<void*>(&received))write(slot.address,reinterpret_cast<void*>(slot.original));
  // Completion is updated under this SAME mutex; the final notification cannot
  // fall between the predicate test and condition-variable wait admission.
  drained_.wait(lock,[]{return executing_==0;});
  // slots_ remains reachable by already-selected forwarders after this return.
 }
};
}
