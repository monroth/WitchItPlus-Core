static Placement component_placement(UObject* root) {
 using namespace RC::Unreal;pin(root);
 auto vector=[&](const wchar_t* name,std::array<const wchar_t*,3> axes){
  auto* p=CastField<FStructProperty>(field(root->GetClassPrivate(),name,root->GetClassPrivate()->GetStructureSize()));require(p,"source transform field unavailable");
  auto* type=p->GetStruct().Get();void* data=p->ContainerPtrToValuePtr<void>(root);std::array<double,3> values{};
  for(int i=0;i<3;++i){auto* q=field(type,axes[i],type->GetStructureSize());
   if(auto* f=CastField<FFloatProperty>(q)){require(f->GetSize()==sizeof(float));values[i]=*f->ContainerPtrToValuePtr<float>(data);}
   else{auto* d=CastField<FDoubleProperty>(q);require(d && d->GetSize()==sizeof(double));values[i]=*d->ContainerPtrToValuePtr<double>(data);}
   require(std::isfinite(values[i]),"nonfinite cooked transform");}
  return values;
 };
 auto p=vector(L"RelativeLocation",{L"X",L"Y",L"Z"}),r=vector(L"RelativeRotation",{L"Pitch",L"Yaw",L"Roll"}),s=vector(L"RelativeScale3D",{L"X",L"Y",L"Z"});
 return {p[0],p[1],p[2],r[1],r[0],r[2],s[0],s[1],s[2]};
}
Placement source_placement(const std::wstring& path) {
 using namespace RC::Unreal;
 auto* source=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,path.c_str());require(source,"source actor unavailable");pin(source);
 auto* root=object(source,L"RootComponent");require(root,"source root unavailable");pin(root);
 require(!object(root,L"AttachParent"),"attached source actor requires an audited world transform");
 return component_placement(root);
}
// Exact cooked component templates, constructed by the engine. Never memcpy a
// BodyInstance (it owns physics state), approximate a convex brush by its bounds,
// run a source gameplay Blueprint, or register a source level in the game world.
bool source_present(UObject* world,const std::wstring& path) {
 using namespace RC::Unreal;
 auto* source=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,path.c_str());
 if(!source || !UObject::IsReal(source))return false;
 return usable(pin(source)) && reinterpret_cast<UObject*>(source->GetWorld())==world;
}
void source_geometry(const Object& actor,const std::wstring& path,Placement position,const std::string& componentNames) {
 using namespace RC::Unreal;
 require(usable(actor),"geometry destination retired");
 auto* source=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,path.c_str());
 require(source && UObject::IsReal(source),"cooked geometry actor unavailable");pin(source);
 require(!source->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject|RF_ArchetypeObject)),"geometry source is a template actor");
 auto* hidden=CastField<FBoolProperty>(field(source->GetClassPrivate(),L"bHidden",source->GetClassPrivate()->GetStructureSize()));
 auto* targetHidden=CastField<FBoolProperty>(field(actor.pointer->GetClassPrivate(),L"bHidden",actor.pointer->GetClassPrivate()->GetStructureSize()));
 require(hidden && targetHidden,"actor hidden flag unavailable");targetHidden->SetPropertyValue(targetHidden->ContainerPtrToValuePtr<void>(actor.pointer),hidden->GetPropertyValue(hidden->ContainerPtrToValuePtr<void>(source)));
 auto* collision=CastField<FBoolProperty>(field(source->GetClassPrivate(),L"bActorEnableCollision",source->GetClassPrivate()->GetStructureSize()));
 auto* targetCollision=CastField<FBoolProperty>(field(actor.pointer->GetClassPrivate(),L"bActorEnableCollision",actor.pointer->GetClassPrivate()->GetStructureSize()));
 require(collision && targetCollision,"actor collision flag unavailable");targetCollision->SetPropertyValue(targetCollision->ContainerPtrToValuePtr<void>(actor.pointer),collision->GetPropertyValue(collision->ContainerPtrToValuePtr<void>(source)));
 auto* tags=CastField<FArrayProperty>(field(source->GetClassPrivate(),L"Tags",source->GetClassPrivate()->GetStructureSize()));
 auto* targetTags=CastField<FArrayProperty>(field(actor.pointer->GetClassPrivate(),L"Tags",actor.pointer->GetClassPrivate()->GetStructureSize()));
 require(tags && targetTags && CastField<FNameProperty>(tags->GetInner()) && CastField<FNameProperty>(targetTags->GetInner()) && tags->GetSize()==sizeof(TArray<FName>) && targetTags->GetSize()==sizeof(TArray<FName>),"actor tag array schema differs");
 *targetTags->ContainerPtrToValuePtr<TArray<FName>>(actor.pointer)=*tags->ContainerPtrToValuePtr<TArray<FName>>(source);
 auto* sceneClass=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.SceneComponent");
 require(sceneClass,"SceneComponent class unavailable");
 auto* root=componentNames.empty()?object(source,L"RootComponent"):nullptr;
 if(!componentNames.empty()) {
  auto name=componentNames.substr(0,componentNames.find('|'));auto full=path+L"."+std::wstring(name.begin(),name.end());
  root=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,full.c_str());
 }
 require(root && UObject::IsReal(root),"source root component unavailable");pin(root);
 require(root->GetOuterPrivate()==source && root->IsA(sceneClass),"source root is not actor-owned SceneComponent");
 auto* childClass=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.ChildActorComponent");
 require(childClass && !root->IsA(childClass),"source root is a child actor spawner");
 require(!object(root,L"AttachParent"),"source root attached outside actor");
 // A loaded source UWorld is not initialized as a game world. Its transient
 // OwnedComponents set is not the serialized component inventory. Resolve only
 // the generator's exact, cooked actor subobjects (never a global object scan).
 std::vector<UObject*> pending{root};
 require(componentNames.size()<1048576,"source component name limit");
 if(!componentNames.empty()) {
  std::size_t begin=0;
  while(begin<componentNames.size()) {
   auto end=componentNames.find('|',begin);auto name=componentNames.substr(begin,end==std::string::npos?end:end-begin);
   require(!name.empty() && name.size()<256 && std::all_of(name.begin(),name.end(),[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}),"invalid source component name");
   auto full=path+L"."+std::wstring(name.begin(),name.end());
   auto* c=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,full.c_str());
   require(c && UObject::IsReal(c),"cooked scene subobject unavailable");pin(c);
   require(c->GetOuterPrivate()==source && c->IsA(sceneClass) && !c->IsA(childClass),"source subobject is not actor-owned scene geometry");
   if(std::find(pending.begin(),pending.end(),c)==pending.end())pending.push_back(c);
   require(pending.size()<4096,"source scene component limit");
   if(end==std::string::npos)break;begin=end+1;
  }
 } else {
  // Version-1 companions written before explicit subobject names: walk only the
  // serialized attachment tree, including its real root even if OwnedComponents
  // is empty (DecalActor/StaticMeshActor/Light all commonly have a single root).
  for(std::size_t i=0;i<pending.size();++i) {
   auto* parent=pending[i];auto* cp=CastField<FArrayProperty>(field(parent->GetClassPrivate(),L"AttachChildren",parent->GetClassPrivate()->GetStructureSize()));
   require(cp && CastField<FObjectProperty>(cp->GetInner()) && cp->GetSize()==sizeof(TArray<UObject*>),"source attachment array schema differs");
   for(auto* c:*cp->ContainerPtrToValuePtr<TArray<UObject*>>(parent)) {
    if(!c)continue;pin(c);
    if(c->GetOuterPrivate()==source && c->IsA(sceneClass) && !c->IsA(childClass) && std::find(pending.begin(),pending.end(),c)==pending.end())pending.push_back(c);
    require(pending.size()<4096,"source scene component limit");
   }
  }
 }
 std::map<UObject*,UObject*> copies;
 auto setObject=[&](UObject* o,const wchar_t* name,UObject* value){auto* p=CastField<FObjectProperty>(field(o->GetClassPrivate(),name,o->GetClassPrivate()->GetStructureSize()));require(p && p->GetSize()==sizeof(UObject*),"geometry object field differs");*p->ContainerPtrToValuePtr<UObject*>(o)=value;};
 auto clearLinks=[&](UObject* c){
  setObject(c,L"AttachParent",nullptr);
  auto* p=CastField<FArrayProperty>(field(c->GetClassPrivate(),L"AttachChildren",c->GetClassPrivate()->GetStructureSize()));
  require(p && CastField<FObjectProperty>(p->GetInner()) && p->GetSize()==sizeof(TArray<UObject*>),"component child array schema differs");
  p->ContainerPtrToValuePtr<TArray<UObject*>>(c)->Empty();
 };
 auto* finish=function(actor.pointer,L"FinishAddComponent",L"/Script/Engine.Actor:FinishAddComponent");
 // FinishAddComponent registers the new component with its real owner. Reflection
 // supplies every parameter offset; StaticConstructObject owns template copying,
 // UObject references, material arrays and FBodyInstance construction/reset.
 while(!pending.empty()) {
  bool progress=false;
  for(auto i=pending.begin();i!=pending.end();) {
   auto* original=*i;pin(original);require(original->IsA(sceneClass),"source component is not SceneComponent");
   auto* parent=original==root?nullptr:object(original,L"AttachParent");
   if(parent && !copies.contains(parent)){++i;continue;}
   FStaticConstructObjectParameters construct{original->GetClassPrivate(),actor.pointer};
   construct.Template=original;construct.SetFlags=RF_Transient;construct.bCopyTransientsFromClassDefaults=true;
   auto* copy=UObjectGlobals::StaticConstructObject(construct);pin(copy);clearLinks(copy);
   // Actor transform IS its root transform. Place the root before registration
   // so a Static component is never moved after its physics/render state exists.
   if(original==root) {
    auto* type=copy->GetClassPrivate();const int size=type->GetStructureSize();
    vector_value(type,copy,size,L"RelativeLocation",{position.x,position.y,position.z});
    vector_value(type,copy,size,L"RelativeScale3D",{position.sx,position.sy,position.sz});
    auto* rotation=CastField<FStructProperty>(field(type,L"RelativeRotation",size));require(rotation,"component rotation unavailable");
    auto* rt=rotation->GetStruct().Get();void* data=rotation->ContainerPtrToValuePtr<void>(copy);
    scalar_value(rt,data,rt->GetStructureSize(),L"Pitch",position.pitch);
    scalar_value(rt,data,rt->GetStructureSize(),L"Yaw",position.yaw);
    scalar_value(rt,data,rt->GetStructureSize(),L"Roll",position.roll);
   }
   // Prevent component replication even when its source Blueprint used it.
   auto* replicated=CastField<FBoolProperty>(field(copy->GetClassPrivate(),L"bReplicates",copy->GetClassPrivate()->GetStructureSize()));
   require(replicated,"component replication field unavailable");replicated->SetPropertyValue(replicated->ContainerPtrToValuePtr<void>(copy),false);
   native::Parameters add(finish);
   auto* cp=CastField<FObjectProperty>(field(finish,L"Component",finish->GetParmsSize()));
   auto* manual=CastField<FBoolProperty>(field(finish,L"bManualAttachment",finish->GetParmsSize()));
   auto* transform=CastField<FStructProperty>(field(finish,L"RelativeTransform",finish->GetParmsSize()));
   require(cp && cp->GetSize()==sizeof(UObject*) && manual && transform && transform->GetStruct()->GetPathName()==L"/Script/CoreUObject.Transform","FinishAddComponent schema differs");
   *static_cast<UObject**>(add.value(cp))=copy;
   manual->SetPropertyValue(add.value(manual),true);
   transform_value(transform,add.value(transform),component_placement(copy));
   // Manual attachment skips automatic reparenting, but FinishAddComponent still
   // writes RelativeTransform: supply the real cooked local transform explicitly.
   if(original==root)setObject(actor.pointer,L"RootComponent",copy);
   else setObject(copy,L"AttachParent",parent?copies.at(parent):copies.at(root));
   if(original!=root) {
    auto* target=parent?copies.at(parent):copies.at(root);
    auto* p=CastField<FArrayProperty>(field(target->GetClassPrivate(),L"AttachChildren",target->GetClassPrivate()->GetStructureSize()));
    require(p && p->GetSize()==sizeof(TArray<UObject*>),"geometry parent child array differs");
    p->ContainerPtrToValuePtr<TArray<UObject*>>(target)->Add(copy);
   }
   auto* primitive=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.PrimitiveComponent");
   if(primitive && copy->IsA(primitive)) {
    auto* simulate=function(copy,L"SetSimulatePhysics",L"/Script/Engine.PrimitiveComponent:SetSimulatePhysics");native::Parameters physical(simulate);
    auto* enable=CastField<FBoolProperty>(field(simulate,L"bSimulate",simulate->GetParmsSize()));require(enable,"SetSimulatePhysics schema differs");
    enable->SetPropertyValue(physical.value(enable),false);copy->ProcessEvent(simulate,physical.data());
   }
   actor.pointer->ProcessEvent(finish,add.data());copies.emplace(original,copy);i=pending.erase(i);progress=true;
  }
  require(progress,"source component attachment cycle or missing parent");
 }
 actor.pointer->SetFlags(RF_Transient);
 // Generic Actor + SceneComponents have none of Creative's placeable interfaces,
 // save registration or selection metadata. No structure/prop budget entry.
 RC::Output::send(STR("[WIR] SourceGeometry: copied {} components source={} actor={}\n"),copies.size(),path,actor.pointer->GetFullName());
}
