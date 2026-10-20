// Exact physical StoreStatics POST. No Lua ticket, package discovery, Core
// geometry capture or disk admission is required to preserve carrier bytes.
struct MetadataState {std::unique_ptr<witchit::save::OwnedStatics> rows;};
std::unordered_map<witchit::FrameKey,MetadataState> metadata_;
struct MetadataSubject {witchit::FrameKey save;std::uint64_t serial,generation;FFrame* store;};
void metadata_current(const MetadataSubject& s) {
    const auto* e=ledger_.find(s.save);
    if(!attached_ || generation_!=s.generation || !e || e->serial!=s.serial || e->writer_seen
        || save_scopes_.empty() || save_scopes_.back()!=s.save
        || Hook::Internal::CurrentWitchItStoreStaticsFrame()!=s.store)
        throw std::runtime_error("physical carrier copy retired");
}
FArrayProperty* metadata_array(FFrame& f,const wchar_t* name,FScriptArray*& array) {
    auto* p=CastField<FArrayProperty>(property(f.Node(),name));
    if(!p || !f.Locals())throw std::runtime_error("captured metadata array schema missing");
    array=static_cast<FScriptArray*>(p->HasAnyPropertyFlags(CPF_OutParm)?FindOutParamValueAddress(f,p):p->ContainerPtrToValuePtr<void>(f.Locals()));
    if(!array)throw std::runtime_error("captured metadata array address missing");return p;
}
void metadata_actor_current(const MetadataSubject& subject,UObject* actor) {
    auto alive=[](UObject* object) {
        return object && UObject::IsReal(object)
            && !object->HasAnyFlags(static_cast<EObjectFlags>(EObjectFlags::RF_ClassDefaultObject|EObjectFlags::RF_ArchetypeObject|EObjectFlags::RF_BeginDestroyed|EObjectFlags::RF_FinishDestroyed))
            && !object->HasAnyInternalFlags(EInternalObjectFlags::PendingKill|EInternalObjectFlags::Unreachable|EInternalObjectFlags::PendingConstruction);
    };
    metadata_current(subject);
    auto* owner=reinterpret_cast<FFrame*>(subject.save)->Object();
    if(!alive(owner) || !alive(actor))throw std::runtime_error("native source actor/owner retired");
    const auto* entry=ledger_.find(subject.save);
    if(!entry || std::to_string(reinterpret_cast<std::uintptr_t>(owner))+"|"+utf8(owner->GetFullName())!=entry->owner_key)throw std::runtime_error("native SaveLayout context differs");
    FWeakObjectPtr ownerWeak(owner),actorWeak(actor);
    auto* world=owner->GetWorld();
    metadata_current(subject);
    if(!world || ownerWeak.Get()!=owner || actorWeak.Get()!=actor || !alive(world) || !alive(actor))throw std::runtime_error("native source world retired");
    FWeakObjectPtr worldWeak(world);
    auto* actorWorld=actor->GetWorld();
    metadata_current(subject);
    if(actorWeak.Get()!=actor || ownerWeak.Get()!=owner || worldWeak.Get()!=world || actorWorld!=world || !alive(actor))throw std::runtime_error("native source actor belongs to another world/incarnation");
}
std::vector<witchit::SaveRow> metadata_actor_rows(const MetadataSubject& subject,const std::vector<witchit::SaveRow>& source) {
    auto desired=source;
    FScriptArray* actors{};auto* property=metadata_array(*subject.store,L"Statics",actors);
    auto* object=CastField<FObjectProperty>(property->GetInner());if(!object)throw std::runtime_error("native actors input differs");
    auto* switchClass=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Game/Blueprints/StaticProps/Placeable/Utility/BP_Switch.BP_Switch_C");
    auto* library=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__KismetSystemLibrary");
    auto* assetFunction=UObjectGlobals::StaticFindObject<UFunction*>(nullptr,nullptr,L"/Script/Engine.KismetSystemLibrary:GetPrimaryAssetIdFromClass");
    if(!library || !assetFunction)throw std::runtime_error("exact native actor projection unavailable");
    FScriptArrayHelper input(property,actors);std::size_t at{};std::set<std::uintptr_t> seen;
    for(int i=0;i<input.Num();++i){
        auto* actor=object->GetObjectPropertyValue(input.GetRawPtr(i));metadata_actor_current(subject,actor);
        auto* cls=actor->GetClassPrivate();if(!cls)throw std::runtime_error("native actor class missing");
        const auto path=cls->GetPathName();
        if(path==L"/Game/Blueprints/StaticProps/Placeable/Utility/BP_WorldProperties.BP_WorldProperties_C"
            || path==L"/Game/Blueprints/StaticProps/Placeable/Utility/BP_PlaceableLight.BP_PlaceableLight_C" || (switchClass && cls->IsChildOf(switchClass)))continue;
        if(at>=source.size() || !seen.insert(reinterpret_cast<std::uintptr_t>(actor)).second)throw std::runtime_error("eligible actor/row count or identity differs");
        const auto& row=source[at++];FWeakObjectPtr actorWeak(actor),classWeak(cls);
        witchit::save::GetterParameters asset(assetFunction);
        auto fields=witchit::save::inputs(assetFunction);
        auto* classInput=fields.size()==1?CastField<FClassProperty>(fields[0]):nullptr;
        auto* assetOutput=CastField<FStructProperty>(assetFunction->GetReturnProperty());
        if(!classInput || classInput->GetSize()!=sizeof(UObject*) || !assetOutput || !assetOutput->GetStruct())throw std::runtime_error("primary asset native getter signature differs");
        *static_cast<UObject**>(asset.value(classInput))=cls;
        library->ProcessEvent(assetFunction,asset.data());metadata_actor_current(subject,actor);
        if(actorWeak.Get()!=actor || classWeak.Get()!=cls)throw std::runtime_error("actor/class incarnation changed during asset getter");
        const auto id=witchit::save::primary_id(assetOutput->GetStruct(),asset.value(assetOutput));
        if(id.first!=row.asset_type || id.second!=row.asset_name)throw std::runtime_error("eligible actor primary asset differs from row");
        unsigned index{};
        for(auto [name,rotation]:{std::pair{L"K2_GetActorLocation",false},std::pair{L"K2_GetActorRotation",true},std::pair{L"GetActorScale3D",false}}) {
            auto* getter=actor->GetFunctionByNameInChain(FName(name,FNAME_Find));
            witchit::save::GetterParameters values(getter);
            auto* output=CastField<FStructProperty>(getter->GetReturnProperty());
            if(!output || !output->GetStruct() || !witchit::save::inputs(getter).empty())throw std::runtime_error("actor transform getter schema differs");
            actor->ProcessEvent(getter,values.data());metadata_actor_current(subject,actor);
            if(actorWeak.Get()!=actor || classWeak.Get()!=cls)throw std::runtime_error("actor incarnation changed during transform getter");
            for(auto value:witchit::save::coordinates(output->GetStruct(),values.value(output),rotation))
                if(value!=row.transform[index++])throw std::runtime_error("eligible actor transform differs from row");
        }
        auto* materials=CastField<FArrayProperty>(actor->GetClassPrivate()->FindProperty(FName(L"SavedMaterialIndices",FNAME_Find)));
        if(materials) {
            if(!CastField<FByteProperty>(materials->GetInner()))throw std::runtime_error("carrier payload is not bytes");
            auto* array=materials->ContainerPtrToValuePtr<FScriptArray>(actor);
            const auto bounds=witchit::save::range(materials,array,"actor-payload");
            if(bounds.num>33000)throw std::runtime_error("carrier payload too large");
            std::vector<std::uint8_t> payload;
            if(bounds.num){auto* bytes=static_cast<const std::uint8_t*>(array->GetData());payload.assign(bytes,bytes+bounds.num);}
            const auto kind=witchit::save::marker_kind(payload);
            if(kind && ((kind==1 && path!=L"/Game/Blueprints/StaticProps/Placeable/StaticProps/BP_WitchCauldron.BP_WitchCauldron_C")
                || (kind>=2 && path!=L"/Game/Blueprints/StaticProps/Placeable/StaticProps/BP_WitchRuneStone01.BP_WitchRuneStone01_C")))
                throw std::runtime_error("payload carrier class differs");
            witchit::save::copy_carrier_payload(desired[at-1],payload);
        }
    }
    if(at!=source.size())throw std::runtime_error("eligible actor/row count differs");
    return desired;
}
void store_statics(FFrame& frame) {
    std::lock_guard lock(mutex_);
    if(!attached_ || !IsInGameThreadRaw() || save_scopes_.empty()
        || Hook::Internal::CurrentWitchItStoreStaticsFrame()!=&frame)return;
    const auto save=save_scopes_.back();const auto* e=ledger_.find(save);if(!e || e->writer_seen)return;
    auto* parent=frame.PreviousFrame();
    for(int depth=0;parent && depth<64;++depth,parent=parent->PreviousFrame()) {
        if(std::find(save_scopes_.begin(),save_scopes_.end(),key(*parent))==save_scopes_.end())continue;
        if(key(*parent)!=save)return;
        try {
            MetadataSubject subject{save,e->serial,generation_,&frame};
            FScriptArray* original{};auto* sp=metadata_array(frame,L"StaticsToSave",original);
            const auto source=witchit::save::OwnedStatics::snapshot(sp,original);
            const auto desired=metadata_actor_rows(subject,source);
            bool changed=false;unsigned pots{},respawns{},entrances{},exits{};
            for(std::size_t i=0;i<desired.size();++i){
                changed=changed || desired[i].materials!=source[i].materials;
                const auto kind=witchit::save::marker_kind(desired[i].materials);pots+=kind==1;respawns+=kind==2;entrances+=kind==3;exits+=kind==4;
            }
            metadata_current(subject);
            if(!changed){metadata_.erase(save);return;}
            auto* fn=UObjectGlobals::StaticFindObject<UFunction*>(nullptr,nullptr,writer_path);
            if(!fn)throw std::runtime_error("native writer missing");
            auto inputs=witchit::save::inputs(fn);
            if(inputs.size()!=8)throw std::runtime_error("native writer input schema differs");
            auto* destination=CastField<FArrayProperty>(inputs[3]);
            if(!destination)throw std::runtime_error("native writer statics input missing");
            auto owned=std::make_unique<witchit::save::OwnedStatics>(destination,sp,original,source,desired,
                "carrier:"+std::to_string(subject.generation)+":"+std::to_string(subject.serial));
            owned->validate(original);metadata_current(subject);
            metadata_[save].rows=std::move(owned);
            Output::send(STR("[WIR] native carrier copy serial={} cauldrons={} respawns={} entrances={} exits={} rows={}\n"),subject.serial,pots,respawns,entrances,exits,desired.size());
        }catch(const std::exception& error){
            metadata_.erase(save);
            Output::send(STR("[WIR] native carrier metadata unavailable: {}; vanilla save continues\n"),ensure_str(error.what()));
        }catch(...){metadata_.erase(save);Output::send(STR("[WIR] native carrier metadata unavailable; vanilla save continues\n"));}
        return;
    }
}

