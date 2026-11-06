#pragma once
#include "NativeActorEngine.hpp"
#include "NativeHash.hpp"
#include "NativeObjectRoots.hpp"
#include "OwnedLocalWorld.hpp"
#include "NativeInputEvents.hpp"
#include "NativePointerEvents.hpp"
#include "NativeReplicationEvents.hpp"
#include "NativeUpdater.hpp"
#include "SkinRpcTuple.hpp"
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
 struct Subscription {Ref fn;std::wstring path;bool active{true},executing{};RC::Unreal::FWeakObjectPtr actor;};
 struct Rollback {actors::UE4SSEngine::Context context;actors::UE4SSEngine::Object actor;};
 inline static std::weak_ptr<ScopedGameplay> active_;
 const ue4ss_uikit_lua_host_api_v1* host_;OwnedLocalWorld& worlds_;actors::UE4SSEngine engine_;
 ue4ss_lua_vm_handle vm_{};ue4ss_lua_module_handle module_{};ue4ss_lua_callback_handle callback_{};
 std::map<std::int64_t,std::shared_ptr<Subscription>> subscriptions_;
 std::map<std::wstring,RC::Unreal::Hook::GlobalCallbackId> hooks_;
 std::vector<RC::Unreal::Hook::GlobalCallbackId> teleport_hooks_;
 RC::Unreal::Hook::GlobalCallbackId overlap_hook_{},replication_construct_{};
 std::list<Rollback> rollbacks_;std::int64_t next_{};bool enabled_{true},audit_queued_{},drained_{};
 std::recursive_mutex mutex_;
 RC::Unreal::FFrame* intercepted_frame_{};UObject* intercepted_object_{};
 ObjectRoots<UObject,RC::Unreal::FWeakObjectPtr> roots_;
 NativeInputEvents input_;NativePointerEvents pointer_;
 static void require(bool yes){if(!yes)throw std::runtime_error("Native primitive refused");}
 bool entry(const Stack& s)const{return enabled_ && uikit::same_handle(vm_,s.info().vm) && uikit::same_handle(module_,s.info().module);}
 static double number(Stack& s,int at){auto v=s.read(at);require(v.kind==UE4SS_LUA_VALUE_INTEGER||v.kind==UE4SS_LUA_VALUE_NUMBER);double n=v.kind==UE4SS_LUA_VALUE_INTEGER?double(v.integer_value):v.number_value;require(std::isfinite(n)&&std::abs(n)<=1e7);return n;}
 void cancel(Stack& s,std::int64_t id){auto i=subscriptions_.find(id);if(i==subscriptions_.end())return;auto sub=i->second;sub->active=false;subscriptions_.erase(i);auto ref=sub->fn;sub->fn={};s.release(ref);if(sub->path==L"input" && std::none_of(subscriptions_.begin(),subscriptions_.end(),[](const auto& item){return item.second->path==L"input";}))input_.stop();if(sub->path==L"pointer" && std::none_of(subscriptions_.begin(),subscriptions_.end(),[](const auto& item){return item.second->path==L"pointer";}))pointer_.stop();}
 void retry_rollback() noexcept {
  std::erase_if(rollbacks_,[&](const auto& r){if(!engine_.world_current(r.context))return true;try{return engine_.destroy_owned(r.context,r.actor);}catch(...){return false;}});
 }
 void queue_destruction_audit() {
  if(audit_queued_ || !enabled_ || !engine_.has_destructions())return;
  audit_queued_=true;std::weak_ptr<ScopedGameplay> weak=shared_from_this();
  RC::UE4SSProgram::get_program().make_game_thread_event([weak]{
   if(auto self=weak.lock()){std::lock_guard lock(self->mutex_);self->audit_queued_=false;
    if(self->enabled_)self->engine_.audit_destructions();}
  })();
 }
 bool intercept(const std::wstring& path,UObject* object,RC::Unreal::FFrame& frame) noexcept {
  std::lock_guard lock(mutex_);
  struct FrameScope {ScopedGameplay& self;RC::Unreal::FFrame* frame;UObject* object;
   ~FrameScope(){self.intercepted_frame_=frame;self.intercepted_object_=object;}
  } frame_scope{*this,intercepted_frame_,intercepted_object_};
  intercepted_frame_=&frame;intercepted_object_=object;
  if(!enabled_)return false;
  try {Stack s(host_);if(!entry(s))return false;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   const auto snapshot=subscriptions_;
   for(const auto& [id,sub]:snapshot)if(sub->active && sub->path==path){
    if(sub->executing)continue;
    struct Executing{Subscription& sub;Executing(Subscription& s):sub(s){sub.executing=true;}~Executing(){sub.executing=false;}} executing{*sub};
    s.push_reference(sub->fn);s.object(object);int argc=1;
    for(auto* p:RC::Unreal::TFieldRange<RC::Unreal::FProperty>(frame.Node())) {
     using namespace RC::Unreal;
     if(!p->HasAnyPropertyFlags(CPF_Parm) || p->HasAnyPropertyFlags(CPF_ReturnParm))continue;
     auto* value=p->ContainerPtrToValuePtr<void>(frame.Locals());
     if(auto* prop=CastField<FBoolProperty>(p))s.boolean(prop->GetPropertyValue(value));
     else if(CastField<FObjectProperty>(p))s.object(*static_cast<UObject**>(value));
     else if(CastField<FIntProperty>(p))s.integer(*static_cast<std::int32_t*>(value));
     else if(CastField<FByteProperty>(p))s.integer(*static_cast<std::uint8_t*>(value));
     else s.nil();
     ++argc;
    }
    bool consume=s.call(argc,1,L"intercept",path)&&s.get_boolean(-1);s.set_top(top);
    if(consume)return true;
   }
  }catch(const std::exception& error){uikit::callback_error(L"intercept",path,error.what());}catch(...){uikit::callback_error(L"intercept",path,"unknown C++ exception");}return false;
 }
 void asset_ready(const std::wstring& path,const std::string& error) noexcept {
  std::lock_guard lock(mutex_);if(!enabled_)return;
  try {
   Stack s(host_);if(!entry(s))return;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   auto* object=error.empty()?engine_.load_asset(path):nullptr;
   const auto snapshot=subscriptions_;
   for(const auto& [id,sub]:snapshot)if(sub->active && sub->path==L"asset:"+path){
    s.push_reference(sub->fn);if(object)s.object(object);else s.nil();
    if(error.empty())s.nil();else s.string(error);
    if(s.call(2,0,L"on_asset",path))cancel(s,id);s.set_top(top);
   }
  }catch(const std::exception& error){uikit::callback_error(L"on_asset",path,error.what());}catch(...){uikit::callback_error(L"on_asset",path,"unknown C++ exception");}
 }
 void replication_applied(UObject* object) noexcept {
  std::lock_guard lock(mutex_);if(!enabled_)return;
  try {Stack s(host_);if(!entry(s))return;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}}restore{s,top};
   auto life=RC::Unreal::FWeakObjectPtr{object};const auto snapshot=subscriptions_;
   for(auto& [id,sub]:snapshot)if(sub->active&&sub->path==L"replication"&&(!object||life.Get()==object)){
    s.push_reference(sub->fn);if(object)s.object(object);else s.nil();s.call(1,0,L"replication",sub->path);s.set_top(top);
   }
  }catch(const std::exception& error){uikit::callback_error(L"replication",L"replication",error.what());}catch(...){uikit::callback_error(L"replication",L"replication","unknown C++ exception");}
 }
 void input_event(unsigned key,unsigned mask,std::int64_t sequence) noexcept {
  std::lock_guard lock(mutex_);if(!enabled_)return;
  try {Stack s(host_);if(!entry(s))return;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   const auto snapshot=subscriptions_;
   for(const auto& [id,sub]:snapshot)if(sub->active && sub->path==L"input"){
    s.push_reference(sub->fn);s.integer(key);s.integer(mask);s.integer(sequence);
    s.call(3,0,L"input",sub->path);s.set_top(top);
   }
  }catch(const std::exception& error){uikit::callback_error(L"input",L"input",error.what());}catch(...){uikit::callback_error(L"input",L"input","unknown C++ exception");}
 }
 void pointer_event() noexcept {
  std::lock_guard lock(mutex_);if(!enabled_)return;
  try {Stack s(host_);if(!entry(s))return;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   const auto snapshot=subscriptions_;
   for(const auto& [id,sub]:snapshot)if(sub->active && sub->path==L"pointer"){
    s.push_reference(sub->fn);s.call(0,0,L"pointer",sub->path);s.set_top(top);
   }
  }catch(const std::exception& error){uikit::callback_error(L"pointer",L"pointer",error.what());}catch(...){uikit::callback_error(L"pointer",L"pointer","unknown C++ exception");}
 }
 void update_completed(std::int64_t id) noexcept {
  std::lock_guard lock(mutex_);if(!enabled_)return;
  auto found=subscriptions_.find(id);if(found==subscriptions_.end() || !found->second->active)return;
  try{Stack s(host_);if(!entry(s))return;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   s.push_reference(found->second->fn);if(s.call(0,0,L"update_completed",L"update"))cancel(s,id);
  }catch(const std::exception& error){uikit::callback_error(L"update_completed",L"update",error.what());}catch(...){uikit::callback_error(L"update_completed",L"update","unknown C++ exception");}
 }
 void overlap_event(UObject* object,RC::Unreal::UFunction* function,void* params) noexcept {
  using namespace RC::Unreal;std::lock_guard lock(mutex_);if(!enabled_ || !params)return;
  // Unrelated actor events never enter Lua or touch the actor's members.
  const auto found=std::find_if(subscriptions_.begin(),subscriptions_.end(),[object](const auto& row){return row.second->active && row.second->path==L"overlap" && row.second->actor.Get()==object;});
  if(function->GetFName()==FName(L"ReceiveActorEndOverlap",FNAME_Find)) {
   if(auto* property=CastField<FObjectProperty>(function->FindProperty(FName(L"OtherActor",FNAME_Find)));property && property->GetSize()==sizeof(UObject*))
    engine_.teleporter_leave(object,*static_cast<UObject**>(property->ContainerPtrToValuePtr<void>(params)));
  }
  if(found==subscriptions_.end())return;
  try {
   auto* property=CastField<FObjectProperty>(function->FindProperty(FName(L"OtherActor",FNAME_Find)));
   require(property && property->GetSize()==sizeof(UObject*));
   auto* other=*static_cast<UObject**>(property->ContainerPtrToValuePtr<void>(params));if(!other)return;
   const bool begin=function->GetFName()==FName(L"ReceiveActorBeginOverlap",FNAME_Find);
   Stack s(host_);if(!entry(s))return;const int top=s.top();
   struct Restore{Stack& s;int top;~Restore(){s.restore(top);}} restore{s,top};
   const auto snapshot=subscriptions_;
   for(const auto& [id,sub]:snapshot)if(sub->active && sub->path==L"overlap" && sub->actor.Get()==object){
    s.push_reference(sub->fn);s.object(other);s.boolean(begin);
    s.call(2,0,L"overlap",function->GetPathName());s.set_top(top);
   }
  }catch(const std::exception& error){uikit::callback_error(L"overlap",function->GetPathName(),error.what());}catch(...){uikit::callback_error(L"overlap",function->GetPathName(),"unknown C++ exception");}
 }
 void install_overlap() {
  using namespace RC::Unreal;if(overlap_hook_)return;
  const auto begin=FName(L"ReceiveActorBeginOverlap",FNAME_Add),end=FName(L"ReceiveActorEndOverlap",FNAME_Add);
  Hook::FScriptNameFilter filter{{static_cast<std::int32_t>(begin.GetComparisonIndex().ToUnstableInt()),static_cast<std::int32_t>(end.GetComparisonIndex().ToUnstableInt())}};
  std::weak_ptr<ScopedGameplay> weak=shared_from_this();
  overlap_hook_=Hook::RegisterProcessEventPostCallback([weak](auto&,UObject* object,UFunction* fn,void* args){if(auto self=weak.lock())self->overlap_event(object,fn,args);},
   Hook::FScriptCallbackOptions{false,true,STR("WitchItNativeProducer"),STR("owned actor overlap events"),filter});
  require(overlap_hook_!=Hook::ERROR_ID);
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
   // RespawnWitch has a bool OUT. This one allowlisted boundary is an
   // observation only: native Blueprint execution still owns Success.
   bool observation=false;
   for(auto* p:TFieldRange<FProperty>(fn))if(p->HasAnyPropertyFlags(CPF_ReturnParm|CPF_OutParm)) {
    if(path!=L"/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectiveGameMode.B_ObjectiveGameMode_C:RespawnWitch"
       || p->GetName()!=L"Success" || !CastField<FBoolProperty>(p) || p->HasAnyPropertyFlags(CPF_ReturnParm))return;
    observation=true;
   }
   if(auto self=weak.lock();self && self->intercept(path,object,frame) && !observation)data.PreventOriginalFunctionCall();
  },{false,false,STR("WitchItNativeProducer"),STR("Native.intercept"),filter});
  require(id!=Hook::ERROR_ID);hooks_.emplace(path,id);
 }
 // Only the executing allowlisted server RPC's parameter block is exposed.
 // No pawn/GI/SaveGame fields, delayed handles, ProcessEvent or asset loads.
 int skin_rpc(Stack& s) {
  using namespace RC::Unreal;
  require(intercepted_frame_ && intercepted_object_==s.object(2));
  auto& frame=*intercepted_frame_;auto* fn=frame.Node();require(fn && frame.Locals());
  const auto path=fn->GetPathName();
  const bool witch=path==L"/Game/Blueprints/Player/Pawns/B_Witch.B_Witch_C:LoadSkinsServer";
  require(witch || path==L"/Game/Blueprints/Player/Pawns/B_Hunter.B_Hunter_C:LoadSkinsServer");
  require(fn->HasAnyFunctionFlags(FUNC_NetServer) && !fn->HasAnyFunctionFlags(FUNC_Native));
  const std::vector<std::wstring> names=witch?std::vector<std::wstring>{L"Head",L"Body",L"Hat",L"Broom",L"Color"}
   :std::vector<std::wstring>{L"HeadSkinClass",L"UpperBodySkinClass",L"LowerBodySkinClass",L"HatSkinClass",L"RangedWeaponSkinClass",L"MeleeWeaponSkinClass",L"SkinColorSkinClass"};
  std::size_t count{};
  for(auto* p:TFieldRange<FProperty>(fn))if(p->HasAnyPropertyFlags(CPF_Parm)){
   require(!p->HasAnyPropertyFlags(CPF_OutParm|CPF_ReturnParm|CPF_ReferenceParm));
   require(count<names.size() && p->GetFName()==FName(names[count].c_str(),FNAME_Find));++count;
  }
  require(count==names.size());
  std::vector<std::pair<FName*,FName*>> targets;
  auto field=[](UStruct* type,const wchar_t* name){return type->FindProperty(FName(name,FNAME_Find));};
  for(const auto& name:names){
   auto* p=CastField<FStructProperty>(fn->FindProperty(FName(name.c_str(),FNAME_Find)));
   require(p && p->HasAnyPropertyFlags(CPF_Parm) && p->GetArrayDim()==1 && p->IsInContainer(fn->GetParmsSize())
    && p->GetStruct() && p->GetStruct()->GetPathName()==L"/Script/CoreUObject.PrimaryAssetId");
   auto* type=p->GetStruct().Get();auto* n=CastField<FNameProperty>(field(type,L"PrimaryAssetName"));
   auto* a=CastField<FStructProperty>(field(type,L"PrimaryAssetType"));
   require(n && a && a->GetStruct() && n->GetSize()==sizeof(FName) && n->GetArrayDim()==1
    && n->IsInContainer(p->GetSize()) && a->IsInContainer(p->GetSize()));
   auto* t=CastField<FNameProperty>(field(a->GetStruct(),L"Name"));require(t && t->GetSize()==sizeof(FName) && t->GetArrayDim()==1 && t->IsInContainer(a->GetSize()));
   auto* value=p->ContainerPtrToValuePtr<void>(frame.Locals());
   targets.emplace_back(t->ContainerPtrToValuePtr<FName>(a->ContainerPtrToValuePtr<void>(value)),n->ContainerPtrToValuePtr<FName>(value));
  }
  if(auto text=s.get_string(3,4096)){
   auto tuple=skins::parse(*text,names.size());std::vector<std::pair<FName,FName>> values;
   // Finish schema/text/name validation and allocation BEFORE writing any slot.
   for(const auto& id:tuple)values.emplace_back(FName(L"Skin",FNAME_Add),FName(std::wstring(id.begin(),id.end()).c_str(),FNAME_Add));
   for(std::size_t i=0;i<values.size();++i){*targets[i].first=values[i].first;*targets[i].second=values[i].second;}
   s.boolean(true);
  }else{
   std::string encoded;
   for(const auto& target:targets){if(!encoded.empty())encoded+='|';
    for(auto value:{target.first,target.second}){if(value==target.second)encoded+=':';auto wide=value->ToString();encoded.append(wide.begin(),wide.end());}
   }
   s.string(encoded);
  }
  return 1;
 }
 // Detached scalar inputs for the executing mod command RPC only. Never
 // expose an arbitrary parameter frame or retain reflected views in Lua.
 int command_inputs(Stack& s) {
  using namespace RC::Unreal;
  require(intercepted_frame_ && intercepted_object_==s.object(2));
  auto& frame=*intercepted_frame_;auto* fn=frame.Node();require(fn && frame.Locals());
  const auto path=fn->GetPathName();const auto name=fn->GetName();
  const std::wstring base=L"/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:";
  const std::wstring objective=L"/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectivePlayerController.B_ObjectivePlayerController_C:";
  std::vector<std::wstring> names;
  if(path==base+L"SetReadyServer")names={L"WantsToSpectate",L"Ready"};
  else if(path==base+L"SelectPlayerTeamServer")names={L"SelectedTeam"};
  else if(path==base+L"SelectHunterSkill" || path==base+L"SelectWitchSkill")names={L"Skill",L"SkillCategory"};
  else if(path==objective+L"SelectObjectiveTeam")names={L"ObjectiveTeam"};
  else require(path==objective+L"SetReadyObjectiveTeamSelection" || path==objective+L"SurrenderServer");
  require(fn->HasAnyFunctionFlags(FUNC_NetServer) && !fn->HasAnyFunctionFlags(FUNC_Native));
  std::size_t count{};std::string encoded="{\"kind\":\""+std::string(name.begin(),name.end())+"\",\"args\":[";
  for(auto* p:TFieldRange<FProperty>(fn))if(p->HasAnyPropertyFlags(CPF_Parm)){
   require(count<names.size() && p->GetFName()==FName(names[count].c_str(),FNAME_Find));
   require(!p->HasAnyPropertyFlags(CPF_OutParm|CPF_ReturnParm|CPF_ReferenceParm) && p->GetArrayDim()==1 && p->IsInContainer(fn->GetParmsSize()));
   if(count){encoded+=',';}
   auto* value=p->ContainerPtrToValuePtr<void>(frame.Locals());
   if(names[count]==L"Skill"){
    auto* property=CastField<FClassProperty>(p);require(property && p->GetSize()==sizeof(UObject*));
    auto* object=*static_cast<UObject**>(value);require(object);auto full=object->GetPathName();
    require(full.starts_with(L"/Game/Blueprints/Skills/") && full.size()<512);
    require(std::all_of(full.begin(),full.end(),[](wchar_t c){return (c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z')||(c>=L'0'&&c<=L'9')||c==L'_'||c==L'/'||c==L'.';}));
    encoded+='"';encoded.append(full.begin(),full.end());encoded+='"';
   }else if(names[count]==L"Ready" || names[count]==L"WantsToSpectate"){
    auto* property=CastField<FBoolProperty>(p);require(property);encoded+=property->GetPropertyValue(value)?"true":"false";
   }else{
    require(p->GetSize()==1 && (CastField<FByteProperty>(p)||CastField<FEnumProperty>(p)));
    encoded+=std::to_string(*static_cast<std::uint8_t*>(value));
   }
   ++count;
  }
  require(count==names.size());encoded+="]}";s.string(encoded);return 1;
 }
 int operate(Stack& s,const std::string& op) {
  require(entry(s));
  if(op=="skin_rpc")return skin_rpc(s);
  if(op=="command_execute") {auto kind=s.get_string(3,64),args=s.get_string(4,4096);require(kind&&args);s.boolean(engine_.command_execute(static_cast<UObject*>(s.object(2)),*kind,*args));return 1;}
  if(op=="command_inputs")return command_inputs(s);
  if(op=="intercept") {
   require(s.is_function(3));auto p=s.get_string(2);require(p && p->size()<1024);
   std::wstring path(p->begin(),p->end());install(path);
   require(next_<INT64_MAX);auto sub=std::make_shared<Subscription>();sub->path=path;sub->fn=s.reference(3);
   try{s.integer(next_+1);subscriptions_.emplace(++next_,sub);}catch(...){s.release(sub->fn);throw;}return 1;
  }
  if(op=="await_update") {
   require(s.is_function(2));require(next_<INT64_MAX);auto sub=std::make_shared<Subscription>();sub->path=L"update";sub->fn=s.reference(2);
   const auto id=++next_;subscriptions_.emplace(id,sub);std::weak_ptr<ScopedGameplay> weak=shared_from_this();
   updater::when_complete(RC::UE4SSProgram::get_program().make_game_thread_event([weak,id]{if(auto self=weak.lock())self->update_completed(id);}));
   s.integer(id);return 1;
  }
  if(op=="overlap") {
   auto* actor=static_cast<UObject*>(s.object(2));require(actor && s.is_function(3));require(next_<INT64_MAX);
   auto sub=std::make_shared<Subscription>();sub->path=L"overlap";sub->actor=RC::Unreal::FWeakObjectPtr{actor};sub->fn=s.reference(3);
   const auto id=++next_;subscriptions_.emplace(id,sub);
   try{install_overlap();}catch(...){cancel(s,id);throw;}
   s.integer(id);return 1;
  }
  if(op=="replication") {
   require(s.is_function(2));require(next_<INT64_MAX);
   auto sub=std::make_shared<Subscription>();sub->path=L"replication";sub->fn=s.reference(2);
   subscriptions_.emplace(++next_,sub);
   if(!replication_construct_) {
    std::weak_ptr<ScopedGameplay> weak=shared_from_this();
    NativeReplicationEvents::start([weak](UObject* object){if(auto self=weak.lock())self->replication_applied(object);});
    replication_construct_=RC::Unreal::Hook::RegisterStaticConstructObjectPostCallback([weak](auto& info,const auto&){
     try{NativeReplicationEvents::watch(info.GetOriginalFunctionCallResult());}
     catch(...){RC::Output::send(STR("[WIR] Native property-batch capture failed; revoking Core\n"));NativeReplicationEvents::fail();
      auto signal=RC::UE4SSProgram::get_program().make_game_thread_event([weak]{if(auto self=weak.lock())self->replication_applied(nullptr);});if(signal)signal();}
    },{false,true,STR("WitchItNativeProducer"),STR("property batch residency")});
    require(replication_construct_!=RC::Unreal::Hook::ERROR_ID);
   }
   s.integer(next_);return 1;
  }
  if(op=="input") {
   require(s.is_function(2));require(next_<INT64_MAX);
   auto sub=std::make_shared<Subscription>();sub->path=L"input";sub->fn=s.reference(2);
   const auto id=++next_;subscriptions_.emplace(id,sub);
   try {std::weak_ptr<ScopedGameplay> weak=shared_from_this();input_.start([weak](unsigned key,unsigned mask,std::int64_t sequence){if(auto self=weak.lock())self->input_event(key,mask,sequence);});}
   catch(...){cancel(s,id);throw;}
   s.integer(id);return 1;
  }
  if(op=="pointer") {
   require(s.is_function(2));require(next_<INT64_MAX);
   auto sub=std::make_shared<Subscription>();sub->path=L"pointer";sub->fn=s.reference(2);
   const auto id=++next_;subscriptions_.emplace(id,sub);
   try {std::weak_ptr<ScopedGameplay> weak=shared_from_this();pointer_.start([weak]{if(auto self=weak.lock())self->pointer_event();});}
   catch(...){cancel(s,id);throw;}
   s.integer(id);return 1;
  }
  if(op=="respawn_probe")return engine_.respawn_probe(s,static_cast<UObject*>(s.object(2)),static_cast<UObject*>(s.object(3)),
   {number(s,4),number(s,5),number(s,6)},{number(s,7),number(s,8),number(s,9)},number(s,10),number(s,11));
  if(op=="actor_bounds"){return engine_.actor_bounds(s,static_cast<UObject*>(s.object(2)));}
  if(op=="input_mark"){s.integer(input_.mark());return 1;}
  if(op=="world_context") {
   auto* pc=static_cast<UObject*>(s.object(2));require(pc);
   if(worlds_.observe_controller(pc))before_world();
   const auto world=worlds_.capture();
   if(!world.generation || world.local_anchor!=pc){s.nil();s.string("current local controller unavailable");return 2;}
   s.object(world.world);s.number(static_cast<RC::Unreal::UWorld*>(world.world)->GetTimeSeconds());return 2;
  }
  if(op=="await_asset") {
   const auto value=s.get_string(2);require(value && !value->empty() && value->size()<1024 && s.is_function(3));
   std::wstring path(value->begin(),value->end());
   require(actors::recipe::leaf_asset(path));
   require(next_<INT64_MAX);auto sub=std::make_shared<Subscription>();sub->path=L"asset:"+path;sub->fn=s.reference(3);
   const auto id=++next_;subscriptions_.emplace(id,sub);
   try {
    auto* object=engine_.load_asset(path);
    if(object)asset_ready(path,{});
   }catch(const std::exception& e){asset_ready(path,e.what());}
   s.integer(id);return 1;
  }
  if(op=="cancel"){auto v=s.read(2);require(v.kind==UE4SS_LUA_VALUE_INTEGER);cancel(s,v.integer_value);s.boolean(true);return 1;}
  if(op=="sha256") return native::sha256_stack(s);
  if(op=="retain"){auto* o=static_cast<UObject*>(s.object(2));require(o);s.integer(roots_.retain(o));return 1;}
  if(op=="unretain"){auto v=s.read(2);require(v.kind==UE4SS_LUA_VALUE_INTEGER);roots_.release(v.integer_value);s.boolean(true);return 1;}
  if(op=="preset_names") {auto offset=s.read(3);require(offset.kind==UE4SS_LUA_VALUE_INTEGER&&offset.integer_value>=0);return engine_.preset_names(s,static_cast<UObject*>(s.object(2)),offset.integer_value);}
  if(op=="objective_pool_capture") {s.integer(engine_.objective_pool_capture(static_cast<UObject*>(s.object(2))));return 1;}
  if(op=="objective_pool_restore") {s.boolean(engine_.objective_pool_restore(static_cast<UObject*>(s.object(2)),static_cast<std::int64_t>(number(s,3))));return 1;}
  if(op=="release_objective_pool") {engine_.release_objective_pool(static_cast<std::int64_t>(number(s,2)));s.boolean(true);return 1;}
  if(op=="objective_pool") {auto body=s.get_string(3,4194304);require(body.has_value());engine_.objective_pool(static_cast<UObject*>(s.object(2)),*body);s.boolean(true);return 1;}
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
  if(op=="teleporter_pair") {
   engine_.teleporter_pair(static_cast<UObject*>(s.object(2)),static_cast<UObject*>(s.object(3)));install_overlap();s.boolean(true);return 1;
  }
  if(op=="teleporter_exit") {
   engine_.teleporter_exit(static_cast<UObject*>(s.object(2)),static_cast<UObject*>(s.object(3)));
   if(teleport_hooks_.empty()) {
    using namespace RC::Unreal;const auto name=FName(L"Teleport",FNAME_Add);
    const Hook::FScriptNameFilter filter{{static_cast<std::int32_t>(name.GetComparisonIndex().ToUnstableInt())}};
    std::weak_ptr<ScopedGameplay> weak=this->shared_from_this();
    auto pre=[weak](auto& data,UObject* o,FFrame& f,void*){if(auto self=weak.lock();self && self->engine_.teleport_event(o,f,false))data.PreventOriginalFunctionCall();};
    auto post=[weak](auto&,UObject* o,FFrame& f,void*){if(auto self=weak.lock())self->engine_.teleport_event(o,f,true);};
    teleport_hooks_.push_back(Hook::RegisterProcessLocalScriptFunctionPreCallback(pre,{false,true,STR("WitchItNativeProducer"),STR("teleporter event before"),filter}));
    teleport_hooks_.push_back(Hook::RegisterProcessLocalScriptFunctionPostCallback(post,{false,true,STR("WitchItNativeProducer"),STR("teleporter event after"),filter}));
    teleport_hooks_.push_back(Hook::RegisterProcessInternalPreCallback(pre,{false,true,STR("WitchItNativeProducer"),STR("internal teleporter event before"),filter}));
    teleport_hooks_.push_back(Hook::RegisterProcessInternalPostCallback(post,{false,true,STR("WitchItNativeProducer"),STR("internal teleporter event after"),filter}));
   }
   s.boolean(true);return 1;
  }
  if(op=="release_source_levels"){engine_.release_source_levels();s.boolean(true);return 1;}
  if(op=="source_actors") {
   require(s.is_function(4));auto n=number(s,3);require(n>=0&&n<=4096&&n==std::floor(n));auto rows=s.reference(4);
   struct Drop{Stack& s;Ref ref;~Drop(){s.release(ref);}} drop{s,rows};
   if(!engine_.source_actors(s,static_cast<UObject*>(s.object(2)),static_cast<unsigned>(n),rows))s.boolean(false);
   return 1;
  }
  if(op=="release_asset"){auto path=s.get_string(2);require(path && path->size()<1024);engine_.release_asset(std::wstring(path->begin(),path->end()));s.boolean(true);return 1;}
  if(op=="load_asset"){auto path=s.get_string(2);require(path && !path->empty() && path->size()<1024);auto* asset=engine_.load_asset(std::wstring(path->begin(),path->end()));if(asset)s.object(asset);else s.nil();return 1;}
  if(op=="load_class"){auto path=s.get_string(2);require(path && !path->empty() && path->size()<1024);s.object(engine_.load_class(std::wstring(path->begin(),path->end()),s.read(3).kind==UE4SS_LUA_VALUE_NIL?nullptr:static_cast<UObject*>(s.object(3))));return 1;}
  if(op=="spawn" || op=="spawn_local") {
   retry_rollback();
   auto* klass=static_cast<UObject*>(s.object(2));require(klass);auto c=engine_.capture(klass,static_cast<UObject*>(s.object(3)),op!="spawn");
   actors::Placement p{number(s,4),number(s,5),number(s,6),number(s,7),number(s,8),number(s,9),number(s,10),number(s,11),number(s,12)};
   Ref init;if(s.is_function(13))init=s.reference(13);else require(s.read(13).kind==UE4SS_LUA_VALUE_NIL);
   struct Drop{Stack& s;Ref ref;~Drop(){try{s.release(ref);}catch(...){}}} drop{s,init};
   const int top=s.top();std::list<Rollback> failed;failed.push_back({c,{}});
   actors::spawn(engine_,c,p,[&](auto actor){if(init){s.push_reference(init);s.object(actor.pointer);bool ok=s.call(1,1,L"spawn",klass->GetPathName())&&s.get_boolean(-1);s.set_top(top);require(ok);}},
    [&](auto actor){s.object(actor.pointer);require(entry(s)&&engine_.current(c)&&engine_.alive(c,actor));uikit::host_require(host_->callback_return(s.info().context,1));},
    [&](auto,auto actor)noexcept{failed.front().actor=actor;rollbacks_.splice(rollbacks_.end(),failed);});
   return -1; // publication succeeded inside the rollback transaction
  }
  throw std::runtime_error("unknown Native primitive");
 }
 static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL dispatch(ue4ss_lua_context_handle context) {
  auto self=active_.lock();if(!self)return UE4SS_LUA_HOST_RETIRED;
  try {std::lock_guard lock(self->mutex_);Stack s(self->host_);require(uikit::same_handle(s.info().context,context));auto op=s.get_string(1);require(op && op->size()<32);const int top=s.top();
   try {int n=self->operate(s,*op);self->queue_destruction_audit();if(n<0)return UE4SS_LUA_HOST_OK;auto status=self->host_->callback_return(context,n);if(status!=UE4SS_LUA_HOST_OK && *op=="intercept")self->cancel(s,self->next_);return status;}
   catch(const std::exception& error){
    // Operational errors are Lua nil,reason, not a generic host refusal (12).
    // Rollback has already run before exposing a failed spawn to its caller.
    self->queue_destruction_audit();
    std::string reason="Native."+*op+": "+error.what();
    if(auto* failure=dynamic_cast<const uikit::HostFailure*>(&error))reason+=" (host status "+std::to_string(failure->status)+")";
    s.set_top(top);s.nil();s.string(reason);return self->host_->callback_return(context,2);
   }
  }catch(const std::exception& error){uikit::callback_error(L"primitive",L"dispatch",error.what());}catch(...){uikit::callback_error(L"primitive",L"dispatch","unknown C++ exception");}return UE4SS_LUA_HOST_INTERNAL_ERROR;
 }
public:
 ScopedGameplay(const ue4ss_uikit_lua_host_api_v1* host,OwnedLocalWorld& worlds):host_(host),worlds_(worlds),engine_(host){
  engine_.asset_completion([this](const auto& path,const auto& error){asset_ready(path,error);});
 }
 bool push(){std::lock_guard lock(mutex_);Stack s(host_);require(enabled_);vm_=s.info().vm;module_=s.info().module;active_=shared_from_this();if(!callback_.id)uikit::host_require(host_->register_callback(dispatch,&callback_));s.callback(callback_,0);return true;}
 void before_world()noexcept{std::lock_guard lock(mutex_);rollbacks_.clear();engine_.before_world();}
 bool drained()const noexcept{return drained_;}
 bool cleanup_complete()const noexcept{return rollbacks_.empty();}
 void drain() noexcept {
  std::map<std::wstring,RC::Unreal::Hook::GlobalCallbackId> hooks;
  std::vector<RC::Unreal::Hook::GlobalCallbackId> teleport;
  RC::Unreal::Hook::GlobalCallbackId overlap{},replication{};
  {std::lock_guard lock(mutex_);hooks.swap(hooks_);teleport.swap(teleport_hooks_);std::swap(overlap,overlap_hook_);std::swap(replication,replication_construct_);}
  // No producer mutex or Lua gate crosses any wait.
  engine_.drain_hooks();
  if(replication){RC::Unreal::Hook::UnregisterCallback(replication);NativeReplicationEvents::drain();}
  for(auto& [path,id]:hooks)RC::Unreal::Hook::UnregisterCallback(id);
  if(overlap)RC::Unreal::Hook::UnregisterCallback(overlap);
  for(auto id:teleport)RC::Unreal::Hook::UnregisterCallback(id);
  drained_=true;
 }
 bool stop()noexcept {
  std::lock_guard lock(mutex_);enabled_=false;input_.stop();pointer_.stop();NativeReplicationEvents::stop();
  for(auto& [id,sub]:subscriptions_)sub->active=false;
  try{Stack s(host_);for(auto& [id,sub]:subscriptions_)s.release(sub->fn);}catch(...){}
  subscriptions_.clear();roots_.clear();
  retry_rollback();if(!rollbacks_.empty())return false;engine_.retire();return true;
 }
};
}
