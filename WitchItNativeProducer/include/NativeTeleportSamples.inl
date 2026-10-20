// Bounded, game-thread telemetry of only the actual teleported pawn. No world
// enumeration, UObject roots, Lua callbacks, physics mutation or retry loop.
struct TeleportSample {Allocation pawn;RC::StringType name;std::chrono::steady_clock::time_point start;TeleportSampleSchedule schedule;std::uint64_t id{};};
std::vector<TeleportSample> teleport_samples_;
bool teleport_trace_next_{};
std::uint64_t teleport_sample_id_{};
static std::string sample_text(const RC::StringType& text){return std::string(text.begin(),text.end());}
static std::string sample_bool(bool value){return value?"true":"false";}
static UObject* sample_object(UObject* source,const wchar_t* name) {
 using namespace RC::Unreal;if(!source)return nullptr;
 auto* property=source->GetClassPrivate()->FindProperty(FName(name,FNAME_Find));if(!property)return nullptr;
 auto* p=CastField<FObjectProperty>(property);require(p && p->GetArrayDim()==1 && p->GetSize()==sizeof(UObject*) && p->IsInContainer(source->GetClassPrivate()->GetStructureSize()),"teleport sample object schema differs");
 auto* value=*p->ContainerPtrToValuePtr<UObject*>(source);if(value)pin(value);return value;
}
static std::string sample_name(UObject* value){return value?sample_text(value->GetFullName()):"none";}
static std::string sample_flag(UObject* source,const wchar_t* name) {
 using namespace RC::Unreal;auto* p=source->GetClassPrivate()->FindProperty(FName(name,FNAME_Find));if(!p)return "n/a";
 auto* b=CastField<FBoolProperty>(field(source->GetClassPrivate(),name,source->GetClassPrivate()->GetStructureSize()));require(b,"teleport sample bool schema differs");
 return sample_bool(b->GetPropertyValueInContainer(source));
}
static std::string sample_byte(RC::Unreal::UStruct* type,void* data,int size,const wchar_t* name) {
 using namespace RC::Unreal;auto* p=field(type,name,size);require(p->GetSize()==1 && (CastField<FByteProperty>(p)||CastField<FEnumProperty>(p)),"teleport sample enum schema differs");
 return std::to_string(*p->ContainerPtrToValuePtr<std::uint8_t>(data));
}
static RC::Unreal::UFunction* sample_function(UObject* target,const wchar_t* name) {
 using namespace RC::Unreal;auto* fn=target->GetFunctionByNameInChain(FName(name,FNAME_Find));require(fn && fn->HasAnyFunctionFlags(FUNC_Native),"teleport sample native getter unavailable");return fn;
}
static std::string sample_native_bool(UObject* target,const wchar_t* name) {
 using namespace RC::Unreal;auto* fn=sample_function(target,name);native::Parameters p(fn);target->ProcessEvent(fn,p.data());
 auto* out=CastField<FBoolProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(out && out->HasAnyPropertyFlags(CPF_ReturnParm));return sample_bool(out->GetPropertyValue(p.value(out)));
}
static std::string sample_floor(UObject* movement) {
 using namespace RC::Unreal;if(!movement)return "n/a";
 auto* f=CastField<FStructProperty>(field(movement->GetClassPrivate(),L"CurrentFloor",movement->GetClassPrivate()->GetStructureSize()));require(f,"CurrentFloor struct unavailable");
 auto* type=f->GetStruct().Get();auto* data=f->ContainerPtrToValuePtr<void>(movement);const auto n=type->GetStructureSize();
 auto* blocked=CastField<FBoolProperty>(field(type,L"bBlockingHit",n));require(blocked,"CurrentFloor bBlockingHit unavailable");
 auto* h=CastField<FStructProperty>(field(type,L"HitResult",n));require(h && h->GetStruct()->GetPathName()==L"/Script/Engine.HitResult","CurrentFloor HitResult differs");
 auto* hit=h->ContainerPtrToValuePtr<void>(data);
 // UE5.7 HitResult's actor handle is not a UObject*. Its Component is the
 // reflected weak index+serial used by ShipTraceNative; resolve owner natively.
 auto* c=CastField<FWeakObjectProperty>(field(h->GetStruct(),L"Component",h->GetSize()));require(c && c->GetSize()==sizeof(FWeakObjectPtr),"HitResult Component weak schema differs");
 Allocation component;component.weak=*c->ContainerPtrToValuePtr<FWeakObjectPtr>(hit);component.pointer=component.weak.Get();UObject* actor=nullptr;
 if(usable(component)) {auto* fn=sample_function(component.pointer,L"GetOwner");native::Parameters p(fn);component.pointer->ProcessEvent(fn,p.data());
  auto* o=CastField<FObjectProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(o && o->GetSize()==sizeof(UObject*));actor=*o->ContainerPtrToValuePtr<UObject*>(p.data());if(actor)pin(actor);
 }
 return "blocking="+sample_bool(blocked->GetPropertyValueInContainer(data))+",dist="+std::to_string(audit_number(type,data,n,L"FloorDist"))+",hit_actor="+sample_name(actor);
}
static std::string sample_collision(UObject* actor) {
 using namespace RC::Unreal;auto* c=sample_object(actor,L"CapsuleComponent");const bool capsule=c!=nullptr;if(!c)c=sample_object(actor,L"RootComponent");if(!c)return "none";
 auto* enabled=sample_function(c,L"GetCollisionEnabled");native::Parameters p(enabled);c->ProcessEvent(enabled,p.data());const auto mode=sample_byte(enabled,p.data(),enabled->GetParmsSize(),L"ReturnValue");
 auto* profile=sample_function(c,L"GetCollisionProfileName");native::Parameters q(profile);c->ProcessEvent(profile,q.data());
 auto* name=CastField<FNameProperty>(field(profile,L"ReturnValue",profile->GetParmsSize()));require(name && name->GetSize()==sizeof(FName),"collision profile FName schema differs");
 return std::string(capsule?"capsule":"root")+",enabled="+mode+",profile="+sample_text(name->ContainerPtrToValuePtr<FName>(q.data())->ToString());
}
static std::string sample_possession(UObject* pawn) {
 auto* controller=sample_object(pawn,L"Controller");auto* controlled=sample_object(controller,L"Pawn");auto* playerState=sample_object(pawn,L"PlayerState");
 std::string result="possessed="+(controller?sample_bool(controlled==pawn):"false")+",controller="+sample_name(controller)+",controller_pawn="+sample_name(controlled)+",hidden="+sample_flag(pawn,L"bHidden")+",dead="+sample_flag(playerState?playerState:pawn,L"IsDead");
 if(!controller) {using namespace RC::Unreal;auto* fn=sample_function(pawn,L"GetInstigatorController");native::Parameters p(fn);pawn->ProcessEvent(fn,p.data());
  auto* property=CastField<FObjectProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(property && property->GetSize()==sizeof(UObject*));auto* instigator=*property->ContainerPtrToValuePtr<UObject*>(p.data());if(instigator)pin(instigator);
  result+=",instigator_controller="+sample_name(instigator)+",instigator_pawn="+sample_name(sample_object(instigator,L"Pawn"))+",controlled_by_witch="+sample_flag(pawn,L"IsControlledByWitch");
 }
 return result;
}
static std::string sample_camera(UObject* pawn) {
 auto* controller=sample_object(pawn,L"Controller");auto* camera=sample_object(controller,L"PlayerCameraManager");if(!camera)return "n/a";
 auto* fn=sample_function(camera,L"GetCameraLocation");native::Parameters p(fn);camera->ProcessEvent(fn,p.data());return audit_point(audit_vector(fn,p.data(),fn->GetParmsSize(),L"ReturnValue"));
}
void teleport_sample(const TeleportSample& sample,double due,double elapsed) noexcept {
 using namespace RC::Unreal;std::string out="[WIR] Teleport sample: id="+std::to_string(sample.id)+" due="+std::to_string(due)+" elapsed="+std::to_string(elapsed)+" pawn="+sample_text(sample.name);
 try {
  if(!usable(sample.pawn)){out+=" alive=false retired=true";}
  else {
   auto* pawn=sample.pawn.pointer;
   const auto item=[&](const char* name,auto fn){out+=" ";out+=name;out+="=";try{out+=fn();}catch(const std::exception& e){out+="unavailable(";out+=e.what();out+=")";}catch(...){out+="unavailable(exception)";}};
   item("alive",[&]{return sample_bool(engine_valid(sample.pawn)&&!destroying(sample.pawn));});
   item("location",[&]{return audit_point(audit_location(pawn));});
   item("velocity",[&]{auto* fn=sample_function(pawn,L"GetVelocity");native::Parameters p(fn);pawn->ProcessEvent(fn,p.data());return audit_point(audit_vector(fn,p.data(),fn->GetParmsSize(),L"ReturnValue"));});
   auto* movement=sample_object(pawn,L"CharacterMovement");
   item("MovementMode",[&]{return movement?sample_byte(movement->GetClassPrivate(),movement,movement->GetClassPrivate()->GetStructureSize(),L"MovementMode"):"n/a";});
   item("IsFalling",[&]{return movement?sample_native_bool(movement,L"IsFalling"):"n/a";});
   item("CurrentFloor",[&]{return sample_floor(movement);});
   item("collision",[&]{return sample_collision(pawn);});
   item("possession",[&]{return sample_possession(pawn);});
   item("camera",[&]{return sample_camera(pawn);});
  }
 }catch(const std::exception& e){out+=" sample_error=";out+=e.what();}catch(...){out+=" sample_error=exception";}
 RC::Output::send(STR("{}\n"),RC::ensure_str(out.c_str()));
}
void start_teleport_samples(const Allocation& pawn,const RC::StringType& name) {
 TeleportSample sample{pawn,name,std::chrono::steady_clock::now(),{},++teleport_sample_id_};
 const auto due=sample.schedule.due(0);teleport_sample(sample,*due,0);teleport_samples_.push_back(std::move(sample));
}
public:
void sample_teleport_ticks() noexcept {
 if(!enabled_ || !RC::Unreal::IsInGameThreadRaw() || teleport_samples_.empty())return;
 const auto now=std::chrono::steady_clock::now();
 for(auto& sample:teleport_samples_) {const double elapsed=std::chrono::duration<double>(now-sample.start).count();while(auto due=sample.schedule.due(elapsed))teleport_sample(sample,*due,elapsed);}
 std::erase_if(teleport_samples_,[](const auto& sample){return sample.schedule.done();});
}
void arm_teleport_trace() {teleport_trace_next_=true;}
private:
