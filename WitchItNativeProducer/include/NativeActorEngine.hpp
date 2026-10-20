#pragma once
#include "NativeSpawnTransaction.hpp"
#include "NativeFunctionParameters.hpp"
#include "NativeClassLoader.hpp"
#include "TeleporterDestination.hpp"
#include "TeleportSampleSchedule.hpp"
#include "UIKitHostStack.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/ScriptArrayHelper.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <DynamicOutput/Output.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <vector>
#include <source_location>
#include <string_view>
#include <map>

namespace witchit::actors {
class UE4SSEngine final:public RC::Unreal::FUObjectCreateListener {
 using UObject=RC::Unreal::UObject;using UFunction=RC::Unreal::UFunction;using Property=RC::Unreal::FProperty;
 using Stack=uikit::HostStack;
 const ue4ss_uikit_lua_host_api_v1* host_;bool enabled_{true};std::uint64_t world_epoch_{};ClassLoader classes_;
 struct Allocation {UObject* pointer{};RC::Unreal::FWeakObjectPtr weak;};
 struct DestroyAudit {Allocation actor;RC::StringType name;bool component{};};
 std::vector<DestroyAudit> destroyed_;
 inline static thread_local std::vector<Allocation>* allocating_{};
 inline static thread_local bool* overflow_{};
 static void require(bool v,const char* reason="reflected actor operation invalid",std::source_location at=std::source_location::current()){
  if(!v)throw std::runtime_error(std::string("Native actor engine line ")+std::to_string(at.line())+": "+reason);
 }
 static bool weak(const Allocation& o)noexcept{try{return o.pointer && o.weak.ObjectIndex>=0 && o.weak.ObjectSerialNumber>0 && o.weak.Get()==o.pointer;}catch(...){return false;}}
 static bool usable(const Allocation& o) {
  using namespace RC::Unreal;if(!weak(o))return false;
  return !o.pointer->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed|RF_FinishDestroyed))
   && !o.pointer->HasAnyInternalFlags(EInternalObjectFlags::PendingKill|EInternalObjectFlags::Unreachable);
 }
 static Allocation pin(UObject* p){require(p && UObject::IsReal(p));Allocation o;o.pointer=p;o.weak=p;require(usable(o));return o;}
 static Property* field(RC::Unreal::UStruct* type,const wchar_t* name,int size) {
  using namespace RC::Unreal;auto* p=type->FindProperty(FName(name,FNAME_Find));require(p && p->GetArrayDim()==1 && p->GetSize()>0 && p->IsInContainer(size));return p;
 }
 static UObject* object(UObject* p,const wchar_t* name) {
  using namespace RC::Unreal;auto* f=CastField<FObjectProperty>(field(p->GetClassPrivate(),name,p->GetClassPrivate()->GetStructureSize()));
  require(f && f->GetSize()==sizeof(UObject*));return f->GetObjectPropertyValue(f->ContainerPtrToValuePtr<void>(p));
 }
 // These are typed, size-checked fields in initialized native parameter storage.
 // UE 5.7 has no SetObjectPropertyValue virtual; no property setter vtable is needed.
 static void scalar_value(RC::Unreal::UStruct* type,void* data,int size,const wchar_t* name,double value) {
  using namespace RC::Unreal;auto* p=field(type,name,size);require(std::isfinite(value));
  if(auto* f=CastField<FFloatProperty>(p)){require(f->GetSize()==sizeof(float) && std::isfinite(static_cast<float>(value)));*f->ContainerPtrToValuePtr<float>(data)=static_cast<float>(value);}
  else {auto* d=CastField<FDoubleProperty>(p);require(d && d->GetSize()==sizeof(double));*d->ContainerPtrToValuePtr<double>(data)=value;}
 }
 static void vector_value(RC::Unreal::UStruct* type,void* data,int size,const wchar_t* name,std::array<double,3> value) {
  using namespace RC::Unreal;auto* p=CastField<FStructProperty>(field(type,name,size));require(p && p->GetStruct()->GetPathName()==L"/Script/CoreUObject.Vector");
  auto* nested=p->ContainerPtrToValuePtr<void>(data);auto* structure=p->GetStruct().Get();int n=structure->GetStructureSize();
  scalar_value(structure,nested,n,L"X",value[0]);scalar_value(structure,nested,n,L"Y",value[1]);scalar_value(structure,nested,n,L"Z",value[2]);
 }
 static void enum_value(Property* p,void* data,unsigned value,const wchar_t* expected) {
  using namespace RC::Unreal;require(p && p->GetSize()==1);
  if(auto* b=CastField<FByteProperty>(p)){require(b->GetEnum().Get() && b->GetEnum()->GetPathName()==expected);*b->ContainerPtrToValuePtr<std::uint8_t>(data)=static_cast<std::uint8_t>(value);}
  else {auto* e=CastField<FEnumProperty>(p);require(e && e->GetEnum().Get() && e->GetEnum()->GetPathName()==expected && e->GetUnderlyingProperty()->GetSize()==1);*p->ContainerPtrToValuePtr<std::uint8_t>(data)=static_cast<std::uint8_t>(value);}
 }
 static void transform_value(RC::Unreal::FStructProperty* transform,void* value,Placement position) {
  using namespace RC::Unreal;
  auto* structure=transform->GetStruct().Get();auto* data=value;const int n=structure->GetStructureSize();
  vector_value(structure,data,n,L"Translation",{position.x,position.y,position.z});vector_value(structure,data,n,L"Scale3D",{position.sx,position.sy,position.sz});
  auto* rotation=CastField<FStructProperty>(field(structure,L"Rotation",n));require(rotation && rotation->GetStruct()->GetPathName()==L"/Script/CoreUObject.Quat");
  auto* q=rotation->ContainerPtrToValuePtr<void>(data);auto* qt=rotation->GetStruct().Get();const int qsize=qt->GetStructureSize();const double half=3.14159265358979323846/360;
  const double sp=std::sin(position.pitch*half),cp=std::cos(position.pitch*half),sy=std::sin(position.yaw*half),cy=std::cos(position.yaw*half),sr=std::sin(position.roll*half),cr=std::cos(position.roll*half);
  scalar_value(qt,q,qsize,L"X",cr*sp*sy-sr*cp*cy);scalar_value(qt,q,qsize,L"Y",-cr*sp*cy-sr*cp*sy);
  scalar_value(qt,q,qsize,L"Z",cr*cp*sy-sr*sp*cy);scalar_value(qt,q,qsize,L"W",cr*cp*cy+sr*sp*sy);
 }
 // Native functions use ParmsSize, not the aligned UStruct storage size.
 static UFunction* function(UObject* target,const wchar_t* name,const wchar_t* path) {
  using namespace RC::Unreal;auto* fn=target->GetFunctionByNameInChain(FName(name,FNAME_Find));
  if(!fn || fn->GetPathName()!=path || !fn->HasAnyFunctionFlags(FUNC_Native))throw std::runtime_error("native function unavailable: "+std::string(path,path+std::char_traits<wchar_t>::length(path)));return fn;
 }
 static bool engine_valid(const Allocation& actor) {
  using namespace RC::Unreal;if(!usable(actor))return false;
  auto* library=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__KismetSystemLibrary");require(library,"KismetSystemLibrary unavailable");
  auto* fn=function(library,L"IsValid",L"/Script/Engine.KismetSystemLibrary:IsValid");native::Parameters args(fn);
  auto* input=CastField<FObjectProperty>(field(fn,L"Object",fn->GetParmsSize()));
  auto* output=CastField<FBoolProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));
  require(input && input->GetSize()==sizeof(UObject*) && output && output->HasAnyPropertyFlags(CPF_ReturnParm),"IsValid schema differs");
  *static_cast<UObject**>(args.value(input))=actor.pointer;library->ProcessEvent(fn,args.data());
  return usable(actor) && output->GetPropertyValue(args.value(output));
 }
 static bool destroying(const Allocation& actor) {
  using namespace RC::Unreal;if(!usable(actor))return true;
  auto* fn=function(actor.pointer,L"IsActorBeingDestroyed",L"/Script/Engine.Actor:IsActorBeingDestroyed");native::Parameters args(fn);
  auto* output=CastField<FBoolProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(output && output->HasAnyPropertyFlags(CPF_ReturnParm));
  actor.pointer->ProcessEvent(fn,args.data());return !usable(actor) || output->GetPropertyValue(args.value(output));
 }
 static bool destruction_state(const DestroyAudit& audit,const wchar_t* phase) {
  const bool valid=engine_valid(audit.actor);const bool ending=valid && !audit.component && destroying(audit.actor);
  RC::Output::send(STR("[WIR] Native.destroy: phase={} actor={} engine_valid={} destroying={} weak_valid={}\n"),phase,audit.name,valid,ending,usable(audit.actor));
  return !valid || ending;
 }
#include "NativeTeleporterLanding.inl"
public:
 std::string read_bytes(UObject* actor,const std::wstring& name) {
  using namespace RC::Unreal;pin(actor);
  auto* p=CastField<FArrayProperty>(field(actor->GetClassPrivate(),name.c_str(),actor->GetClassPrivate()->GetStructureSize()));
  require(p && CastField<FByteProperty>(p->GetInner()),"read_bytes requires a byte-array property");
  auto* value=p->ContainerPtrToValuePtr<FScriptArray>(actor);FScriptArrayHelper array(p,value);
  require(array.Num()>=0 && (array.Num()==0 || value->GetData()),"invalid native byte array");
  return array.Num()?std::string(static_cast<const char*>(value->GetData()),array.Num()):std::string{};
 }
 using Object=Allocation;
 static Object track(UObject* p){return pin(p);}
 struct Context {ue4ss_lua_vm_handle vm{};ue4ss_lua_module_handle module{};std::uint64_t epoch{};Allocation world,mode,klass,statics,function;bool local_scene{};};
 UE4SSEngine(const ue4ss_uikit_lua_host_api_v1* host):host_(host) {
  require(uikit::host_values_available(host_));
  RC::Unreal::FUObjectArray::AddUObjectCreateListener(this);
 }
 ~UE4SSEngine(){RC::Unreal::FUObjectArray::RemoveUObjectCreateListener(this);}
 void retire()noexcept{classes_.stop();enabled_=false;destroyed_.clear();teleporters_.clear();teleport_events_.clear();teleport_samples_.clear();teleport_trace_next_=false;}
 void before_world()noexcept{++world_epoch_;destroyed_.clear();teleporters_.clear();teleport_events_.clear();teleport_samples_.clear();teleport_trace_next_=false;}
 Context capture(UObject* klass,UObject* worldContext,bool local_scene=false) {
  using namespace RC::Unreal;require(enabled_ && IsInGameThreadRaw(),"active game-thread entry required");Stack stack{host_};Context c;c.vm=stack.info().vm;c.module=stack.info().module;
  require(worldContext && UObject::IsReal(worldContext),"invalid world context");c.epoch=world_epoch_;
  c.world=pin(reinterpret_cast<UObject*>(worldContext->GetWorld()));
  // Witch It's OpenMatchByOptions adds ?listen even for solo Creative. A
  // NetDriver is transport, not evidence that this authoritative spawn is unsafe.
  c.local_scene=local_scene;c.mode=local_scene?c.world:pin(object(c.world.pointer,L"AuthorityGameMode"));
  require(local_scene || c.mode.pointer->GetClassPrivate()->GetPathName()==L"/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C","authoritative Creative game mode required");
  c.klass=pin(klass ? klass : UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Actor"));
  auto* actorClass=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.Actor");
  require(c.klass.pointer->IsA<UClass>() && actorClass && static_cast<UClass*>(c.klass.pointer)->IsChildOf(actorClass));
  c.statics=pin(UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__GameplayStatics"));
  c.function=pin(c.statics.pointer->GetFunctionByNameInChain(FName(L"BeginDeferredActorSpawnFromClass",FNAME_Find)));
  require(c.function.pointer->GetPathName()==L"/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass");require(current(c));return c;
 }
 bool world_current(const Context& c)const noexcept {
  try{return enabled_ && RC::Unreal::IsInGameThreadRaw() && c.epoch==world_epoch_
   && usable(c.world) && usable(c.mode) && usable(c.klass) && usable(c.statics) && usable(c.function);}catch(...){return false;}
 }
 bool current(const Context& c)const noexcept {
  try {if(!world_current(c) || (!c.local_scene && object(c.world.pointer,L"AuthorityGameMode")!=c.mode.pointer))return false;Stack stack{host_};return uikit::same_handle(stack.info().vm,c.vm) && uikit::same_handle(stack.info().module,c.module);}catch(...){return false;}
 }
 bool alive(const Context& c,const Object& o)const {
  if(!world_current(c) || !usable(o))return false;
  return o.pointer->GetClassPrivate()==c.klass.pointer && reinterpret_cast<UObject*>(o.pointer->GetWorld())==c.world.pointer;
 }
 template<class Retain> void spawn_owned(const Context& c,Placement position,Retain retain) {
  using namespace RC::Unreal;require(current(c));auto* fn=static_cast<UFunction*>(c.function.pointer);
  require(fn->HasAnyFunctionFlags(FUNC_Native) && fn->HasAnyFunctionFlags(FUNC_Static));
  const int size=fn->GetStructureSize();require(size>0 && size<=256);
  alignas(16) std::array<std::uint8_t,256> params{};std::vector<Property*> fields;fields.reserve(8);
  struct Drop {void* params;std::vector<Property*>& fields;~Drop(){for(auto* p:fields)p->DestroyValue_InContainer(params);}} drop{params.data(),fields};
  for(auto* p:TFieldRange<Property>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm)) {
   require(fields.size()<8 && p->GetArrayDim()==1 && p->IsInContainer(size));p->InitializeValue_InContainer(params.data());fields.push_back(p);
  }
  auto* scale=fn->FindProperty(FName(L"TransformScaleMethod",FNAME_Find));
  require(fields.size()==(scale?7:6)); // UE4.27 has no scale-method parameter; reflected UE5 variant is optional
  auto setObject=[&](const wchar_t* name,UObject* value){auto* p=CastField<FObjectProperty>(field(fn,name,size));require(p && p->GetSize()==sizeof(UObject*));*p->ContainerPtrToValuePtr<UObject*>(params.data())=value;};
  setObject(L"WorldContextObject",c.mode.pointer);setObject(L"ActorClass",c.klass.pointer);setObject(L"Owner",nullptr);
  auto* transform=CastField<FStructProperty>(field(fn,L"SpawnTransform",size));require(transform && transform->GetStruct()->GetPathName()==L"/Script/CoreUObject.Transform");
  transform_value(transform,transform->ContainerPtrToValuePtr<void>(params.data()),position);
  enum_value(field(fn,L"CollisionHandlingOverride",size),params.data(),1,L"/Script/Engine.ESpawnActorCollisionHandlingMethod");if(scale)enum_value(field(fn,L"TransformScaleMethod",size),params.data(),2,L"/Script/Engine.ESpawnActorScaleMethod");
  auto* returned=CastField<FObjectProperty>(field(fn,L"ReturnValue",size));require(returned && returned->GetSize()==sizeof(UObject*) && returned->HasAnyPropertyFlags(CPF_ReturnParm));
  auto* finish=c.statics.pointer->GetFunctionByNameInChain(FName(L"FinishSpawningActor",FNAME_Find));
  require(finish && finish->GetPathName()==L"/Script/Engine.GameplayStatics:FinishSpawningActor");
  std::vector<Allocation> births;births.reserve(256);bool overflow=false;auto* previous=allocating_;auto* previousOverflow=overflow_;allocating_=&births;overflow_=&overflow;
  struct Reset {std::vector<Allocation>* previous;bool* previousOverflow;~Reset(){allocating_=previous;overflow_=previousOverflow;}} reset{previous,previousOverflow};
  require(current(c));c.statics.pointer->ProcessEvent(fn,params.data());
  auto* result=*returned->ContainerPtrToValuePtr<UObject*>(params.data());
  // Match the native CREATE notification, not a caller-selected object or an
  // overridden preexisting return. No result-object dereference before weak pin.
  auto i=std::find_if(births.begin(),births.end(),[&](const auto& b){return b.pointer==result;});
  require(i!=births.end());retain(*i);require(!overflow && alive(c,*i));
 }

 void finish_owned(const Context& c,const Object& actor,Placement position) {
  using namespace RC::Unreal;require(current(c) && alive(c,actor));
  auto* fn=c.statics.pointer->GetFunctionByNameInChain(FName(L"FinishSpawningActor",FNAME_Find));
  require(fn && fn->GetPathName()==L"/Script/Engine.GameplayStatics:FinishSpawningActor");
  const int size=fn->GetStructureSize();require(size>0 && size<=256);
  alignas(16) std::array<std::uint8_t,256> params{};std::vector<Property*> fields;fields.reserve(8);
  struct Drop {void* data;std::vector<Property*>& fields;~Drop(){for(auto i=fields.rbegin();i!=fields.rend();++i)(*i)->DestroyValue_InContainer(data);}} drop{params.data(),fields};
  for(auto* p:TFieldRange<Property>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm)) {
   require(fields.size()<8 && p->GetArrayDim()==1 && p->IsInContainer(size));p->InitializeValue_InContainer(params.data());fields.push_back(p);
  }
  auto* object=CastField<FObjectProperty>(field(fn,L"Actor",size));require(object && object->GetSize()==sizeof(UObject*));*object->ContainerPtrToValuePtr<UObject*>(params.data())=actor.pointer;
  auto* transform=CastField<FStructProperty>(field(fn,L"SpawnTransform",size));require(transform && transform->GetStruct()->GetPathName()==L"/Script/CoreUObject.Transform");
  transform_value(transform,transform->ContainerPtrToValuePtr<void>(params.data()),position);
  if(auto* scale=fn->FindProperty(FName(L"TransformScaleMethod",FNAME_Find)))enum_value(scale,params.data(),2,L"/Script/Engine.ESpawnActorScaleMethod");
  c.statics.pointer->ProcessEvent(fn,params.data());require(current(c) && alive(c,actor));
 }
 // Cleanup needs the actor, not a still-authoritative Creative mode. Exit
 // routing may already have retired that mode while its actors remain alive.
 bool destroy(const Object& actor) {
  using namespace RC::Unreal;require(enabled_ && IsInGameThreadRaw(),"active game-thread entry required");if(!usable(actor))return true;
  auto* actorClass=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.Actor");
  require(actorClass,"Actor class unavailable");
  if(!actor.pointer->IsA(actorClass)) {
   auto* cls=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.ActorComponent");
   require(cls && actor.pointer->IsA(cls),"Destroy target must be an Actor or ActorComponent");
   auto* ownerFn=function(actor.pointer,L"GetOwner",L"/Script/Engine.ActorComponent:GetOwner");native::Parameters ownerArgs(ownerFn);
   auto* result=CastField<FObjectProperty>(field(ownerFn,L"ReturnValue",ownerFn->GetParmsSize()));
   require(result && result->GetSize()==sizeof(UObject*) && result->HasAnyPropertyFlags(CPF_ReturnParm),"component GetOwner schema differs");
   actor.pointer->ProcessEvent(ownerFn,ownerArgs.data());
   auto* owner=*static_cast<UObject**>(ownerArgs.value(result));require(owner && UObject::IsReal(owner),"component owner unavailable");
   auto* fn=function(actor.pointer,L"K2_DestroyComponent",L"/Script/Engine.ActorComponent:K2_DestroyComponent");native::Parameters args(fn);
   auto* input=CastField<FObjectProperty>(field(fn,L"Object",fn->GetParmsSize()));
   require(input && input->GetSize()==sizeof(UObject*),"K2_DestroyComponent schema differs");
   *static_cast<UObject**>(args.value(input))=owner;
   DestroyAudit audit{actor,actor.pointer->GetFullName(),true};destroyed_.push_back(audit);
   actor.pointer->ProcessEvent(fn,args.data());
   if(destruction_state(audit,L"immediate"))return true;
   throw std::runtime_error("DestroyComponent left component active: "+std::string(audit.name.begin(),audit.name.end()));
  }
  auto* fn=function(actor.pointer,L"K2_DestroyActor",L"/Script/Engine.Actor:K2_DestroyActor");
  require(fn->GetParmsSize()==0,"K2_DestroyActor has unexpected parameters");
  DestroyAudit audit{actor,actor.pointer->GetFullName()};destroyed_.push_back(audit);
  actor.pointer->ProcessEvent(fn,nullptr);
  if(destruction_state(audit,L"immediate"))return true;
  throw std::runtime_error("DestroyActor left actor active: "+std::string(audit.name.begin(),audit.name.end()));
 }
 bool destroy_owned(const Context& c,const Object& actor) {return world_current(c) && destroy(actor);}
 void audit_destructions() noexcept {
  if(!enabled_ || !RC::Unreal::IsInGameThreadRaw())return;
  auto pending=std::move(destroyed_);destroyed_.clear();
  for(const auto& audit:pending)try{destruction_state(audit,L"next_tick");}
  catch(const std::exception& e){RC::Output::send(STR("[WIR] Native.destroy: next_tick query failed actor={} reason={}\n"),audit.name,std::wstring(e.what(),e.what()+std::char_traits<char>::length(e.what())));}
 }
 static UObject* resident_asset(const std::wstring& path) {
  using namespace RC::Unreal;
  auto* existing=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,path.c_str());
  const auto unfinished=static_cast<EObjectFlags>(RF_NeedInitialization|RF_NeedLoad|RF_NeedPostLoad|RF_NeedPostLoadSubobjects);
  if(existing && !existing->HasAnyFlags(unfinished)){pin(existing);return existing;}
  return nullptr;
 }
 void poll_class_loads() noexcept {
  if(enabled_ && RC::Unreal::IsInGameThreadRaw())classes_.poll([](const auto& path){return resident_asset(path)!=nullptr;});
 }
 UObject* load_asset(const std::wstring& path,bool class_only=false) {
  using namespace RC::Unreal;require(enabled_ && IsInGameThreadRaw(),"class loading requires game thread");
  auto* existing=resident_asset(path);
  if(existing && class_only){require(existing->IsA<UClass>(),"requested class is not UClass");return existing;}
  // A UWorld can be visible before its actor/component exports finish loading.
  // For assets, wait for the real streamable completion, not object discovery.
  if(!class_only && classes_.holds(path)){classes_.request(path,nullptr,false);return classes_.ready(path)?existing:nullptr;}
  auto* library=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__KismetSystemLibrary");require(library,"KismetSystemLibrary unavailable");
  auto* make=function(library,L"MakeSoftClassPath",L"/Script/Engine.KismetSystemLibrary:MakeSoftClassPath");native::Parameters pathArgs(make);
  auto* text=CastField<FStrProperty>(field(make,L"PathString",make->GetParmsSize()));
  auto* softPath=CastField<FStructProperty>(field(make,L"ReturnValue",make->GetParmsSize()));
  require(text && text->GetSize()==sizeof(FString) && softPath && softPath->GetSize()==32 && softPath->GetStruct()->GetPathName()==L"/Script/CoreUObject.SoftClassPath","MakeSoftClassPath schema differs");
  *static_cast<FString*>(pathArgs.value(text))=FString(path.c_str());library->ProcessEvent(make,pathArgs.data());
  classes_.request(path,pathArgs.value(softPath),class_only);return nullptr;
 }
 void release_asset(const std::wstring& path){require(enabled_ && RC::Unreal::IsInGameThreadRaw(),"asset release requires game thread");classes_.release(path);}
 UObject* load_class(const std::wstring& path,UObject*) {return load_asset(path,true);}
#include "NativeSourceGeometry.inl"
 void NotifyUObjectCreated(const RC::Unreal::UObjectBase* object,RC::Unreal::int32 index)override {
  if(!allocating_ || !object || !RC::Unreal::IsInGameThreadRaw())return;
  if(allocating_->size()>=256){if(overflow_)*overflow_=true;return;}
  Allocation value;value.pointer=reinterpret_cast<UObject*>(const_cast<RC::Unreal::UObjectBase*>(object));value.weak.ObjectIndex=index;
  value.weak.ObjectSerialNumber=RC::Unreal::FUObjectArray::AllocateSerialNumber(index);
  try{allocating_->push_back(value);}catch(...){if(overflow_)*overflow_=true;}
 }
 void OnUObjectArrayShutdown()override{classes_.stop();enabled_=false;}
};
}
