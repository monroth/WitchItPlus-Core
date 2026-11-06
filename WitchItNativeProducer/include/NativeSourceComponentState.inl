// Apply cooked overrides to engine-created components through reflected native
// setters. Never overwrite BodyInstance, render resources, ownership or delegates.
static void source_setter(UObject* target,const wchar_t* name,const wchar_t* path,const wchar_t* argument,Property* property,void* value) {
 using namespace RC::Unreal;auto* fn=function(target,name,path);native::Parameters args(fn);
 auto* p=field(fn,argument,fn->GetParmsSize());
 if(auto* objectProperty=CastField<FObjectProperty>(property)) {
  require(CastField<FObjectProperty>(p)&&p->GetSize()==sizeof(UObject*)&&property->GetSize()==sizeof(UObject*),"source object setter schema differs");
  // Native UFunction parameters contain resolved UObject pointers, not a
  // copied UObjectPtr handle from a cooked member property.
  *static_cast<UObject**>(args.value(p))=objectProperty->GetObjectPropertyValue(value);
 } else {
  if(p->GetClass()!=property->GetClass()||p->GetSize()!=property->GetSize())throw std::runtime_error("source setter schema differs: "+std::string(name,name+std::char_traits<wchar_t>::length(name)));
  property->CopyCompleteValue(args.value(p),value);
 }
 target->ProcessEvent(fn,args.data());
}
static void source_component_state(UObject* target,UObject* source,bool root) {
 using namespace RC::Unreal;require(target!=source&&target->GetClassPrivate()==source->GetClassPrivate(),"source component instance differs");
 auto* scene=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.SceneComponent");
 if(!scene||!source->IsA(scene))return;
 // Registered class components may be Static. Use the engine setter while
 // applying instance overrides, then restore their authored mobility below.
 auto* mobilityFn=function(target,L"SetMobility",L"/Script/Engine.SceneComponent:SetMobility");native::Parameters mobilityArgs(mobilityFn);
 enum_value(field(mobilityFn,L"NewMobility",mobilityFn->GetParmsSize()),mobilityArgs.data(),2,L"/Script/Engine.EComponentMobility");target->ProcessEvent(mobilityFn,mobilityArgs.data());
 if(!root) {
  auto* fn=function(target,L"K2_SetRelativeTransform",L"/Script/Engine.SceneComponent:K2_SetRelativeTransform");native::Parameters args(fn);
  auto* p=CastField<FStructProperty>(field(fn,L"NewTransform",fn->GetParmsSize()));require(p&&p->GetStruct()->GetPathName()==L"/Script/CoreUObject.Transform","source relative transform schema differs");
  transform_value(p,args.value(p),component_placement(source));
  auto* teleport=CastField<FBoolProperty>(field(fn,L"bTeleport",fn->GetParmsSize()));require(teleport,"relative teleport parameter missing");teleport->SetPropertyValue(args.value(teleport),true);target->ProcessEvent(fn,args.data());
 }
 auto boolean=[&](const wchar_t* property,const wchar_t* setter,const wchar_t* path,const wchar_t* parameter){
  auto* p=CastField<FBoolProperty>(field(source->GetClassPrivate(),property,source->GetClassPrivate()->GetStructureSize()));require(p,"source visibility schema differs");
  boolean_call(target,setter,path,parameter,p->GetPropertyValue(p->ContainerPtrToValuePtr<void>(source)));
 };
 boolean(L"bVisible",L"SetVisibility",L"/Script/Engine.SceneComponent:SetVisibility",L"bNewVisibility");
 boolean(L"bHiddenInGame",L"SetHiddenInGame",L"/Script/Engine.SceneComponent:SetHiddenInGame",L"NewHidden");
 auto set=[&](const wchar_t* property,const wchar_t* setter,const wchar_t* path,const wchar_t* parameter){
  auto* p=source->GetClassPrivate()->FindProperty(FName(property,FNAME_Find));if(!p)return;
  source_setter(target,setter,path,parameter,p,p->ContainerPtrToValuePtr<void>(source));
 };
 auto* mesh=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.StaticMeshComponent");
 if(mesh&&source->IsA(mesh))set(L"StaticMesh",L"SetStaticMesh",L"/Script/Engine.StaticMeshComponent:SetStaticMesh",L"NewMesh");
 auto* primitive=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.PrimitiveComponent");
 if(primitive&&source->IsA(primitive)) {
  for(auto names:{std::array<const wchar_t*,4>{L"GetCollisionProfileName",L"/Script/Engine.PrimitiveComponent:GetCollisionProfileName",L"SetCollisionProfileName",L"InCollisionProfileName"},
                  std::array<const wchar_t*,4>{L"GetCollisionEnabled",L"/Script/Engine.PrimitiveComponent:GetCollisionEnabled",L"SetCollisionEnabled",L"NewType"}}) {
   auto* get=function(source,names[0],names[1]);native::Parameters value(get);auto* returned=field(get,L"ReturnValue",get->GetParmsSize());source->ProcessEvent(get,value.data());
   source_setter(target,names[2],names[2]==std::wstring_view(L"SetCollisionEnabled")?L"/Script/Engine.PrimitiveComponent:SetCollisionEnabled":L"/Script/Engine.PrimitiveComponent:SetCollisionProfileName",names[3],returned,value.value(returned));
  }
  auto* overrides=CastField<FArrayProperty>(source->GetClassPrivate()->FindProperty(FName(L"OverrideMaterials",FNAME_Find)));
  if(overrides) {
   require(CastField<FObjectProperty>(overrides->GetInner())&&overrides->GetInner()->GetSize()==sizeof(UObject*),"source material array schema differs");
   FScriptArrayHelper array(overrides,overrides->ContainerPtrToValuePtr<void>(source));require(array.Num()<4096,"source material count limit");
   auto* fn=function(target,L"SetMaterial",L"/Script/Engine.PrimitiveComponent:SetMaterial");
   for(int i=0;i<array.Num();++i){native::Parameters args(fn);auto* index=CastField<FIntProperty>(field(fn,L"ElementIndex",fn->GetParmsSize()));auto* material=CastField<FObjectProperty>(field(fn,L"Material",fn->GetParmsSize()));require(index&&material&&material->GetSize()==sizeof(UObject*),"source material setter differs");
    *static_cast<int*>(args.value(index))=i;*static_cast<UObject**>(args.value(material))=static_cast<FObjectProperty*>(overrides->GetInner())->GetObjectPropertyValue(array.GetRawPtr(i));target->ProcessEvent(fn,args.data());}
  }
 }
 auto* box=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.BoxComponent");
 if(box&&source->IsA(box))set(L"BoxExtent",L"SetBoxExtent",L"/Script/Engine.BoxComponent:SetBoxExtent",L"InBoxExtent");
 auto* sphere=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.SphereComponent");
 if(sphere&&source->IsA(sphere))set(L"SphereRadius",L"SetSphereRadius",L"/Script/Engine.SphereComponent:SetSphereRadius",L"InSphereRadius");
 set(L"Mobility",L"SetMobility",L"/Script/Engine.SceneComponent:SetMobility",L"NewMobility");
}

