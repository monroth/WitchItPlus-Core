// One synchronous Pawn-profile sweep. HitResult (including its weak members)
// lives and dies in initialized native storage; Lua receives only scalars.
// HitResult uses Engine's quantized Vector subclasses, not CoreUObject.Vector.
// Read inherited numeric fields through reflection; never cast a HitResult layout.
static AuditVector respawn_hit_vector(RC::Unreal::UStruct* type,void* data,int size,const wchar_t* name,const wchar_t* expected) {
 using namespace RC::Unreal;auto* p=CastField<FStructProperty>(field(type,name,size));
 require(p && p->GetStruct()->GetPathName()==expected,"respawn hit vector schema differs");
 auto* structure=p->GetStruct().Get();auto* v=p->ContainerPtrToValuePtr<void>(data);const auto n=structure->GetStructureSize();
 return {audit_number(structure,v,n,L"X"),audit_number(structure,v,n,L"Y"),audit_number(structure,v,n,L"Z")};
}
public:
int respawn_probe(Stack& s,UObject* context,UObject* ignored,AuditVector start,AuditVector end,double radius,double height) {
 using namespace RC::Unreal;
 require(enabled_ && IsInGameThreadRaw());auto owner=pin(context),skip=pin(ignored);
 auto* world=context->GetWorld();require(world && world==ignored->GetWorld());
 require(radius>0 && height>=radius && height<=4096);
 auto* library=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__KismetSystemLibrary");require(library);
 auto* fn=function(library,L"CapsuleTraceSingleByProfile",L"/Script/Engine.KismetSystemLibrary:CapsuleTraceSingleByProfile");
 native::Parameters p(fn);const auto n=fn->GetParmsSize();
 auto* ctx=CastField<FObjectProperty>(field(fn,L"WorldContextObject",n));require(ctx && ctx->GetSize()==sizeof(UObject*));
 *static_cast<UObject**>(p.value(ctx))=context;
 vector_value(fn,p.data(),n,L"Start",start);vector_value(fn,p.data(),n,L"End",end);
 scalar_value(fn,p.data(),n,L"Radius",radius);scalar_value(fn,p.data(),n,L"HalfHeight",height);
 auto* profile=CastField<FNameProperty>(field(fn,L"ProfileName",n));require(profile && profile->GetSize()==sizeof(FName));
 *static_cast<FName*>(p.value(profile))=FName(L"Pawn",FNAME_Add);
 for(auto name:{L"bTraceComplex",L"bIgnoreSelf"}){auto* b=CastField<FBoolProperty>(field(fn,name,n));require(b);b->SetPropertyValue(p.value(b),std::wstring_view(name)==L"bIgnoreSelf");}
 enum_value(field(fn,L"DrawDebugType",n),p.data(),0,L"/Script/Engine.EDrawDebugTrace");
 auto* array=CastField<FArrayProperty>(field(fn,L"ActorsToIgnore",n));auto* inner=array?CastField<FObjectProperty>(array->GetInner()):nullptr;
 require(inner && inner->GetSize()==sizeof(UObject*));FScriptArrayHelper ignore(array,static_cast<FScriptArray*>(p.value(array)));
 ignore.AddValues(1);*reinterpret_cast<UObject**>(ignore.GetRawPtr(0))=ignored;
 auto* hit=CastField<FStructProperty>(field(fn,L"OutHit",n));require(hit && hit->HasAnyPropertyFlags(CPF_OutParm) && hit->GetStruct()->GetPathName()==L"/Script/Engine.HitResult");
 auto* returned=CastField<FBoolProperty>(field(fn,L"ReturnValue",n));require(returned);
 library->ProcessEvent(fn,p.data());require(usable(owner) && usable(skip) && context->GetWorld()==world && ignored->GetWorld()==world);
 const bool blocked=returned->GetPropertyValue(p.value(returned));s.boolean(blocked);
 if(!blocked)return 1;
 auto* type=hit->GetStruct().Get();auto* data=p.value(hit);const auto size=hit->GetSize();
 auto* penetrating=CastField<FBoolProperty>(field(type,L"bStartPenetrating",size));require(penetrating);
 s.boolean(penetrating->GetPropertyValue(penetrating->ContainerPtrToValuePtr<void>(data)));
 for(double v:respawn_hit_vector(type,data,size,L"ImpactPoint",L"/Script/Engine.Vector_NetQuantize")){require(std::isfinite(v));s.number(v);}
 for(double v:respawn_hit_vector(type,data,size,L"ImpactNormal",L"/Script/Engine.Vector_NetQuantizeNormal")){require(std::isfinite(v));s.number(v);}
 return 8;
}
private:
