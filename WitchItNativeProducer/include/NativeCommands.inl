// Commands settle against accepted native state after the authoritative script
// body returns. A void ProcessEvent invocation alone is never an acceptance.
bool command_execute(UObject* pc,std::string_view kind,std::string_view body) {
 using namespace RC::Unreal;require(enabled_&&IsInGameThreadRaw()&&body.size()<=4096,"command execution refused");
 auto life=pin(pc);auto world=pin(reinterpret_cast<UObject*>(pc->GetWorld()));
 auto read_object=[&](UObject* o,const wchar_t* name) {
  pin(o);auto* p=CastField<FObjectProperty>(field(o->GetClassPrivate(),name,o->GetClassPrivate()->GetStructureSize()));require(p,"command object schema differs");
  auto* value=p->GetObjectPropertyValue(p->ContainerPtrToValuePtr<void>(o));pin(value);return value;
 };
 auto read_bool=[&](UObject* o,const wchar_t* name) {
  auto* p=CastField<FBoolProperty>(field(o->GetClassPrivate(),name,o->GetClassPrivate()->GetStructureSize()));require(p,"command bool schema differs");return p->GetPropertyValueInContainer(o);
 };
 auto read_byte=[&](UObject* o,const wchar_t* name) {
  auto* p=field(o->GetClassPrivate(),name,o->GetClassPrivate()->GetStructureSize());require(p->GetSize()==1&&(CastField<FByteProperty>(p)||CastField<FEnumProperty>(p)),"command byte schema differs");return *p->ContainerPtrToValuePtr<std::uint8_t>(o);
 };
 // HasAuthority has no parameters and a bool result in the actor schema.
 auto* authority=function(pc,L"HasAuthority",L"/Script/Engine.Actor:HasAuthority");native::Parameters proof(authority);
 auto* authorityResult=CastField<FBoolProperty>(field(authority,L"ReturnValue",authority->GetParmsSize()));require(authorityResult,"authority schema differs");
 pc->ProcessEvent(authority,proof.data());require(authorityResult->GetPropertyValue(proof.value(authorityResult)),"host authority required");
 const auto args=recipe::Parser(body,4096).parse().list();std::vector<std::wstring> names;
 const std::wstring base=L"/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:",objective=L"/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectivePlayerController.B_ObjectivePlayerController_C:";
 std::wstring path;auto name=source_text(kind);
 if(kind=="SetReadyServer"){path=base+name;names={L"WantsToSpectate",L"Ready"};}
 else if(kind=="SelectPlayerTeamServer"){path=base+name;names={L"SelectedTeam"};}
 else if(kind=="SelectHunterSkill"||kind=="SelectWitchSkill"){path=base+name;names={L"Skill",L"SkillCategory"};}
 else if(kind=="SelectObjectiveTeam"){path=objective+name;names={L"ObjectiveTeam"};}
 else {require(kind=="SetReadyObjectiveTeamSelection"||kind=="SurrenderServer","command not allowed");path=objective+name;}
 auto* fn=pc->GetFunctionByNameInChain(FName(name.c_str(),FNAME_Find));require(fn&&fn->GetPathName()==path&&fn->HasAnyFunctionFlags(FUNC_NetServer)&&!fn->HasAnyFunctionFlags(FUNC_Native),"command RPC schema differs");
 require(args.size()==names.size(),"command arity differs");native::Parameters parameters(fn);std::size_t count=0;UObject* requestedSkill{};std::wstring skillSlot;
 for(auto* p:TFieldRange<FProperty>(fn))if(p->HasAnyPropertyFlags(CPF_Parm)) {
  require(count<names.size()&&p->GetFName()==FName(names[count].c_str(),FNAME_Find)&&!p->HasAnyPropertyFlags(CPF_OutParm|CPF_ReturnParm|CPF_ReferenceParm)&&p->GetArrayDim()==1&&p->IsInContainer(fn->GetParmsSize()),"command parameter schema differs");
  auto* dest=parameters.value(p);const auto& value=args[count];
  if(names[count]==L"Skill") {
   require(CastField<FClassProperty>(p)&&p->GetSize()==sizeof(UObject*),"skill schema differs");auto asset=value.text();require(asset.starts_with("/Game/Blueprints/Skills/")&&asset.ends_with("_C"),"skill class refused");requestedSkill=resident_asset(source_text(asset));pin(requestedSkill);*static_cast<UObject**>(dest)=requestedSkill;
  }else if(auto* b=CastField<FBoolProperty>(p)){require(value.kind==Recipe::Bool,"command bool required");b->SetPropertyValue(dest,value.boolean);}
  else {auto n=value.numeric();require(n>=0&&n<=255&&n==std::floor(n)&&p->GetSize()==1&&(CastField<FByteProperty>(p)||CastField<FEnumProperty>(p)),"command byte required");*static_cast<std::uint8_t*>(dest)=static_cast<std::uint8_t>(n);}
  if(names[count]==L"SkillCategory") {
   auto* byte=CastField<FByteProperty>(p);require(byte&&byte->GetEnum().Get(),"skill category enum unavailable");
   const auto category=byte->GetEnum()->GetNameByValue(static_cast<int64_t>(value.numeric())).ToString();
   if(category.find(L"Movement")!=std::wstring::npos)skillSlot=L"Movement";
   else if(category.find(L"Utility")!=std::wstring::npos)skillSlot=L"Utility";
   else throw std::runtime_error("skill category has no verified selected-state slot");
  }
  ++count;
 }
 require(count==args.size(),"command parameter count differs");
 auto* ps=read_object(pc,L"PlayerState");auto psLife=pin(ps);
 pc->ProcessEvent(fn,parameters.data());
 require(usable(life)&&usable(world)&&usable(psLife)&&reinterpret_cast<UObject*>(pc->GetWorld())==world.pointer&&read_object(pc,L"PlayerState")==ps,"command lifetime retired");
 if(kind=="SetReadyServer")return read_bool(ps,L"IsReady")==args[1].boolean&&read_bool(ps,L"WantsToSpectate")==args[0].boolean;
 if(kind=="SelectPlayerTeamServer")return read_byte(ps,L"Team")==args[0].numeric();
 if(kind=="SelectObjectiveTeam")return read_byte(ps,L"ObjectiveTeam")==args[0].numeric();
 if(kind=="SetReadyObjectiveTeamSelection")return read_bool(ps,L"IsReadyObjectiveTeamSelection");
 if(requestedSkill) {
  auto* p=CastField<FStructProperty>(field(ps->GetClassPrivate(),L"SelectedObjectiveSkills",ps->GetClassPrivate()->GetStructureSize()));require(p&&p->GetStruct(),"selected skills schema differs");
  // The cooked signature's category byte selects one slot. Observing the
  // same class in the other slot is not acceptance of this command.
  require(!skillSlot.empty(),"unknown skill category");
  const wchar_t* slot=kind=="SelectHunterSkill"
   ?(skillSlot==L"Movement"?L"HunterMovement_5_972050544946E965CA999CB953A6C20C":L"HunterUtility_6_A50F517B4189925A416735804AF141C9")
   :(skillSlot==L"Movement"?L"WitchMovement_7_277148B94174BD1D295A6E8350CF3B8F":L"WitchUtility_8_0A4175D745028494279E1788F5FC94B3");
  auto* selected=p->ContainerPtrToValuePtr<void>(ps);
  auto* value=CastField<FClassProperty>(field(p->GetStruct().Get(),slot,p->GetStruct()->GetStructureSize()));require(value,"selected skill slot schema differs");
  return value->GetObjectPropertyValue(value->ContainerPtrToValuePtr<void>(selected))==requestedSkill;
 }
 // Surrender is accepted only at the match manager's committed team field.
 auto* gp=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__GameplayStatics");pin(gp);
 auto* getGI=function(gp,L"GetGameInstance",L"/Script/Engine.GameplayStatics:GetGameInstance");native::Parameters giArgs(getGI);
 auto* context=CastField<FObjectProperty>(field(getGI,L"WorldContextObject",getGI->GetParmsSize()));auto* result=CastField<FObjectProperty>(field(getGI,L"ReturnValue",getGI->GetParmsSize()));require(context&&result,"game instance schema differs");
 context->SetObjectPropertyValue(giArgs.value(context),pc);gp->ProcessEvent(getGI,giArgs.data());auto* gi=result->GetObjectPropertyValue(giArgs.value(result));auto* mm=read_object(gi,L"ObjectiveMatchManager");
 const auto team=read_byte(ps,L"ObjectiveTeam");return team>0&&read_byte(mm,L"SurrenderedTeam")==team;
}
