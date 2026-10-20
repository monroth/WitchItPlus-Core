#pragma once
#include "UIKitHostStack.hpp"
#include "NativeWorldContext.hpp"
#include "ShipComponentGate.hpp"
#include "ShipScalarSchema.hpp"
#include "NativeFunctionParameters.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <DynamicOutput/Output.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <cmath>
#include <set>

namespace ship_parts {
class UE4SSEngine final {
 using UObject=RC::Unreal::UObject;
 using UFunction=RC::Unreal::UFunction;
 using Property=RC::Unreal::FProperty;
 using Source=witchit::uikit::NativeWorldSource;
 using Observation=witchit::uikit::NativeWorldObservation;
 using HostStack=witchit::uikit::HostStack;
 static void require(bool b){if(!b)throw std::runtime_error("ShipParts UE4SS operation refused");}
 static std::wstring wide(std::string_view v) {
  require(!v.empty() && v.size()<=1024);std::wstring out;
  for(unsigned char c:v){require(c>=32 && c<=126);out.push_back(c);}return out;
 }
 static std::string ascii(std::wstring_view v) {
  require(v.size()<=1024);std::string out;for(wchar_t c:v){require(c>=32 && c<=126);out.push_back(char(c));}return out;
 }
 const ue4ss_uikit_lua_host_api_v1* host_{};Source source_{};std::uint64_t epoch_{1};bool enabled_{};
public:
 struct Object {UObject* pointer{};RC::Unreal::FWeakObjectPtr weak;};
 struct Context {
  ue4ss_lua_vm_handle vm{};ue4ss_lua_module_handle module{};
  Observation observation{};std::uint64_t epoch{},document{};
  Object world,level,game_mode,game_mode_type;std::string world_path,layout;
 };
 struct Prepared {
  Object source,type,function,enumeration;
  std::wstring name;std::string function_path;
  alignas(16) std::array<std::uint8_t,256> parameters{};
 };
 UE4SSEngine(const ue4ss_uikit_lua_host_api_v1* host,Source source):host_(host),source_(source) {
  require(witchit::uikit::host_values_available(host_,UE4SS_LUA_HOST_CAP_SCALARS_V1) && source_.backend && source_.capture && source_.current);enabled_=true;
 }
 void retire() noexcept {enabled_=false;++epoch_;}
 bool current(const Context& c) const noexcept {
  try {
   if(!enabled_ || c.epoch!=epoch_ || !c.document || !RC::Unreal::IsInGameThreadRaw()
    || !source_.current(source_.backend,c.observation))return false;
   HostStack stack{host_,UE4SS_LUA_HOST_CAP_SCALARS_V1};
   if(!witchit::uikit::same_handle(stack.info().vm,c.vm) || !witchit::uikit::same_handle(stack.info().module,c.module))return false;
   if(!live(c.world) || !live(c.level) || !live(c.game_mode) || !live(c.game_mode_type))return false;
   return same(type(c.game_mode),c.game_mode_type) && path(c.game_mode_type)==creative_class
    && same(object_field(c.world,"PersistentLevel"),c.level)
    && same(object_field(c.world,"AuthorityGameMode"),c.game_mode)
    && enum_field(c.game_mode,"Role")==3 && path(c.world)==c.world_path;
  }catch(...){return false;}
 }
 static constexpr auto creative_class="/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C";
 Context capture(std::uint64_t document,std::string world_path,std::string layout) {
  require(enabled_ && RC::Unreal::IsInGameThreadRaw() && document && !layout.empty() && layout.size()<=256);
  HostStack stack{host_,UE4SS_LUA_HOST_CAP_SCALARS_V1};Context c;c.vm=stack.info().vm;c.module=stack.info().module;c.epoch=epoch_;c.document=document;
  c.observation=source_.capture(source_.backend);
  require(c.observation.generation && source_.current(source_.backend,c.observation));
  c.world=pin(static_cast<UObject*>(c.observation.world));
  require(c.world.weak.ObjectIndex==c.observation.world_index && c.world.weak.ObjectSerialNumber==c.observation.world_serial);
  c.world_path=std::move(world_path);c.layout=std::move(layout);
  c.level=object_field(c.world,"PersistentLevel");c.game_mode=object_field(c.world,"AuthorityGameMode");
  c.game_mode_type=type(c.game_mode);require(current(c));return c;
 }
 bool same_context(const Context& a,const Context& b) const noexcept {
  return a.epoch==b.epoch && a.document==b.document && a.observation.generation==b.observation.generation
   && a.world_path==b.world_path && a.layout==b.layout && same(a.world,b.world)
   && witchit::uikit::same_handle(a.vm,b.vm) && witchit::uikit::same_handle(a.module,b.module);
 }
 Object pin(UObject* object) const {
  using namespace RC::Unreal;
  require(object && UObject::IsReal(object));Object out;out.pointer=object;out.weak=object;require(live(out));return out;
 }
 bool live(const Object& o) const noexcept {
  using namespace RC::Unreal;
  try {
   if(!o.pointer || o.weak.ObjectIndex<0 || o.weak.ObjectSerialNumber<=0 || o.weak.Get()!=o.pointer)return false;
   return !o.pointer->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed|RF_FinishDestroyed))
    && !o.pointer->HasAnyInternalFlags(EInternalObjectFlags::PendingKill|EInternalObjectFlags::Unreachable|EInternalObjectFlags::PendingConstruction);
  }catch(...){return false;}
 }
 bool same(const Object& a,const Object& b) const noexcept {
  return a.pointer==b.pointer && a.weak.ObjectIndex==b.weak.ObjectIndex && a.weak.ObjectSerialNumber==b.weak.ObjectSerialNumber;
 }
 std::uint64_t incarnation(const Object& o) const {
  require(live(o));return (std::uint64_t(std::uint32_t(o.weak.ObjectSerialNumber))<<32)|std::uint32_t(o.weak.ObjectIndex);
 }
 Object find(std::string_view path_name) const {
  using namespace RC::Unreal;require(RC::Unreal::IsInGameThreadRaw());auto w=wide(path_name);
  return pin(UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,w.c_str()));
 }
 std::string path(const Object& o) const {require(live(o));return ascii(o.pointer->GetPathName());}
 std::string name(const Object& o) const {require(live(o));return ascii(o.pointer->GetName());}
 Object type(const Object& o) const {require(live(o));return pin(o.pointer->GetClassPrivate());}
 Object outer(const Object& o) const {require(live(o));return pin(o.pointer->GetOuterPrivate());}
 bool was_loaded(const Object& o) const {
  using namespace RC::Unreal;require(live(o));return o.pointer->HasAnyFlags(RF_WasLoaded)
   && !o.pointer->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject|RF_ArchetypeObject|RF_Transient));
 }
 Object persistent_level(const Context& c) const {require(current(c));return c.level;}
 bool in_world(const Context& c,const Object& o) const {
  require(current(c) && live(o));auto* world=o.pointer->GetWorld();
  // GetWorld is virtual: no source dereference after a reentrant scope change.
  return current(c) && live(o) && static_cast<void*>(world)==static_cast<void*>(c.world.pointer);
 }
private:
 struct Located {Property* field{};void* container{};int container_size{};};
 Located locate(const Object& object,std::string_view dotted) const {
  using namespace RC::Unreal;
  require(live(object));auto cls=type(object);auto* structure=static_cast<UStruct*>(cls.pointer);
  void* container=object.pointer;int container_size=structure->GetStructureSize();std::size_t start=0;
  while(start<dotted.size()) {
   auto end=dotted.find('.',start);auto segment=dotted.substr(start,end==dotted.npos?end:end-start);
   auto w=wide(segment);auto* field=structure->FindProperty(FName(w.c_str(),FNAME_Find));
   require(field && field->GetArrayDim()==1 && field->GetSize()>0 && field->IsInContainer(container_size));
   if(end==dotted.npos)return {field,container,container_size};
   auto* nested=CastField<FStructProperty>(field);require(nested);
   auto nested_type=pin(nested->GetStruct());container=nested->ContainerPtrToValuePtr<void>(container);
   structure=static_cast<UStruct*>(nested_type.pointer);container_size=structure->GetStructureSize();start=end+1;
  }
  throw std::runtime_error("empty ShipParts reflected field");
 }
 static void bool_bounds(RC::Unreal::FBoolProperty* p) {
  require(p && p->GetSize()==1 && p->GetByteOffset()==0 && p->GetFieldMask()!=0
   && (p->GetFieldMask()==255 || !(p->GetFieldMask()&(p->GetFieldMask()-1)))
   && (p->GetFieldMask()==255?(p->GetByteMask()==1 || p->GetByteMask()==255):p->GetByteMask()==p->GetFieldMask()));
 }
 static RC::Unreal::UEnum* byte_enum(Property* p) {
  using namespace RC::Unreal;
  require(p && p->GetSize()==1);
  if(auto* b=CastField<FByteProperty>(p))return b->GetEnum().Get();
  auto* e=CastField<FEnumProperty>(p);require(e && e->GetUnderlyingProperty() && e->GetUnderlyingProperty()->GetSize()==1);
  return e->GetEnum().Get();
 }
public:
 std::optional<Object> optional_object_field(const Object& o,std::string_view field) const {
  using namespace RC::Unreal;auto l=locate(o,field);auto* p=CastField<FObjectProperty>(l.field);require(p);
  require(p->GetSize()==sizeof(UObject*));(void)pin(p->GetPropertyClass());
  auto* raw=p->GetObjectPropertyValue(p->ContainerPtrToValuePtr<void>(l.container));
  if(!raw)return {};auto result=pin(raw);require(raw->IsA(p->GetPropertyClass()));return result;
 }
 Object object_field(const Object& o,std::string_view field) const {
  auto result=optional_object_field(o,field);require(result.has_value());return *result;
 }
 std::vector<Object> object_array(const Object& o,std::string_view field,unsigned bound) const {
  using namespace RC::Unreal;auto l=locate(o,field);auto* p=CastField<FArrayProperty>(l.field);
  auto* inner=p?CastField<FObjectProperty>(p->GetInner()):nullptr;
  require(inner && inner->GetSize()==sizeof(UObject*) && p->GetSize()==sizeof(FScriptArray));(void)pin(inner->GetPropertyClass());
  auto* array=p->ContainerPtrToValuePtr<FScriptArray>(l.container);
  int count=array->NumUnchecked();require(count>=0 && unsigned(count)<=bound && count<=array->Max() && (!count || array->GetData()));
  FScriptArrayHelper helper(p,array);std::vector<Object> out;
  for(int i=0;i<count;++i){auto object=pin(inner->GetObjectPropertyValue(helper.GetRawPtr(i)));
   require(object.pointer->IsA(inner->GetPropertyClass()));out.push_back(std::move(object));}return out;
 }
 bool bool_field(const Object& o,std::string_view field) const {
  using namespace RC::Unreal;auto l=locate(o,field);auto* p=CastField<FBoolProperty>(l.field);bool_bounds(p);
  return p->GetPropertyValueInContainer(l.container);
 }
 unsigned enum_field(const Object& o,std::string_view field) const {
  using namespace RC::Unreal;auto l=locate(o,field);(void)pin(byte_enum(l.field));
  if(auto* p=CastField<FByteProperty>(l.field))return p->GetPropertyValueInContainer(l.container);
  auto* p=CastField<FEnumProperty>(l.field);return unsigned(p->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(p->ContainerPtrToValuePtr<void>(l.container)));
 }
 std::string name_field(const Object& o,std::string_view field) const {
  using namespace RC::Unreal;auto l=locate(o,field);auto* p=CastField<FNameProperty>(l.field);require(p && p->GetSize()==sizeof(FName));
  return ascii(p->ContainerPtrToValuePtr<FName>(l.container)->ToString());
 }
 std::string guid_field(const Object& o,std::string_view field) const {
  using namespace RC::Unreal;auto l=locate(o,field);auto* p=CastField<FStructProperty>(l.field);require(p);
  auto structure=pin(p->GetStruct());require(path(structure)=="/Script/CoreUObject.Guid" && p->GetSize()==16
   && static_cast<UStruct*>(structure.pointer)->GetStructureSize()==16);
  // FGuid's public ABI is four uint32 words. No actor/BodyInstance offsets are guessed.
  std::array<std::uint32_t,4> words;std::memcpy(words.data(),p->ContainerPtrToValuePtr<void>(l.container),16);
  char out[64];std::snprintf(out,sizeof(out),"{%08X-%04X-%04X-%04X-%04X%08X}",words[0],words[1]>>16,words[1]&65535,words[2]>>16,words[2]&65535,words[3]);return out;
 }
private:
 mutable std::set<std::string> logged_schemas_;
 Prepared prepare(const Object& source,std::wstring name,std::string exact_path,
  const std::vector<std::pair<std::string,bool>>& bools,std::optional<unsigned> collision) const {
  using namespace RC::Unreal;require(live(source));Prepared out;out.source=source;out.type=type(source);out.name=std::move(name);out.function_path=std::move(exact_path);
  auto* fn=source.pointer->GetFunctionByNameInChain(FName(out.name.c_str(),FNAME_Find));out.function=pin(fn);
  require(path(out.function)==out.function_path && fn->HasAnyFunctionFlags(FUNC_Native) && !fn->HasAnyFunctionFlags(FUNC_Static));
  int size=fn->GetParmsSize();std::vector<ScalarSlot> slots;std::vector<Property*> fields;std::vector<std::string> names;
  for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated)) {
   if(!p->HasAnyPropertyFlags(CPF_Parm))continue;
   ScalarSlot slot;slot.name=ascii(p->GetName());slot.offset=p->GetOffset_Internal();slot.width=p->GetSize();slot.array_dim=p->GetArrayDim();
   slot.out=p->HasAnyPropertyFlags(CPF_OutParm);slot.returned=p->HasAnyPropertyFlags(CPF_ReturnParm);slot.reference=p->HasAnyPropertyFlags(CPF_ReferenceParm);
   try {
   if(auto* b=CastField<FBoolProperty>(p)){bool_bounds(b);slot.kind=ScalarKind::boolean;slot.byte_offset=b->GetByteOffset();slot.mask=b->GetFieldMask();}
   else {require(collision.has_value());out.enumeration=pin(byte_enum(p));require(path(out.enumeration)=="/Script/Engine.ECollisionEnabled");slot.kind=ScalarKind::collision_enum;}
   }catch(const std::exception& e){throw std::runtime_error("ShipParts scalar schema: "+out.function_path+" field="+slot.name+": "+e.what());}
   slots.push_back(slot);fields.push_back(p);
  }
  for(const auto& [name,value]:bools){(void)value;names.push_back(name);}if(collision)names.push_back("NewType");scalar_schema(size,slots,names,out.function_path);
  if(logged_schemas_.insert(out.function_path).second) {
   auto description=scalar_description(size,slots,out.function_path);
   RC::Output::send(STR("[WIR] ShipParts scalar schema: {}\n"),std::wstring(description.begin(),description.end()));
  }
  for(std::size_t i=0;i<slots.size();++i) {
   auto* p=fields[i];const auto& slot=slots[i];
   if(slot.kind==ScalarKind::boolean) {
    auto it=std::find_if(bools.begin(),bools.end(),[&](const auto& x){return x.first==slot.name;});require(it!=bools.end());
    CastField<FBoolProperty>(p)->SetPropertyValueInContainer(out.parameters.data(),it->second);
   }else {
    require(collision && slot.name=="NewType" && *collision<=5);
    auto* en=static_cast<UEnum*>(out.enumeration.pointer);auto n=ascii(en->GetNameByValue(*collision).ToString());
    require(n!="None" && !n.ends_with("_MAX"));
    out.parameters[slot.offset]=std::uint8_t(*collision);
   }
  }
  require(prepared_live(out));return out;
 }
public:
 Prepared prepare_bool(const Object& o,Field f,bool value,bool propagate) const {
  require(!propagate && (f==Field::visible || f==Field::hidden));
  return prepare(o,f==Field::visible?L"SetVisibility":L"SetHiddenInGame",
   f==Field::visible?"/Script/Engine.SceneComponent:SetVisibility":"/Script/Engine.SceneComponent:SetHiddenInGame",
   {{f==Field::visible?"bNewVisibility":"NewHidden",value},{"bPropagateToChildren",false}},{});
 }
 Prepared prepare_collision(const Object& o,unsigned value) const {
  require(value<=5);return prepare(o,L"SetCollisionEnabled","/Script/Engine.PrimitiveComponent:SetCollisionEnabled",{},value);
 }
 bool prepared_live(const Prepared& p) const {
  using namespace RC::Unreal;
  if(!live(p.source) || !live(p.type) || !live(p.function) || (p.enumeration.pointer && !live(p.enumeration)))return false;
  return same(type(p.source),p.type) && path(p.function)==p.function_path
   && p.source.pointer->GetFunctionByNameInChain(FName(p.name.c_str(),FNAME_Find))==p.function.pointer;
 }
 void execute(Prepared& p) const {
  require(RC::Unreal::IsInGameThreadRaw() && prepared_live(p));
  p.source.pointer->ProcessEvent(static_cast<UFunction*>(p.function.pointer),p.parameters.data());
 }
 std::vector<Object> actors(const Context& c,std::string_view exact_class,unsigned limit) const {
  using namespace RC::Unreal;require(current(c));auto cls=find(exact_class);
  auto library=find("/Script/Engine.Default__GameplayStatics");
  auto* fn=library.pointer->GetFunctionByNameInChain(FName(L"GetAllActorsOfClass",FNAME_Find));
  require(fn && fn->GetPathName()==L"/Script/Engine.GameplayStatics:GetAllActorsOfClass");
  witchit::native::Parameters parameters(fn);
  auto object_parameter=[&](const wchar_t* name,UObject* value) {
   auto* p=CastField<FObjectProperty>(fn->FindProperty(FName(name,FNAME_Find)));
   require(p && p->HasAnyPropertyFlags(CPF_Parm) && p->GetSize()==sizeof(UObject*));
   *static_cast<UObject**>(parameters.value(p))=value;
  };
  object_parameter(L"WorldContextObject",c.world.pointer);object_parameter(L"ActorClass",cls.pointer);
  auto* output=CastField<FArrayProperty>(fn->FindProperty(FName(L"OutActors",FNAME_Find)));
  auto* inner=output?CastField<FObjectProperty>(output->GetInner()):nullptr;
  require(output && output->HasAnyPropertyFlags(CPF_Parm|CPF_OutParm) && output->GetSize()==sizeof(FScriptArray) && inner && inner->GetSize()==sizeof(UObject*));
  require(current(c) && live(cls) && live(library));library.pointer->ProcessEvent(fn,parameters.data());require(current(c));
  FScriptArrayHelper actors(output,static_cast<FScriptArray*>(parameters.value(output)));
  require(actors.Num()>=0 && unsigned(actors.Num())<=limit);std::vector<Object> out;
  for(int i=0;i<actors.Num();++i) {
   auto* raw=*reinterpret_cast<UObject**>(actors.GetRawPtr(i));auto object=pin(raw);
   if(same(type(object),cls) && was_loaded(object) && in_world(c,object))out.push_back(std::move(object));
  }
  require(current(c));return out;
 }
#include "ShipTraceNative.inl"

};
} // namespace ship_parts