// Static Blueprint library calls can use ProcessInternal on UE 5.7. Restore
// endpoint bytes at that physical return, after vanilla material initialisation
// has consumed/clamped its material indices. No registry or Lua load event.
void load_carrier_payload(FFrame& frame) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if(!attached_ || !IsInGameThreadRaw() || frame.Node()->GetPathName()!=L"/Game/Blueprints/Match/BFL_MapLayoutLoading.BFL_MapLayoutLoading_C:LoadStatics")return;
        FScriptArray *input{},*output{};
        auto* rowsProperty=metadata_array(frame,L"StaticsToLoad",input);
        auto* actorsProperty=metadata_array(frame,L"LoadedStatics",output);
        auto rows=witchit::save::OwnedStatics::snapshot(rowsProperty,input);
        auto* actorProperty=CastField<FObjectProperty>(actorsProperty->GetInner());
        if(!actorProperty || actorProperty->GetSize()!=sizeof(UObject*))throw std::runtime_error("LoadStatics actor array schema differs");
        FScriptArrayHelper actors(actorsProperty,output);
        if(actors.Num()!=rows.size())throw std::runtime_error("LoadStatics actor/row count differs");
        auto* contextProperty=CastField<FObjectProperty>(property(frame.Node(),L"__WorldContext"));
        if(!contextProperty || contextProperty->GetSize()!=sizeof(UObject*))throw std::runtime_error("LoadStatics world context missing");
        auto* context=*contextProperty->ContainerPtrToValuePtr<UObject*>(frame.Locals());
        if(!context || !UObject::IsReal(context) || !context->GetWorld())throw std::runtime_error("LoadStatics current world missing");
        unsigned restored{};
        for(int i=0;i<actors.Num();++i) {
            const auto& bytes=rows[i].materials;const auto kind=witchit::save::marker_kind(bytes);
            if(kind<3)continue;
            auto* actor=*reinterpret_cast<UObject**>(actors.GetRawPtr(i));
            if(!actor || !UObject::IsReal(actor) || actor->GetWorld()!=context->GetWorld()
                || actor->GetClassPrivate()->GetPathName()!=L"/Game/Blueprints/StaticProps/Placeable/StaticProps/BP_WitchRuneStone01.BP_WitchRuneStone01_C")throw std::runtime_error("LoadStatics endpoint actor/world differs");
            auto* p=CastField<FArrayProperty>(actor->GetClassPrivate()->FindProperty(FName(L"SavedMaterialIndices",FNAME_Find)));
            if(!p || !CastField<FByteProperty>(p->GetInner()))throw std::runtime_error("LoadStatics endpoint byte array missing");
            auto* array=p->ContainerPtrToValuePtr<FScriptArray>(actor);
            FScriptArrayHelper payload(p,array);
            std::vector<std::uint8_t> current;
            if(payload.Num()){auto* data=static_cast<const std::uint8_t*>(array->GetData());current.assign(data,data+payload.Num());}
            if(!witchit::save::restore_endpoint_payload(current,bytes))continue;
            payload.Resize(static_cast<int>(bytes.size()));
            std::memcpy(array->GetData(),bytes.data(),bytes.size());++restored;
        }
        if(restored)Output::send(STR("[WIR] native LoadStatics: restored {} FAPT endpoint payload(s) in current world\n"),restored);
    }catch(const std::exception& error){Output::send(STR("[WIR] native LoadStatics endpoint restore failed: {}; vanilla load continues\n"),ensure_str(error.what()));}
    catch(...){Output::send(STR("[WIR] native LoadStatics endpoint restore failed; vanilla load continues\n"));}
}
