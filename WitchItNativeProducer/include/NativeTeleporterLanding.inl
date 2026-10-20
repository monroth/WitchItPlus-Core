// Utility exit landing uses the real current collision footprints, at the one
// native Teleport entry. The cooked Blueprint still moves/rotates/replicates the
// actor. Reflected native buffers keep HitResult OUT fields out of Lua.
using AuditVector=std::array<double,3>;
struct TeleporterWatch {Allocation zone,exit;};
struct TeleportEvent {Allocation pawn,zone;RC::StringType pawn_name;AuditVector before{},target{},exit{};double marker_radius{},pawn_radius{};};
std::vector<TeleporterWatch> teleporters_;
std::map<RC::Unreal::FFrame*,TeleportEvent> teleport_events_;
static double audit_number(RC::Unreal::UStruct* type,void* data,int size,const wchar_t* name) {
 using namespace RC::Unreal;auto* p=field(type,name,size);
 if(auto* f=CastField<FFloatProperty>(p))return *f->ContainerPtrToValuePtr<float>(data);
 auto* d=CastField<FDoubleProperty>(p);require(d);return *d->ContainerPtrToValuePtr<double>(data);
}
static AuditVector audit_vector(RC::Unreal::UStruct* type,void* data,int size,const wchar_t* name) {
 using namespace RC::Unreal;auto* p=CastField<FStructProperty>(field(type,name,size));
 require(p && p->GetStruct()->GetPathName()==L"/Script/CoreUObject.Vector");
 auto* structure=p->GetStruct().Get();auto* v=p->ContainerPtrToValuePtr<void>(data);const auto n=structure->GetStructureSize();
 return {audit_number(structure,v,n,L"X"),audit_number(structure,v,n,L"Y"),audit_number(structure,v,n,L"Z")};
}
static AuditVector audit_location(UObject* actor,bool component=false) {
 auto* fn=function(actor,component?L"K2_GetComponentLocation":L"K2_GetActorLocation",component?L"/Script/Engine.SceneComponent:K2_GetComponentLocation":L"/Script/Engine.Actor:K2_GetActorLocation");
 native::Parameters params(fn);actor->ProcessEvent(fn,params.data());return audit_vector(fn,params.data(),fn->GetParmsSize(),L"ReturnValue");
}
static std::string audit_point(const AuditVector& v){return std::to_string(v[0])+","+std::to_string(v[1])+","+std::to_string(v[2]);}
static double landing_bounds(UObject* actor,const AuditVector& position,bool collision_only=true) {
 using namespace RC::Unreal;auto* fn=function(actor,L"GetActorBounds",L"/Script/Engine.Actor:GetActorBounds");native::Parameters p(fn);const auto n=fn->GetParmsSize();
 for(auto* name:{L"bOnlyCollidingComponents",L"bIncludeFromChildActors"}){auto* b=CastField<FBoolProperty>(field(fn,name,n));require(b);b->SetPropertyValue(p.value(b),std::wstring_view(name)==L"bIncludeFromChildActors" || collision_only);}
 actor->ProcessEvent(fn,p.data());
 return landing_radius(position,audit_vector(fn,p.data(),n,L"Origin"),audit_vector(fn,p.data(),n,L"BoxExtent"));
}
static double landing_yaw(UObject* actor) {
 using namespace RC::Unreal;auto* fn=function(actor,L"K2_GetActorRotation",L"/Script/Engine.Actor:K2_GetActorRotation");native::Parameters p(fn);
 actor->ProcessEvent(fn,p.data());auto* r=CastField<FStructProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(r && r->GetStruct()->GetPathName()==L"/Script/CoreUObject.Rotator");
 return audit_number(r->GetStruct(),p.value(r),r->GetSize(),L"Yaw");
}
static void landing_target(UObject* target,const AuditVector& position) {
 using namespace RC::Unreal;auto* fn=function(target,L"K2_SetWorldLocation",L"/Script/Engine.SceneComponent:K2_SetWorldLocation");native::Parameters p(fn);const auto n=fn->GetParmsSize();
 vector_value(fn,p.data(),n,L"NewLocation",position);
 for(auto* name:{L"bSweep",L"bTeleport"}){auto* b=CastField<FBoolProperty>(field(fn,name,n));require(b);b->SetPropertyValue(p.value(b),std::wstring_view(name)==L"bTeleport");}
 // FHitResult stays in initialized native storage; none of its weak fields
 // crosses the Lua bridge. Call the engine setter to update ComponentToWorld.
 target->ProcessEvent(fn,p.data());
}

#include "NativeTeleportSamples.inl"
public:
void teleporter_exit(UObject* zone,UObject* exit) {
 auto z=pin(zone),e=pin(exit);require(zone->GetClassPrivate()->GetPathName()==L"/Game/Blueprints/StaticProps/B_TeleporterZone.B_TeleporterZone_C" && zone->GetWorld()==exit->GetWorld());
 std::erase_if(teleporters_,[&](const auto& w){return !usable(w.zone) || w.zone.pointer==zone;});teleporters_.push_back({z,e});
}
void teleport_event(UObject* zone,RC::Unreal::FFrame& frame,bool post) noexcept {
 using namespace RC::Unreal;
 try {
  if(!enabled_ || !IsInGameThreadRaw() || frame.Node()->GetPathName()!=L"/Game/Blueprints/StaticProps/B_TeleporterZone.B_TeleporterZone_C:Teleport")return;
  if(!post) {
   if(teleport_events_.contains(&frame))return;
   auto found=std::find_if(teleporters_.begin(),teleporters_.end(),[&](const auto& w){return w.zone.pointer==zone && usable(w.zone) && usable(w.exit);});
   if(found==teleporters_.end())return;
   auto* p=CastField<FObjectProperty>(field(frame.Node(),L"Actor",frame.Node()->GetStructureSize()));require(p && p->GetSize()==sizeof(UObject*));
   auto* pawn=*p->ContainerPtrToValuePtr<UObject*>(frame.Locals());auto* target=object(zone,L"TargetLocation");TeleportEvent event;
   event.pawn=pin(pawn);event.pawn_name=pawn->GetFullName();event.zone=found->zone;event.before=audit_location(pawn);event.target=audit_location(target,true);event.exit=audit_location(found->exit.pointer);
   event.marker_radius=landing_bounds(found->exit.pointer,event.exit,false);event.pawn_radius=landing_bounds(pawn,event.before);
   const auto destination=landing_ahead(event.exit,landing_yaw(found->exit.pointer),event.marker_radius,event.pawn_radius,event.target[2]);
   landing_target(target,destination);event.target=audit_location(target,true);
   auto* update=function(zone,L"ForceNetUpdate",L"/Script/Engine.Actor:ForceNetUpdate");require(update->GetParmsSize()==0);zone->ProcessEvent(update,nullptr);
   teleport_events_.emplace(&frame,std::move(event));return;
  }
  auto it=teleport_events_.find(&frame);if(it==teleport_events_.end())return;auto event=std::move(it->second);teleport_events_.erase(it);
  const auto after=usable(event.pawn)?audit_point(audit_location(event.pawn.pointer)):"retired";
  RC::Output::send(STR("[WIR] Teleport event: pawn={} before=({},{},{}) after={} target=({},{},{}) exit=({},{},{}) radii={}/{} gap=16\n"),
   event.pawn_name,event.before[0],event.before[1],event.before[2],RC::ensure_str(after.c_str()),event.target[0],event.target[1],event.target[2],event.exit[0],event.exit[1],event.exit[2],event.marker_radius,event.pawn_radius);
  if(teleport_trace_next_) {teleport_trace_next_=false;start_teleport_samples(event.pawn,event.pawn_name);}
 }catch(const std::exception& error){teleport_events_.erase(&frame);RC::Output::send(STR("[WIR] Teleporter landing failed: {}\n"),RC::ensure_str(error.what()));}
 catch(...){teleport_events_.erase(&frame);RC::Output::send(STR("[WIR] Teleporter landing failed\n"));}
}
private:
