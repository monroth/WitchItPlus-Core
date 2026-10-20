// Private UE4SSEngine implementation. Reflection supplies every field offset;
// the SDK's empty FHitResult C++ stub is never instantiated or cast.
private:
 struct TraceFrame {
  Object source,type,function;std::vector<Object> schemas;
  std::wstring name;std::string exact_path;
  alignas(16) std::array<std::uint8_t,4096> data{};
  int size{};RC::Unreal::UStruct* hit_type{};Object hit_schema;void* hit_storage{};
  TraceFrame()=default;TraceFrame(const TraceFrame&)=delete;TraceFrame& operator=(const TraceFrame&)=delete;
  // FHitResult owns native struct state. Cleanup uses only the independently
  // pinned engine schema and owned memory, never a world actor or stale FProperty.
  ~TraceFrame(){if(hit_storage && hit_schema.weak.Get()==hit_schema.pointer)hit_type->DestroyStruct(hit_storage);}
 };
 struct TraceInputs {Object pc,camera,library;};
 RC::Unreal::FProperty* trace_field(RC::Unreal::UStruct* type,const wchar_t* name,int size) const {
  using namespace RC::Unreal;auto* p=type->FindProperty(FName(name,FNAME_Find));
  require(p && p->GetArrayDim()==1 && p->GetSize()>0 && p->GetOffset_Internal()>=0 && p->IsInContainer(size));return p;
 }
 void trace_frame(TraceFrame& f,const Object& source,const wchar_t* name,const char* path_name) const {
  using namespace RC::Unreal;require(live(source));f.source=source;f.type=type(source);f.name=name;f.exact_path=path_name;
  auto* fn=source.pointer->GetFunctionByNameInChain(FName(name,FNAME_Find));f.function=pin(fn);
  require(path(f.function)==path_name && fn->HasAnyFunctionFlags(FUNC_Native));f.size=fn->GetStructureSize();require(f.size>0 && f.size<=4096);
 }
 bool trace_frame_live(const TraceFrame& f) const {
  using namespace RC::Unreal;if(!live(f.source) || !live(f.type) || !live(f.function))return false;
  for(const auto& p:f.schemas)if(!live(p))return false;
  return same(type(f.source),f.type) && path(f.function)==f.exact_path
   && f.source.pointer->GetFunctionByNameInChain(FName(f.name.c_str(),FNAME_Find))==f.function.pointer;
 }
 std::array<double,3> trace_vector(RC::Unreal::FStructProperty* p,void* container,bool rotation,
  const std::optional<std::array<double,3>>& input={}) const {
  using namespace RC::Unreal;require(p);auto schema=pin(p->GetStruct());
  require(path(schema)==(rotation?"/Script/CoreUObject.Rotator":"/Script/CoreUObject.Vector"));
  auto* structure=static_cast<UStruct*>(schema.pointer);int size=structure->GetStructureSize();require(size==12 || size==24);
  require(p->GetSize()==size);auto* data=p->ContainerPtrToValuePtr<void>(container);std::array<double,3> out;int index=0;std::vector<std::pair<int,int>> ranges;
  for(auto* name:rotation?std::array{L"Pitch",L"Yaw",L"Roll"}:std::array{L"X",L"Y",L"Z"}) {
   auto* value=CastField<FNumericProperty>(trace_field(structure,name,size));require(value && value->IsFloatingPoint() && value->GetSize()==size/3);
   int begin=value->GetOffset_Internal(),end=begin+value->GetSize();for(const auto& r:ranges)require(end<=r.first || begin>=r.second);ranges.emplace_back(begin,end);
   if(input){require(std::isfinite((*input)[index]));value->SetFloatingPointPropertyValue(value->ContainerPtrToValuePtr<void>(data),(*input)[index]);}
   out[index]=value->GetFloatingPointPropertyValue(value->ContainerPtrToValuePtr<void>(data));require(std::isfinite(out[index]));++index;
  }return out;
 }
 TraceInputs trace_inputs(const Context& c) const {
  using namespace RC::Unreal;require(current(c));TraceInputs in;
  in.pc=pin(static_cast<UObject*>(c.observation.local_anchor));
  require(in.pc.weak.ObjectIndex==c.observation.anchor_index && in.pc.weak.ObjectSerialNumber==c.observation.anchor_serial);
  require(path(type(in.pc))=="/Game/Blueprints/Gamemodes/CreativeMode/B_CreativePlayerController.B_CreativePlayerController_C"
   && enum_field(in.pc,"Role")==3 && in_world(c,in.pc));
  auto gs=object_field(c.world,"GameState");require(!bool_field(gs,"IsPlayTestMode"));
  in.camera=object_field(in.pc,"PlayerCameraManager");auto camera_class=find("/Script/Engine.PlayerCameraManager");
  require(in.camera.pointer->IsA(static_cast<UClass*>(camera_class.pointer))
   && same(object_field(in.camera,"PCOwner"),in.pc) && in_world(c,in.camera));
  in.library=find("/Script/Engine.Default__KismetSystemLibrary");auto cls=type(in.library);
  require(path(cls)=="/Script/Engine.KismetSystemLibrary" && in.library.pointer->HasAnyFlags(RF_ClassDefaultObject)
   && static_cast<UClass*>(cls.pointer)->GetClassDefaultObject().Get()==in.library.pointer);return in;
 }
 bool trace_inputs_live(const Context& c,const TraceInputs& in) const {
  return current(c) && live(in.pc) && live(in.camera) && live(in.library)
   && in.pc.pointer==c.observation.local_anchor && same(object_field(in.pc,"PlayerCameraManager"),in.camera)
   && same(object_field(in.camera,"PCOwner"),in.pc);
 }
 void trace_camera_schema(TraceFrame& f,bool rotation) const {
  using namespace RC::Unreal;auto* fn=static_cast<UFunction*>(f.function.pointer);unsigned count=0;
  for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm)) {
   require(++count==1 && p->GetName()==L"ReturnValue" && p->HasAnyPropertyFlags(CPF_ReturnParm) && p->GetArrayDim()==1
    && !p->HasAnyPropertyFlags(CPF_ReferenceParm));auto* s=CastField<FStructProperty>(p);require(s && p->IsInContainer(f.size));
   f.schemas.push_back(pin(s->GetStruct()));(void)trace_vector(s,f.data.data(),rotation);
  }require(count==1);
 }
 void trace_line_schema(TraceFrame& f,const TraceInputs& in) const {
  using namespace RC::Unreal;auto* fn=static_cast<UFunction*>(f.function.pointer);
  const std::set<std::wstring> expected={L"WorldContextObject",L"Start",L"End",L"TraceChannel",L"bTraceComplex",L"ActorsToIgnore",L"DrawDebugType",L"OutHit",L"bIgnoreSelf",L"TraceColor",L"TraceHitColor",L"DrawTime",L"ReturnValue"};
  std::set<std::wstring> seen;std::vector<std::pair<int,int>> ranges;
  for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm)) {
   auto name=p->GetName();require(expected.contains(name) && seen.insert(name).second && p->GetArrayDim()==1 && p->IsInContainer(f.size));
   int begin=p->GetOffset_Internal(),end=begin+p->GetSize();require(begin>=0 && end>begin && end<=f.size);
   for(const auto& r:ranges)require(end<=r.first || begin>=r.second);ranges.emplace_back(begin,end);
   require(p->HasAnyPropertyFlags(CPF_ReturnParm)==(name==L"ReturnValue"));
   require(!p->HasAnyPropertyFlags(CPF_OutParm) || name==L"OutHit" || name==L"ReturnValue");
   require(!p->HasAnyPropertyFlags(CPF_ReferenceParm) || name==L"OutHit" || (name==L"ActorsToIgnore" && p->HasAnyPropertyFlags(CPF_ConstParm)));
   if(name==L"WorldContextObject") {auto* o=CastField<FObjectProperty>(p);require(o && p->GetSize()==sizeof(UObject*));auto cls=pin(o->GetPropertyClass());f.schemas.push_back(cls);require(in.pc.pointer->IsA(static_cast<UClass*>(cls.pointer)));o->SetObjectPropertyValue(o->ContainerPtrToValuePtr<void>(f.data.data()),in.pc.pointer);}
   else if(name==L"Start" || name==L"End") {auto* s=CastField<FStructProperty>(p);require(s);f.schemas.push_back(pin(s->GetStruct()));(void)trace_vector(s,f.data.data(),false);}
   else if(name==L"TraceChannel" || name==L"DrawDebugType") {auto en=pin(byte_enum(p));f.schemas.push_back(en);require(path(en)==(name==L"TraceChannel"?"/Script/Engine.ETraceTypeQuery":"/Script/Engine.EDrawDebugTrace"));auto n=ascii(static_cast<UEnum*>(en.pointer)->GetNameByValue(0).ToString());require(n.ends_with(name==L"TraceChannel"?"TraceTypeQuery1":"None"));}
   else if(name==L"bTraceComplex" || name==L"bIgnoreSelf" || name==L"ReturnValue") {auto* b=CastField<FBoolProperty>(p);bool_bounds(b);b->SetPropertyValueInContainer(f.data.data(),name!=L"ReturnValue");}
   else if(name==L"ActorsToIgnore") {auto* a=CastField<FArrayProperty>(p);auto* o=a?CastField<FObjectProperty>(a->GetInner()):nullptr;require(o && a->GetSize()==sizeof(FScriptArray) && o->GetSize()==sizeof(UObject*));f.schemas.push_back(pin(o->GetPropertyClass()));require(path(f.schemas.back())=="/Script/Engine.Actor");new(a->ContainerPtrToValuePtr<void>(f.data.data())) FScriptArray{};}
   else if(name==L"OutHit") {auto* h=CastField<FStructProperty>(p);require(h && p->HasAnyPropertyFlags(CPF_OutParm));auto hs=pin(h->GetStruct());f.schemas.push_back(hs);require(path(hs)=="/Script/Engine.HitResult" && h->GetSize()>0 && h->GetSize()<=1024);f.hit_schema=hs;f.hit_type=static_cast<UStruct*>(hs.pointer);
    auto* component=CastField<FWeakObjectProperty>(trace_field(f.hit_type,L"Component",h->GetSize()));require(component && component->GetSize()==sizeof(FWeakObjectPtr));f.schemas.push_back(pin(component->GetPropertyClass()));require(path(f.schemas.back())=="/Script/Engine.PrimitiveComponent");
    require(CastField<FIntProperty>(trace_field(f.hit_type,L"Item",h->GetSize())));bool_bounds(CastField<FBoolProperty>(trace_field(f.hit_type,L"bBlockingHit",h->GetSize())));
   }else if(name==L"TraceColor" || name==L"TraceHitColor") {auto* s=CastField<FStructProperty>(p);require(s);auto t=pin(s->GetStruct());f.schemas.push_back(t);require(path(t)=="/Script/CoreUObject.LinearColor" && s->GetSize()==16);for(auto* k:{L"R",L"G",L"B",L"A"})require(CastField<FFloatProperty>(trace_field(static_cast<UStruct*>(t.pointer),k,16)));}
   else require(name==L"DrawTime" && CastField<FFloatProperty>(p));
  }require(seen==expected && f.hit_type);
 }
public:
 struct TraceHit {bool blocked{};int item{-1};Object component;};
 bool trace_ready(const Context& c) const noexcept {
  try {auto in=trace_inputs(c);TraceFrame location,rotation,line;trace_frame(location,in.camera,L"GetCameraLocation","/Script/Engine.PlayerCameraManager:GetCameraLocation");trace_camera_schema(location,false);
   trace_frame(rotation,in.camera,L"GetCameraRotation","/Script/Engine.PlayerCameraManager:GetCameraRotation");trace_camera_schema(rotation,true);
   trace_frame(line,in.library,L"LineTraceSingle","/Script/Engine.KismetSystemLibrary:LineTraceSingle");trace_line_schema(line,in);return trace_inputs_live(c,in) && trace_frame_live(location) && trace_frame_live(rotation) && trace_frame_live(line);
  }catch(...){return false;}
 }
 TraceHit trace(const Context& c,const std::function<bool()>& guard) const {
  using namespace RC::Unreal;auto in=trace_inputs(c);TraceFrame location,rotation,line;
  trace_frame(location,in.camera,L"GetCameraLocation","/Script/Engine.PlayerCameraManager:GetCameraLocation");trace_camera_schema(location,false);
  trace_frame(rotation,in.camera,L"GetCameraRotation","/Script/Engine.PlayerCameraManager:GetCameraRotation");trace_camera_schema(rotation,true);
  trace_frame(line,in.library,L"LineTraceSingle","/Script/Engine.KismetSystemLibrary:LineTraceSingle");trace_line_schema(line,in);
  auto native_current=[&]{return trace_inputs_live(c,in) && trace_frame_live(location) && trace_frame_live(rotation) && trace_frame_live(line);};
  auto gate=[&]{require(native_current() && guard && guard());require(native_current());};
  auto camera=[&](TraceFrame& f,bool rot) {gate();f.source.pointer->ProcessEvent(static_cast<UFunction*>(f.function.pointer),f.data.data());require(native_current());
   auto* p=CastField<FStructProperty>(trace_field(static_cast<UFunction*>(f.function.pointer),L"ReturnValue",f.size));return trace_vector(p,f.data.data(),rot);};
  auto start=camera(location,false),angle=camera(rotation,true);const double radians=3.14159265358979323846/180.0;
  std::array<double,3> end{start[0]+50000*std::cos(angle[0]*radians)*std::cos(angle[1]*radians),start[1]+50000*std::cos(angle[0]*radians)*std::sin(angle[1]*radians),start[2]+50000*std::sin(angle[0]*radians)};
  auto* fn=static_cast<UFunction*>(line.function.pointer);auto* from=CastField<FStructProperty>(trace_field(fn,L"Start",line.size));auto* to=CastField<FStructProperty>(trace_field(fn,L"End",line.size));
  trace_vector(from,line.data.data(),false,start);trace_vector(to,line.data.data(),false,end);
  auto* hit=CastField<FStructProperty>(trace_field(fn,L"OutHit",line.size));void* hit_storage=hit->ContainerPtrToValuePtr<void>(line.data.data());
  gate();line.hit_type->InitializeStruct(hit_storage);line.hit_storage=hit_storage;require(native_current());gate();
  line.source.pointer->ProcessEvent(fn,line.data.data());require(native_current());
  auto* returned=CastField<FBoolProperty>(trace_field(fn,L"ReturnValue",line.size));TraceHit result;result.blocked=returned->GetPropertyValueInContainer(line.data.data());
  if(!result.blocked)return result; // Native miss is final; no AABB or actor fallback.
  auto* blocked=CastField<FBoolProperty>(trace_field(line.hit_type,L"bBlockingHit",hit->GetSize()));require(blocked->GetPropertyValueInContainer(hit_storage));
  auto* item=CastField<FIntProperty>(trace_field(line.hit_type,L"Item",hit->GetSize()));result.item=item->GetPropertyValueInContainer(hit_storage);require(result.item==-1);
  auto* component=CastField<FWeakObjectProperty>(trace_field(line.hit_type,L"Component",hit->GetSize()));
  // Commit the PRODUCED weak index+serial BEFORE any Lua callback or marshaling.
  result.component.weak=*component->ContainerPtrToValuePtr<FWeakObjectPtr>(hit_storage);result.component.pointer=result.component.weak.Get();
  require(live(result.component) && native_current());return result;
 }
