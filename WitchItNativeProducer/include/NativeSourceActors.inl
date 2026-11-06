// Self-contained cooked values; no source UWorld is loaded or retained.
struct SourceRow {
 std::wstring source,klass,parent,component;Placement position;bool authority{};
 Recipe state;Object actor{};Context context{};bool owned{},child{};
};
UObject* source_batch_world_{};
std::set<UObject*> source_batch_classes_;
std::vector<Allocation> source_begins_;
bool source_deferring_{};
std::map<UObject*,Allocation> source_owned_;
std::map<std::wstring,Allocation> source_restored_;
unsigned source_query_calls_{};
bool source_static_profiled_{};
struct SourceCandidate {Object actor;Placement position;};
std::map<UObject*,std::vector<SourceCandidate>> source_base_index_;
std::map<UObject*,Placement> source_positions_;
std::vector<Allocation> source_rollback_;
std::vector<RC::Unreal::Hook::GlobalCallbackId> source_hooks_;
std::pair<int,int> source_query_hook_{};
bool source_query_installed_{};
RC::Unreal::UFunction* source_query_function_{};
void source_unhook()noexcept {
 using namespace RC::Unreal;
 for(auto id:source_hooks_)Hook::UnregisterCallback(id);source_hooks_.clear();
 if(source_query_installed_)UObjectGlobals::UnregisterHook(source_query_function_,source_query_hook_);
 source_query_function_=nullptr;
 source_query_installed_=false;source_deferring_=false;source_batch_world_=nullptr;source_batch_classes_.clear();source_begins_.clear();
}
bool source_owned(UObject* actor)const {
 auto i=source_owned_.find(actor);return i!=source_owned_.end() && usable(i->second);
}
void source_filter(RC::Unreal::FArrayProperty* p,void* data) {
 using namespace RC::Unreal;
 require(p&&CastField<FObjectProperty>(p->GetInner())&&p->GetInner()->GetSize()==sizeof(UObject*)&&p->GetSize()==sizeof(TArray<UObject*>),"source edit/save actor array differs");
 auto& actors=*static_cast<TArray<UObject*>*>(data);
 for(int i=actors.Num()-1;i>=0;--i)if(source_owned(actors[i]))actors.RemoveAt(i);
}
void source_install_hooks() {
 using namespace RC::Unreal;if(source_query_installed_)return;
 source_unhook();
 try {
 auto edit=[this](auto& info,UObject*,FFrame& frame,void*) {
  if(source_owned_.empty() || !IsInGameThreadRaw())return;
  try {
   auto* fn=frame.Node();auto path=fn->GetPathName();
   const auto prefix=L"/Game/Blueprints/Gamemodes/CreativeMode/B_CreativePlayerController.B_CreativePlayerController_C:";
   if(path==L"/Game/Blueprints/Match/BFL_MapLayoutLoading.BFL_MapLayoutLoading_C:StoreStatics") {
    auto* p=CastField<FArrayProperty>(field(fn,L"Statics",fn->GetStructureSize()));
    auto* data=FindOutParamValueAddress(frame,p);if(!data)data=p->ContainerPtrToValuePtr<void>(frame.Locals());source_filter(p,data);return;
   }
   if(!path.starts_with(prefix))return;
   auto name=fn->GetName();
   if(name==L"DeleteMultiple"||name==L"DeleteMultipleServer") {
    auto* p=CastField<FArrayProperty>(field(fn,L"ActorsToDelete",fn->GetStructureSize()));
    auto* data=p->HasAnyPropertyFlags(CPF_OutParm)?FindOutParamValueAddress(frame,p):p->ContainerPtrToValuePtr<void>(frame.Locals());source_filter(p,data);return;
   }
   const wchar_t* argument=name==L"SelectActor"?L"TargetActor":name==L"SelectActorSever"?L"ActorToEdit":name==L"SelectMultiple"?L"HitActor":nullptr;
   if(argument){auto* p=CastField<FObjectProperty>(field(fn,argument,fn->GetStructureSize()));require(p&&p->GetSize()==sizeof(UObject*));if(source_owned(*p->ContainerPtrToValuePtr<UObject*>(frame.Locals())))info.PreventOriginalFunctionCall();}
  }catch(const std::exception& e){RC::Output::send(STR("[WIR] SourceGeometry: edit/save filtering failed: {}\n"),widen(e.what()));}
 };
 Hook::FScriptNameFilter filter;
 for(auto name:{L"SelectActor",L"SelectActorSever",L"SelectMultiple",L"DeleteMultiple",L"DeleteMultipleServer",L"StoreStatics"})filter.ComparisonIndices.push_back(static_cast<std::int32_t>(FName(name,FNAME_Add).GetComparisonIndex().ToUnstableInt()));
 source_hooks_.push_back(Hook::RegisterProcessLocalScriptFunctionPreCallback(edit,{false,false,STR("WitchItNativeProducer"),STR("source scenery is not editable/saveable"),filter}));
 source_hooks_.push_back(Hook::RegisterProcessInternalPreCallback(edit,{false,false,STR("WitchItNativeProducer"),STR("source scenery is not editable/saveable"),filter}));
 auto post=[this](UnrealScriptFunctionCallableContext& c,void*) {
  if(source_owned_.empty() || !IsInGameThreadRaw())return;
  try {
   auto* fn=c.TheStack.Node();auto* cp=CastField<FObjectProperty>(field(fn,L"ActorClass",fn->GetStructureSize()));require(cp&&cp->GetSize()==sizeof(UObject*));
   auto* cls=*cp->ContainerPtrToValuePtr<UObject*>(c.TheStack.Locals());if(!cls)return;
   auto path=cls->GetName();
   // Creative's general inventories feed its budget, selection and save lists.
   // Specific gameplay class queries continue seeing switches and doors.
   if(path!=L"BP_PlaceableActor_C" && path!=L"BP_PlaceableStaticProp_C")return;
   auto* p=CastField<FArrayProperty>(field(fn,L"OutActors",fn->GetStructureSize()));
   auto* data=FindOutParamValueAddress(c.TheStack,p);if(!data)data=p->ContainerPtrToValuePtr<void>(c.TheStack.Locals());source_filter(p,data);
  }catch(const std::exception& e){RC::Output::send(STR("[WIR] SourceGeometry: inventory filtering failed: {}\n"),widen(e.what()));}
 };
 // One startup hook for all batches. No hook registration or cancellation is
 // permitted in source_actors, component copying, or reference remapping.
 Hook::FScriptNameFilter begin_filter{{static_cast<std::int32_t>(FName(L"ReceiveBeginPlay",FNAME_Add).GetComparisonIndex().ToUnstableInt())}};
 source_hooks_.push_back(Hook::RegisterProcessEventPreCallback([this](auto& info,UObject* o,UFunction*,void*) {
  if(!source_deferring_||!IsInGameThreadRaw()||!o)return;
  if(source_batch_classes_.contains(o->GetClassPrivate())&&reinterpret_cast<UObject*>(o->GetWorld())==source_batch_world_) {
   source_begins_.push_back(pin(o));info.PreventOriginalFunctionCall();
  }
 },{false,false,STR("WitchItNativeProducer"),STR("source batch BeginPlay"),begin_filter}));
 require(source_hooks_.back()!=Hook::ERROR_ID,"source startup BeginPlay hook unavailable");
 source_query_function_=UObjectGlobals::StaticFindObject<UFunction*>(nullptr,nullptr,L"/Script/Engine.GameplayStatics:GetAllActorsOfClass");
 require(source_query_function_,"source inventory function unavailable");
 source_query_hook_=UObjectGlobals::RegisterHook(source_query_function_,[](auto&,void*){},post,nullptr);source_query_installed_=true;
 }catch(...){source_unhook();throw;}
}
static std::wstring widen(const std::string& s){return {s.begin(),s.end()};}
static UObject* returned_object(UObject* target,const wchar_t* name,const wchar_t* expected) {
 using namespace RC::Unreal;auto* fn=function(target,name,expected);native::Parameters args(fn);
 auto* p=CastField<FObjectProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(p && p->GetSize()==sizeof(UObject*),"source getter return schema differs");
 target->ProcessEvent(fn,args.data());return *static_cast<UObject**>(args.value(p));
}
static void boolean_call(UObject* target,const wchar_t* name,const wchar_t* expected,const wchar_t* argument,bool value) {
 using namespace RC::Unreal;auto* fn=function(target,name,expected);native::Parameters args(fn);
 auto* p=CastField<FBoolProperty>(field(fn,argument,fn->GetParmsSize()));require(p,"source boolean parameter differs");
 p->SetPropertyValue(args.value(p),value);target->ProcessEvent(fn,args.data());
}
void source_retry_rollback() {
 auto pending=std::move(source_rollback_);source_rollback_.clear();
 for(auto actor:pending)try{destroy(actor);}catch(const std::exception& e){
  source_rollback_.push_back(actor);RC::Output::send(STR("[WIR] SourceGeometry: actor rollback retained: {}\n"),widen(e.what()));
 }
 require(source_rollback_.empty(),"source actor rollback pending; retained for next cleanup");
}
void release_source_levels() {
 source_retry_rollback();source_owned_.clear();source_restored_.clear();source_base_index_.clear();source_positions_.clear();source_static_profiled_=false;
}
static Placement live_placement(UObject* actor) {
 using namespace RC::Unreal;Placement out;double* slots[3][3]={{&out.x,&out.y,&out.z},{&out.pitch,&out.yaw,&out.roll},{&out.sx,&out.sy,&out.sz}};
 const wchar_t* names[3]={L"K2_GetActorLocation",L"K2_GetActorRotation",L"GetActorScale3D"};
 for(int k=0;k<3;++k) {
  auto* fn=actor->GetFunctionByNameInChain(FName(names[k],FNAME_Find));require(fn,"source world transform getter missing");native::Parameters args(fn);
  auto* p=CastField<FStructProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(p,"source world transform return differs");
  actor->ProcessEvent(fn,args.data());auto* type=p->GetStruct().Get();void* data=args.value(p);
  const wchar_t* xyz[3]={L"X",L"Y",L"Z"};const wchar_t* pyr[3]={L"Pitch",L"Yaw",L"Roll"};
  for(int j=0;j<3;++j){auto* q=field(type,k==1?pyr[j]:xyz[j],type->GetStructureSize());
   if(CastField<FDoubleProperty>(q))*slots[k][j]=*q->ContainerPtrToValuePtr<double>(data);
   else{require(CastField<FFloatProperty>(q),"source world coordinate differs");*slots[k][j]=*q->ContainerPtrToValuePtr<float>(data);}}
 }
 return out;
}
static bool source_coincides(Placement a,Placement b) {
 if(std::hypot(a.x-b.x,a.y-b.y,a.z-b.z)>5)return false;
 auto quat=[](Placement p){
  const double h=3.14159265358979323846/360;
  const double sp=std::sin(p.pitch*h),cp=std::cos(p.pitch*h),sy=std::sin(p.yaw*h),cy=std::cos(p.yaw*h),sr=std::sin(p.roll*h),cr=std::cos(p.roll*h);
  return std::array<double,4>{cr*sp*sy-sr*cp*cy,-cr*sp*cy-sr*cp*sy,cr*cp*sy-sr*sp*cy,cr*cp*cy+sr*sp*sy};
 };
 auto qa=quat(a),qb=quat(b);double dot=0;for(int i=0;i<4;++i)dot+=qa[i]*qb[i];
 if(std::abs(dot)<std::cos(3.14159265358979323846/180))return false;
 auto scale=[](double a,double b){return std::abs(a-b)<=.01*std::max({std::abs(a),std::abs(b),1e-9});};
 return scale(a.sx,b.sx)&&scale(a.sy,b.sy)&&scale(a.sz,b.sz);
}
std::vector<UObject*> source_class_actors(UObject* world,UObject* cls) {
 using namespace RC::Unreal;++source_query_calls_;auto* gp=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__GameplayStatics");pin(gp);
 auto* fn=function(gp,L"GetAllActorsOfClass",L"/Script/Engine.GameplayStatics:GetAllActorsOfClass");native::Parameters args(fn);
 for(auto name:{L"WorldContextObject",L"ActorClass"}){auto* p=CastField<FObjectProperty>(field(fn,name,fn->GetParmsSize()));require(p&&p->GetSize()==sizeof(UObject*));*static_cast<UObject**>(args.value(p))=name==std::wstring_view(L"ActorClass")?cls:world;}
 auto* p=CastField<FArrayProperty>(field(fn,L"OutActors",fn->GetParmsSize()));require(p&&CastField<FObjectProperty>(p->GetInner())&&p->GetInner()->GetSize()==sizeof(UObject*),"source actor world query differs");
 gp->ProcessEvent(fn,args.data());FScriptArrayHelper array(p,args.value(p));std::vector<UObject*> out;
 for(int i=0;i<array.Num();++i){auto* o=*reinterpret_cast<UObject**>(array.GetRawPtr(i));if(o&&usable(pin(o))&&reinterpret_cast<UObject*>(o->GetWorld())==world&&o->GetClassPrivate()==cls)out.push_back(o);}return out;
}
const std::vector<SourceCandidate>& source_candidates(UObject* world,UObject* cls) {
 auto i=source_base_index_.find(cls);
 if(i==source_base_index_.end()) {
  std::vector<SourceCandidate> values;
  for(auto* actor:source_class_actors(world,cls))if(!source_owned(actor))values.push_back({pin(actor),live_placement(actor)});
  i=source_base_index_.emplace(cls,std::move(values)).first;
 }
 return i->second;
}
UObject* source_match(UObject* world,UObject* cls,Placement position) {
 for(auto& candidate:source_candidates(world,cls))if(usable(candidate.actor)&&source_coincides(position,candidate.position))return candidate.actor.pointer;
 // References into previously restored groups can use the retained authored
 // transform too; no new world query or reflected transform getter is needed.
 for(auto& [path,actor]:source_restored_)if(usable(actor)&&actor.pointer->GetClassPrivate()==cls) {auto p=source_positions_.find(actor.pointer);if(p!=source_positions_.end()&&source_coincides(position,p->second))return actor.pointer;}
 return nullptr;
}
static UObject* source_component(UObject* actor,const std::string& name) {
 using namespace RC::Unreal;auto path=actor->GetPathName()+L"."+source_text(name);
 return UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,path.c_str());
}
void source_register_component(UObject* actor,UObject* component,UObject* parent,bool root) {
 using namespace RC::Unreal;
 auto setObject=[](UObject* object,const wchar_t* name,UObject* value){
  auto* p=CastField<FObjectProperty>(field(object->GetClassPrivate(),name,object->GetClassPrivate()->GetStructureSize()));
  require(p&&p->GetSize()==sizeof(UObject*)&&p->GetPropertyClass().Get()&&value->IsA(p->GetPropertyClass().Get()),"source component attachment schema differs");
  *p->ContainerPtrToValuePtr<UObject*>(object)=value;
 };
 if(root)setObject(actor,L"RootComponent",component);
 else if(parent) {
  setObject(component,L"AttachParent",parent);
  auto* children=CastField<FArrayProperty>(field(parent->GetClassPrivate(),L"AttachChildren",parent->GetClassPrivate()->GetStructureSize()));
  require(children&&children->GetSize()==sizeof(TArray<UObject*>),"source attachment child array differs");children->ContainerPtrToValuePtr<TArray<UObject*>>(parent)->Add(component);
 }
 auto* finish=function(actor,L"FinishAddComponent",L"/Script/Engine.Actor:FinishAddComponent");native::Parameters args(finish);
 auto* cp=CastField<FObjectProperty>(field(finish,L"Component",finish->GetParmsSize()));auto* manual=CastField<FBoolProperty>(field(finish,L"bManualAttachment",finish->GetParmsSize()));auto* transform=CastField<FStructProperty>(field(finish,L"RelativeTransform",finish->GetParmsSize()));
 require(cp&&cp->GetSize()==sizeof(UObject*)&&manual&&transform,"source FinishAddComponent schema differs");*static_cast<UObject**>(args.value(cp))=component;manual->SetPropertyValue(args.value(manual),true);auto* scene=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.SceneComponent");transform_value(transform,args.value(transform),component->IsA(scene)?component_placement(component):Placement{0,0,0,0,0,0,1,1,1});actor->ProcessEvent(finish,args.data());
}
bool source_actors(Stack& s,UObject* worldContext,unsigned count,uikit::HostReference rows) {
 require(witchit::debug::enabled("native_source_hooks")&&witchit::debug::enabled("native_object_listener"),"source restoration disabled by local native diagnostic");
 using namespace RC::Unreal;require(enabled_&&IsInGameThreadRaw(),"source restore requires game thread");source_retry_rollback();
 require(worldContext&&usable(pin(worldContext)),"source world context unavailable");auto* world=reinterpret_cast<UObject*>(worldContext->GetWorld());require(world&&usable(pin(world)),"source world unavailable");bool host=object(world,L"AuthorityGameMode")!=nullptr;
 auto started=std::chrono::steady_clock::now();auto queries=source_query_calls_;auto pins=pin_calls_;std::vector<SourceRow> batch;
 for(unsigned i=1;i<=count;++i) {
  int top=s.top();s.push_reference(rows);s.integer(i);require(s.call(1,16,L"source_recipe",world->GetPathName()),"source recipe callback failed");SourceRow r;
  auto text=[&](int slot){auto v=s.get_string(top+slot,2048);require(v.has_value(),"source recipe string expected");return source_text(*v);};
  r.source=text(1);r.klass=text(2);auto role=text(3);require(role==L"authority"||role==L"local","source role invalid");r.authority=role==L"authority";r.parent=text(4);r.component=text(5);r.child=!r.parent.empty();
  double t[9];for(int k=0;k<9;++k){auto v=s.read(top+6+k);t[k]=v.kind==UE4SS_LUA_VALUE_INTEGER?double(v.integer_value):v.number_value;require((v.kind==UE4SS_LUA_VALUE_INTEGER||v.kind==UE4SS_LUA_VALUE_NUMBER)&&std::isfinite(t[k]),"source transform invalid");}
  r.position={t[0],t[1],t[2],t[4],t[3],t[5],t[6],t[7],t[8]};auto data=s.get_string(top+16,1250000);require(data.has_value(),"self-contained source properties missing; regenerate companion offline");r.state=recipe::authored_state(recipe::Parser(*data).parse());s.set_top(top);
  require(recipe::leaf_asset(r.klass),"source class belongs to a map package");if(!host&&r.authority)continue;
  auto* cls=resident_asset(r.klass);require(cls&&cls->IsA<UClass>(),"source class not resident");batch.push_back(std::move(r));
 }
 std::vector<Object> created;std::map<std::wstring,UObject*> links;std::map<UObject*,std::vector<std::pair<UObject*,const Recipe*>>> added;
 auto resolveFor=[&](SourceRow& row)->SourceResolve {
  return [&,rp=&row](const Recipe& v)->UObject* {
   if(v.kind==Recipe::Null||v.get("null"))return nullptr;
   if(auto* asset=v.get("asset")){auto path=source_text(asset->text());require(recipe::leaf_asset(path),"map property asset request forbidden");auto* value=resident_asset(path);require(value,"source leaf asset not resident");return value;}
   auto key=v.get("ref")?source_text(v.at("ref").text()):rp->source+L".$"+source_text(v.at("object").text());
   if(auto found=links.find(key);found!=links.end())return found->second;
   if(auto found=source_restored_.find(key);found!=source_restored_.end()&&usable(found->second))return found->second.pointer;
   // References to surviving base actors are matched in this world. Their source
   // package identity is metadata and is never passed to a loader/finder.
   auto& external=rp->state.at("external");for(auto& [identity,metadata]:external.object) {
    auto path=source_text(identity);if(key!=path&&!key.starts_with(path+L"."))continue;
    auto cls=source_text(metadata.at("class").text());auto* type=resident_asset(cls);require(type&&type->IsA<UClass>(),"source external class unavailable");auto& a=metadata.at("transform").list();require(a.size()==9,"source external transform invalid");
    Placement p{a[0].numeric(),a[1].numeric(),a[2].numeric(),a[4].numeric(),a[3].numeric(),a[5].numeric(),a[6].numeric(),a[7].numeric(),a[8].numeric()};auto* actor=source_match(world,type,p);require(actor,"source current-world reference target missing");
    auto* result=key==path?actor:source_component(actor,std::string(key.begin()+path.size()+1,key.end()));require(result,"source external component missing");links[key]=result;return result;
   }
   throw std::runtime_error("source restored reference missing: "+std::string(key.begin(),key.end()));
  };
 };
 auto bindComponents=[&](SourceRow& row,bool allowCreate){
  for(auto& component:row.state.at("components").list()) {
   auto name=component.at("name").text();auto klass=source_text(component.at("class").text());
   const auto identity=row.source+L"."+source_text(name);
   // A native actor already owns its root (Decal, LightComponent, etc.). Cooked
   // editor names may differ. Reuse that exact-class root, and retain the source
   // alias across FinishSpawning, without creating a duplicate default root.
   auto* existing=witchit::source_component_binding<UObject>([&](){auto i=links.find(identity);return i==links.end()?nullptr:i->second;},
    [&](){return source_component(row.actor.pointer,name);},[&](){return object(row.actor.pointer,L"RootComponent");},
    component.at("root").boolean,[&](auto* c){return c->GetOuterPrivate()==row.actor.pointer && c->GetClassPrivate()->GetPathName()==klass;});
   if(!existing&&allowCreate) {
    auto* cls=resident_asset(klass);auto* base=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.ActorComponent");require(cls&&cls->IsA<UClass>()&&static_cast<UClass*>(cls)->IsChildOf(base),"source component class unavailable/invalid");FStaticConstructObjectParameters params{static_cast<UClass*>(cls),row.actor.pointer};params.Name=FName(source_text(name).c_str());params.SetFlags=RF_Transient;
    existing=UObjectGlobals::StaticConstructObject(params);pin(existing);added[row.actor.pointer].push_back({existing,&component});
   }
   require(existing&&usable(pin(existing))&&existing->GetOuterPrivate()==row.actor.pointer&&existing->GetClassPrivate()->GetPathName()==klass,"source engine component missing/class differs");links[row.source+L"."+source_text(name)]=existing;
  }
 };
 require(!source_deferring_,"nested source construction refused");source_batch_world_=world;source_batch_classes_.clear();source_begins_.clear();for(auto& row:batch)source_batch_classes_.insert(resident_asset(row.klass));source_deferring_=true;
 auto reset=[&](){source_deferring_=false;source_batch_world_=nullptr;source_batch_classes_.clear();source_begins_.clear();};
 auto rollback=[&](){reset();for(auto i=created.rbegin();i!=created.rend();++i)try{destroy(*i);}catch(...){source_rollback_.push_back(*i);}};
 restore_source_batch([&](){
  for(auto& row:batch) {
   auto* cls=resident_asset(row.klass);
   if(!row.child) {
    auto* existing=source_existing_actor(row.klass,[&]()->UObject*{auto i=source_restored_.find(row.source);return i!=source_restored_.end()&&usable(i->second)?i->second.pointer:nullptr;},[&]()->const auto&{return source_candidates(world,cls);},[&](const auto& c){return usable(c.actor)&&source_coincides(row.position,c.position);});
    if(existing)row.actor=pin(existing);
    else {row.context=capture(cls,worldContext,true);spawn_owned(row.context,row.position,[&](auto a){row.actor=a;row.owned=true;created.push_back(a);source_owned_[a.pointer]=a;});}
    links[row.source]=row.actor.pointer;
    // Native scenery components already exist on a deferred actor. Configure
    // them BEFORE registration, so physics/render state is built exactly once.
    if(row.owned&&source_component_route(row.klass))bindComponents(row,true);
   }
  }
 },[&](){
  auto createValues=[&](SourceRow& row){auto resolve=resolveFor(row);for(auto& [name,value]:row.state.at("objects").object)links[row.source+L".$"+source_text(name)]=source_value_object(row.actor.pointer,value,resolve);};
  for(auto& row:batch)if(row.owned&&source_component_route(row.klass))createValues(row);
  // Material/mesh/body values on native components must precede registration.
  // Reference-dependent actor values are applied after every actor is present.
  for(auto& row:batch)if(row.owned&&source_component_route(row.klass)) {
   auto resolve=resolveFor(row);
   for(auto& [name,value]:row.state.at("objects").object){auto* object=links.at(row.source+L".$"+source_text(name));source_object_properties(object,value,resolve);}
   for(auto& c:row.state.at("components").list()) {auto* object=links.at(row.source+L"."+source_text(c.at("name").text()));source_leaf_properties(object,c.at("p"),resolve);}
   finish_owned(row.context,row.actor,row.position);
  }
  source_component_order(batch,[](auto& r){return r.source;},[](auto& r){return r.parent;},[&](auto& row){
   if(row.child) {auto p=links.find(row.parent);require(p!=links.end(),"source child parent unavailable");auto* c=source_component(p->second,std::string(row.component.begin(),row.component.end()));require(c,"source child component missing");auto* actor=object(c,L"ChildActor");require(actor&&usable(pin(actor))&&actor->GetClassPrivate()->GetPathName()==row.klass&&reinterpret_cast<UObject*>(actor->GetWorld())==world,"source constructed child invalid");row.actor=pin(actor);source_owned_[actor]=row.actor;links[row.source]=actor;}
   else if(row.owned&&!source_component_route(row.klass))finish_owned(row.context,row.actor,row.position);
   if(row.owned||row.child) {
    bindComponents(row,true);
    if(!source_component_route(row.klass)) {
     createValues(row);auto resolve=resolveFor(row);
     for(auto& [name,value]:row.state.at("objects").object)source_object_properties(links.at(row.source+L".$"+source_text(name)),value,resolve);
     for(auto& c:row.state.at("components").list())source_component_recipe(links.at(row.source+L"."+source_text(c.at("name").text())),c,resolve);
    }
    for(auto& [component,metadata]:added[row.actor.pointer]) {
     auto parent=metadata->at("parent").text();auto* parentObject=parent.empty()?nullptr:links.at(row.source+L"."+source_text(parent));source_register_component(row.actor.pointer,component,parentObject,metadata->at("root").boolean);
    }
   }
  });
 },[&](){
  for(auto& row:batch)if(row.owned||row.child) {
   auto resolve=resolveFor(row);
   source_properties(row.actor.pointer->GetClassPrivate(),row.actor.pointer,row.state.at("p"),resolve);
   boolean_call(row.actor.pointer,L"SetReplicates",L"/Script/Engine.Actor:SetReplicates",L"bInReplicates",row.authority);
  }
 },[&](){
  auto begins=std::move(source_begins_);reset();for(auto& actor:begins)if(usable(actor)){auto* fn=actor.pointer->GetFunctionByNameInChain(FName(L"ReceiveBeginPlay",FNAME_Find));if(fn)actor.pointer->ProcessEvent(fn,nullptr);}
  for(auto& row:batch)if(row.owned||row.child){source_restored_[row.source]=row.actor;source_positions_[row.actor.pointer]=row.position;}
  s.table();for(unsigned i=0;i<created.size();++i){s.object(created[i].pointer);s.set_field(-2,std::to_string(i+1));}
 },rollback);
 RC::Output::send(STR("[WIR] SourceGeometry: leaf recipe batch={} owned={} host={} elapsed_ms={:.3f} world_queries={} weak_pins={} map_loads=0\n"),batch.size(),created.size(),host,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),source_query_calls_-queries,pin_calls_-pins);
 return true;
}