// Registered render/physics components must run their engine update path.
// Assigning the reflected SkeletalMesh/LeaderPose/Constraint fields alone leaves
// their bone buffers, render proxies and physics frames out of sync.
static void source_recipe_call(UObject* target,const wchar_t* name,const wchar_t* path,
 std::initializer_list<std::pair<const wchar_t*,Recipe>> arguments,const SourceResolve& resolve) {
 auto* fn=function(target,name,path);native::Parameters args(fn);
 for(auto& [key,value]:arguments){auto* p=field(fn,key,fn->GetParmsSize());source_value(p,args.value(p),value,resolve);}
 target->ProcessEvent(fn,args.data());
}
static bool source_component_update(UObject* target,std::string_view name,const Recipe& value,const SourceResolve& resolve) {
 Recipe yes;yes.kind=Recipe::Bool;yes.boolean=true;
 if(name=="SkeletalMesh")source_recipe_call(target,L"SetSkeletalMeshAsset",L"/Script/Engine.SkeletalMeshComponent:SetSkeletalMeshAsset",{{L"NewMesh",value}},resolve);
 else if(name=="LeaderPoseComponent")source_recipe_call(target,L"SetLeaderPoseComponent",L"/Script/Engine.SkinnedMeshComponent:SetLeaderPoseComponent",{{L"NewLeaderBoneComponent",value},{L"bForceUpdate",yes}},resolve);
 else if(name=="DecalMaterial")source_recipe_call(target,L"SetDecalMaterial",L"/Script/Engine.DecalComponent:SetDecalMaterial",{{L"NewDecalMaterial",value}},resolve);
 else if(name=="bReplicates")source_recipe_call(target,L"SetIsReplicated",L"/Script/Engine.ActorComponent:SetIsReplicated",{{L"ShouldReplicate",value}},resolve);
 else if(name=="AnimationData") {
  if(auto* animation=value.get("AnimToPlay"))source_recipe_call(target,L"PlayAnimation",L"/Script/Engine.SkeletalMeshComponent:PlayAnimation",{{L"NewAnimToPlay",*animation},{L"bLooping",yes}},resolve);
  if(auto* position=value.get("SavedPosition"))source_recipe_call(target,L"SetPosition",L"/Script/Engine.SkeletalMeshComponent:SetPosition",{{L"InPos",*position}},resolve);
 } else if(name=="ConstraintInstance") {
  for(int i=0;i<2;++i){Recipe frame;frame.kind=Recipe::Number;frame.number=i;auto n=std::to_string(i+1);
   if(auto* position=value.get("Pos"+n))source_recipe_call(target,L"SetConstraintReferencePosition",L"/Script/Engine.PhysicsConstraintComponent:SetConstraintReferencePosition",{{L"Frame",frame},{L"RefPosition",*position}},resolve);
   auto* pri=value.get("PriAxis"+n);auto* sec=value.get("SecAxis"+n);
   if(pri&&sec)source_recipe_call(target,L"SetConstraintReferenceOrientation",L"/Script/Engine.PhysicsConstraintComponent:SetConstraintReferenceOrientation",{{L"Frame",frame},{L"PriAxis",*pri},{L"SecAxis",*sec}},resolve);
  }
 }else return false;
 return true;
}

// Native deferred actors can already have registered native components.
// Assigning a UObject field bypasses the setter's render/system update. Route
// leaf assets through the engine too; never copy built render/lighting state.
static void source_leaf_properties(UObject* target,const Recipe& properties,const SourceResolve& resolve) {
 using namespace RC::Unreal;
 // Stage scalar overrides first. The subsequent asset setter must build its
 // proxy with the final decal size/sort/fade and component settings.
 for(auto& entry:properties.list()) {
  const auto& name=entry.list()[0].text();
  if(name=="DecalMaterial"||name=="StaticMesh"||name=="OverrideMaterials"||name=="Asset"||name=="Template")continue;
  Recipe one;one.kind=Recipe::Array;one.array.push_back(entry);source_properties(target->GetClassPrivate(),target,one,resolve);
 }
 for(auto& entry:properties.list()) {
  const auto& name=entry.list()[0].text();const auto& value=entry.list()[2];
  if(name=="DecalMaterial")source_component_update(target,name,value,resolve);
  else if(name=="StaticMesh")source_recipe_call(target,L"SetStaticMesh",L"/Script/Engine.StaticMeshComponent:SetStaticMesh",{{L"NewMesh",value}},resolve);
  else if(name=="OverrideMaterials") {
   for(unsigned i=0;i<value.list().size();++i){Recipe index;index.kind=Recipe::Number;index.number=i;
    source_recipe_call(target,L"SetMaterial",L"/Script/Engine.PrimitiveComponent:SetMaterial",{{L"ElementIndex",index},{L"Material",value.list()[i]}},resolve);}
  }else if(name=="Asset" || name=="Template") {
   const auto* method=name=="Asset"?L"SetAsset":L"SetTemplate";
   const auto* path=name=="Asset"?L"/Script/Niagara.NiagaraComponent:SetAsset":L"/Script/Engine.ParticleSystemComponent:SetTemplate";
   auto* fn=function(target,method,path);native::Parameters args(fn);unsigned objects=0;
   for(auto* p:TFieldRange<FProperty>(fn))if(p->HasAnyPropertyFlags(CPF_Parm) && !p->HasAnyPropertyFlags(CPF_ReturnParm)) {
    if(CastField<FObjectProperty>(p)){source_value(p,args.value(p),value,resolve);++objects;}
    else require(CastField<FBoolProperty>(p),"source asset setter parameter schema differs");
   }
   require(objects==1,"source asset setter object parameter differs");target->ProcessEvent(fn,args.data());
  }
 }
 // Visibility's engine setter invalidates the render proxy using all authored
 // scalar settings (light colour/intensity). Both changes
 // occur in one construction call, before a frame is rendered; no registration
 // churn, source-world resources or standing repair is involved.
 auto* visible=CastField<FBoolProperty>(target->GetClassPrivate()->FindProperty(FName(L"bVisible",FNAME_Find)));
 if(visible && target->GetClassPrivate()->GetName().find(L"LightComponent")!=std::wstring::npos){const bool original=visible->GetPropertyValue(visible->ContainerPtrToValuePtr<void>(target));
  boolean_call(target,L"SetVisibility",L"/Script/Engine.SceneComponent:SetVisibility",L"bNewVisibility",!original);
  boolean_call(target,L"SetVisibility",L"/Script/Engine.SceneComponent:SetVisibility",L"bNewVisibility",original);}
}

