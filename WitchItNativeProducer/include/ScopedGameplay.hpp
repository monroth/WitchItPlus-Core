#pragma once
#include "NativeActorEngine.hpp"
#include "OwnedLocalWorld.hpp"
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/World.hpp>
#include <DynamicOutput/Output.hpp>
#include <map>
#include <list>
#include <memory>
#include <mutex>

namespace witchit {
// Shared subscriptions and actor construction. The host owns entry, reflection,
// Object marshaling and Lua callback stacks; this class owns no second VM.
class ScopedGameplay final:public std::enable_shared_from_this<ScopedGameplay> {
 using Stack=uikit::HostStack;using Ref=uikit::HostReference;
 using UObject=RC::Unreal::UObject;
 struct Subscription {Ref fn;std::wstring path;bool active{true};};
 struct Rollback {actors::UE4SSEngine::Context context;actors::UE4SSEngine::Object actor;};
 inline static std::weak_ptr<ScopedGameplay> active_;
 const ue4ss_uikit_lua_host_api_v1* host_;OwnedLocalWorld& worlds_;actors::UE4SSEngine engine_;
 ue4ss_lua_vm_handle vm_{};ue4ss_lua_module_handle module_{};ue4ss_lua_callback_handle callback_{};
 std::map<std::int64_t,std::shared_ptr<Subscription>> subscriptions_;
 std::map<std::wstring,RC::Unreal::Hook::GlobalCallbackId> hooks_;
 std::vector<RC::Unreal::Hook::GlobalCallbackId> teleport_hooks_;
 std::list<Rollback> rollbacks_;std::int64_t next_{};bool enabled_{true},emitting_{};
 std::recursive_mutex mutex_;
 static void require(bool yes){if(!yes)throw std::runtime_error("Native primitive refused");}
 bool entry(const Stack& s)const{return enabled_ && uikit::same_handle(vm_,s.info().vm) && uikit::same_handle(module_,s.info().module);}
 static double number(Stack& s,int at){auto v=s.read(at);require(v.kind==UE4SS_LUA_VALUE_INTEGER||v.kind==UE4SS_LUA_VALUE_NUMBER);double n=v.kind==UE4SS_LUA_VALUE_INTEGER?double(v.integer_value):v.number_value;require(std::isfinite(n)&&std::abs(n)<=1e7);return n;}
 void cancel(Stack& s,std::int64_t id){auto i=subscriptions_.find(id);if(i==subscriptions_.end())return;auto sub=i->second;sub->active=false;subscriptions_.erase(i);auto ref=sub->fn;sub->fn={};s.release(ref);}
 void retry_rollback() noexcept {
  std::erase_if(rollbacks_,[&](const auto& r){if(!engine_.world_current(r.context))return true;try{return engine_.destroy_owned(r.context,r.actor);}catch(...){return false;}});
 }
 bool intercept(const std::wstring& path,UObject* object) noexcept {
  std::lock_guard lock(mutex_);if(!enabled_)return false;
  try {Stack s(host_);if(!entry(s))return false;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   const auto snapshot=subscriptions_;
   for(const auto& [id,sub]:snapshot)if(sub->active && sub->path==path){
    s.push_reference(sub->fn);s.object(object);bool ok=s.call(1,1);bool consume=ok&&s.get_boolean(-1);s.set_top(top);
    if(!ok){RC::Output::send(STR("[WIR] Native.intercept callback failed\n"));cancel(s,id);}
    if(consume)return true;
   }
  }catch(...){RC::Output::send(STR("[WIR] Native.intercept dispatch failed\n"));}return false;
 }
 void install(const std::wstring& path) {
  using namespace RC::Unreal;if(hooks_.contains(path))return;
  const auto colon=path.rfind(L':');require(colon!=std::wstring::npos && colon+1<path.size());
  FName name(path.substr(colon+1).c_str(),FNAME_Add);
  Hook::FScriptNameFilter filter{{static_cast<std::int32_t>(name.GetComparisonIndex().ToUnstableInt())}};
  std::weak_ptr<ScopedGameplay> weak=this->shared_from_this();
  auto id=Hook::RegisterProcessLocalScriptFunctionPreCallback([weak,path](auto& data,UObject* object,FFrame& frame,void*){
   auto* fn=frame.Node();if(fn->GetPathName()!=path)return;
   if(fn->HasAnyFunctionFlags(FUNC_Native))return;
   for(auto* p:TFieldRange<FProperty>(fn))if(p->HasAnyPropertyFlags(CPF_ReturnParm|CPF_OutParm))return;
   if(auto self=weak.lock();self && self->intercept(path,object))data.PreventOriginalFunctionCall();
  },{false,false,STR("WitchItNativeProducer"),STR("Native.intercept"),filter});
  require(id!=Hook::ERROR_ID);hooks_.emplace(path,id);
 }
 int operate(Stack& s,const std::string& op) {
  require(entry(s));
  if(op=="tick"||op=="intercept") {
   int at=op=="tick"?2:3;require(s.is_function(at));std::wstring path;
   if(op=="intercept"){auto p=s.get_string(2);require(p && p->size()<1024);path.assign(p->begin(),p->end());install(path);}
   require(next_<INT64_MAX);auto sub=std::make_shared<Subscription>();sub->path=path;sub->fn=s.reference(at);
   try{s.integer(next_+1);subscriptions_.emplace(++next_,sub);}catch(...){s.release(sub->fn);throw;}return 1;
  }
  if(op=="cancel"){auto v=s.read(2);require(v.kind==UE4SS_LUA_VALUE_INTEGER);cancel(s,v.integer_value);s.boolean(true);return 1;}
  if(op=="read_bytes") {
   auto name=s.get_string(3);require(name && !name->empty() && name->size()<128);
   s.string(engine_.read_bytes(static_cast<UObject*>(s.object(2)),std::wstring(name->begin(),name->end())));return 1;
  }
  if(op=="destroy") {
   auto* actor=static_cast<UObject*>(s.object(2));require(actor);auto obj=actors::UE4SSEngine::track(actor);
   s.boolean(engine_.destroy(obj));return 1;
  }
  if(op=="teleport_trace") {
   engine_.arm_teleport_trace();s.boolean(true);return 1;
  }
  if(op=="teleporter_exit") {
   engine_.teleporter_exit(static_cast<UObject*>(s.object(2)),static_cast<UObject*>(s.object(3)));
   if(teleport_hooks_.empty()) {
    using namespace RC::Unreal;const auto name=FName(L"Teleport",FNAME_Add);
    const Hook::FScriptNameFilter filter{{static_cast<std::int32_t>(name.GetComparisonIndex().ToUnstableInt())}};
    std::weak_ptr<ScopedGameplay> weak=this->shared_from_this();
    auto pre=[weak](auto&,UObject* o,FFrame& f,void*){if(auto self=weak.lock())self->engine_.teleport_event(o,f,false);};
    auto post=[weak](auto&,UObject* o,FFrame& f,void*){if(auto self=weak.lock())self->engine_.teleport_event(o,f,true);};
    teleport_hooks_.push_back(Hook::RegisterProcessLocalScriptFunctionPreCallback(pre,{false,true,STR("WitchItNativeProducer"),STR("teleporter event before"),filter}));
    teleport_hooks_.push_back(Hook::RegisterProcessLocalScriptFunctionPostCallback(post,{false,true,STR("WitchItNativeProducer"),STR("teleporter event after"),filter}));
    teleport_hooks_.push_back(Hook::RegisterProcessInternalPreCallback(pre,{false,true,STR("WitchItNativeProducer"),STR("internal teleporter event before"),filter}));
    teleport_hooks_.push_back(Hook::RegisterProcessInternalPostCallback(post,{false,true,STR("WitchItNativeProducer"),STR("internal teleporter event after"),filter}));
   }
   s.boolean(true);return 1;
  }
  if(op=="release_asset"){auto path=s.get_string(2);require(path && path->size()<1024);engine_.release_asset(std::wstring(path->begin(),path->end()));s.boolean(true);return 1;}
  if(op=="load_asset"){auto path=s.get_string(2);require(path && !path->empty() && path->size()<1024);auto* asset=engine_.load_asset(std::wstring(path->begin(),path->end()));if(asset)s.object(asset);else s.nil();return 1;}
  if(op=="load_class"){auto path=s.get_string(2);require(path && !path->empty() && path->size()<1024);s.object(engine_.load_class(std::wstring(path->begin(),path->end()),s.read(3).kind==UE4SS_LUA_VALUE_NIL?nullptr:static_cast<UObject*>(s.object(3))));return 1;}
  if(op=="spawn" || op=="spawn_local" || op=="scene_actor") {
   retry_rollback();
   auto* klass=static_cast<UObject*>(s.object(2));require(klass);auto c=engine_.capture(klass,static_cast<UObject*>(s.object(3)),op!="spawn");
   auto source=op=="scene_actor"?s.get_string(13):std::optional<std::string>{};
   actors::Placement p;
   if(source && s.read(4).kind==UE4SS_LUA_VALUE_NIL)p=engine_.source_placement(std::wstring(source->begin(),source->end()));
   else p={number(s,4),number(s,5),number(s,6),number(s,7),number(s,8),number(s,9),number(s,10),number(s,11),number(s,12)};
   if(op=="scene_actor")require(source && source->size()<2048);
   if(source && engine_.source_present(c.world.pointer,std::wstring(source->begin(),source->end()))){s.boolean(true);return 1;}
   Ref init;if(op!="scene_actor"){if(s.is_function(13))init=s.reference(13);else require(s.read(13).kind==UE4SS_LUA_VALUE_NIL);}
   struct Drop{Stack& s;Ref ref;~Drop(){try{s.release(ref);}catch(...){}}} drop{s,init};
   const int top=s.top();std::list<Rollback> failed;failed.push_back({c,{}});
   actors::spawn(engine_,c,p,[&](auto actor){if(source)engine_.source_geometry(actor,std::wstring(source->begin(),source->end()),p,s.get_string(14).value_or(""));if(init){s.push_reference(init);s.object(actor.pointer);bool ok=s.call(1,1)&&s.get_boolean(-1);s.set_top(top);require(ok);}},
    [&](auto actor){s.object(actor.pointer);require(entry(s)&&engine_.current(c)&&engine_.alive(c,actor));uikit::host_require(host_->callback_return(s.info().context,1));},
    [&](auto,auto actor)noexcept{failed.front().actor=actor;rollbacks_.splice(rollbacks_.end(),failed);});
   return -1; // publication succeeded inside the rollback transaction
  }
  throw std::runtime_error("unknown Native primitive");
 }
 static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL dispatch(ue4ss_lua_context_handle context) {
  auto self=active_.lock();if(!self)return UE4SS_LUA_HOST_RETIRED;
  try {std::lock_guard lock(self->mutex_);Stack s(self->host_);require(uikit::same_handle(s.info().context,context));auto op=s.get_string(1);require(op && op->size()<32);const int top=s.top();
   try {int n=self->operate(s,*op);if(n<0)return UE4SS_LUA_HOST_OK;auto status=self->host_->callback_return(context,n);if(status!=UE4SS_LUA_HOST_OK && (*op=="tick"||*op=="intercept"))self->cancel(s,self->next_);return status;}
   catch(const std::exception& error){
    // Operational errors are Lua nil,reason, not a generic host refusal (12).
    // Rollback has already run before exposing a failed spawn to its caller.
    std::string reason="Native."+*op+": "+error.what();
    if(auto* failure=dynamic_cast<const uikit::HostFailure*>(&error))reason+=" (host status "+std::to_string(failure->status)+")";
    s.set_top(top);s.nil();s.string(reason);return self->host_->callback_return(context,2);
   }
  }catch(...){RC::Output::send(STR("[WIR] Native primitive host dispatch failed\n"));return UE4SS_LUA_HOST_INTERNAL_ERROR;}
 }
public:
 ScopedGameplay(const ue4ss_uikit_lua_host_api_v1* host,OwnedLocalWorld& worlds):host_(host),worlds_(worlds),engine_(host){}
 bool push(){std::lock_guard lock(mutex_);Stack s(host_);require(enabled_);vm_=s.info().vm;module_=s.info().module;active_=shared_from_this();if(!callback_.id)uikit::host_require(host_->register_callback(dispatch,&callback_));s.callback(callback_,0);return true;}
 void tick(float delta) noexcept {
  using namespace RC::Unreal;std::lock_guard lock(mutex_);if(!enabled_||emitting_||!std::isfinite(delta)||delta<0)return;
  engine_.audit_destructions();engine_.poll_class_loads();engine_.sample_teleport_ticks();if(subscriptions_.empty())return;
  emitting_=true;struct Reset{bool& flag;~Reset(){flag=false;}} reset{emitting_};
  try {Stack s(host_);if(!entry(s))return;const int top=s.top();struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   const auto snapshot=subscriptions_;
   for(const auto& [id,sub]:snapshot)if(sub->active && sub->path.empty()){
    s.push_reference(sub->fn);s.number(delta);auto world=worlds_.capture();
    if(world.generation && worlds_.current(world)){
     const double time=static_cast<UWorld*>(world.world)->GetTimeSeconds();require(std::isfinite(time));
     s.number(time);s.object(world.local_anchor);s.object(world.world);
    }else{s.nil();s.nil();s.nil();}
    bool ok=s.call(4,1),keep=ok&&s.get_boolean(-1);s.set_top(top);
    if(!ok)RC::Output::send(STR("[WIR] Native.on_tick callback failed\n"));
    if(!keep)cancel(s,id);
   }
  }catch(...){RC::Output::send(STR("[WIR] Native.on_tick dispatch failed\n"));}
 }
 void before_world()noexcept{std::lock_guard lock(mutex_);rollbacks_.clear();engine_.before_world();}
 bool stop()noexcept {
  std::lock_guard lock(mutex_);enabled_=false;
  for(auto& [id,sub]:subscriptions_)sub->active=false;
  try{Stack s(host_);for(auto& [id,sub]:subscriptions_)s.release(sub->fn);}catch(...){}
  subscriptions_.clear();for(auto& [path,id]:hooks_)RC::Unreal::Hook::UnregisterCallback(id);hooks_.clear();
  for(auto id:teleport_hooks_)RC::Unreal::Hook::UnregisterCallback(id);teleport_hooks_.clear();
  retry_rollback();if(!rollbacks_.empty())return false;engine_.retire();return true;
 }
};
}