// For a class-created, already registered component, stage authored values in an
// unregistered component and use engine setters to rebuild render/physics state.
// No object is copied from another UWorld; no native physics pointer is copied.
static void source_component_recipe(UObject* target,const Recipe& metadata,const SourceResolve& resolve) {
 using namespace RC::Unreal;
 auto* transient=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Engine/Transient");require(transient,"engine transient package unavailable");
 FStaticConstructObjectParameters params{target->GetClassPrivate(),transient};params.SetFlags=RF_Transient;
 auto* values=UObjectGlobals::StaticConstructObject(params);pin(values);
 // Cooked instance fields are deltas from the class-created component, not
 // from an empty component CDO. Retain the live class defaults for fields that
 // were not serialized. Only reflected settings are copied: never BodyInstance
 // itself (it contains physics pointers), ownership, render or registration data.
 for(auto name:{L"StaticMesh",L"OverrideMaterials",L"Mobility",L"bVisible",L"bHiddenInGame",L"RelativeLocation",L"RelativeRotation",L"RelativeScale3D",L"BoxExtent",L"SphereRadius"}) {
  auto* p=target->GetClassPrivate()->FindProperty(FName(name,FNAME_Find));if(!p)continue;
  p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(values),p->ContainerPtrToValuePtr<void>(target));
 }
 if(auto* body=CastField<FStructProperty>(target->GetClassPrivate()->FindProperty(FName(L"BodyInstance",FNAME_Find)))) {
  auto* type=body->GetStruct().Get();auto* from=body->ContainerPtrToValuePtr<void>(target);auto* to=body->ContainerPtrToValuePtr<void>(values);
  for(auto name:{L"CollisionProfileName",L"CollisionEnabled",L"ObjectType",L"CollisionResponses"}) {
   auto* p=type->FindProperty(FName(name,FNAME_Find));if(!p)continue;
   p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(to),p->ContainerPtrToValuePtr<void>(from));
  }
 }
 source_properties(values->GetClassPrivate(),values,metadata.at("p"),resolve);
 static const std::set<std::string> setters={"StaticMesh","OverrideMaterials","Mobility","bVisible","bHiddenInGame","RelativeLocation","RelativeRotation","RelativeScale3D","BodyInstance","BoxExtent","SphereRadius"};
 Recipe remaining;remaining.kind=Recipe::Array;
 for(auto& entry:metadata.at("p").list())if(!setters.contains(entry.list()[0].text())) {
  const auto& name=entry.list()[0].text();
  // Keep skeleton/animation/constraint engine updates, but apply decal and
  // particle assets after all their scalar overrides, just as native actors.
  if(name=="DecalMaterial"||!source_component_update(target,name,entry.list()[2],resolve))remaining.array.push_back(entry);
 }
 source_leaf_properties(target,remaining,resolve);
 source_component_state(target,values,metadata.at("root").boolean);
}
