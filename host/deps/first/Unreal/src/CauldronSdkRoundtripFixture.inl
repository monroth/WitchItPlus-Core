// Compile-only here. Enabled solely for a future explicitly authorized diagnostic
// build. Runs inside the REAL writer transaction after its REAL native reader;
// no fixture allocator, serializer, Lua array or invented UFunction is supplied.
struct CauldronSdkRoundtripFacts {
    std::size_t arguments{},arrayArguments{},readerOutputs{},cauldrons{},respawns{},grownEmpty{},grownShort{};
};
inline CauldronSdkRoundtripFacts cauldron_sdk_roundtrip_fixture(
    const std::vector<FProperty*>& writer,const std::vector<FProperty*>& reader,
    const WriterParameters& source,const WriterParameters& prepared,const WriterParameters& returned) {
    if(writer.size()!=8 || reader.size()!=8)throw std::runtime_error("SDK roundtrip fixture requires all eight arguments");
    CauldronSdkRoundtripFacts facts;facts.arguments=writer.size();
    for(std::size_t i=1;i<writer.size();++i) {
        if(!writer[i]->SameType(reader[i]) || !writer[i]->Identical(prepared.value(writer[i]),returned.value(reader[i])))
            throw std::runtime_error("SDK roundtrip fixture output differs");
        ++facts.readerOutputs;
    }
    auto allocation=[](const void* data,std::size_t required) {
        std::size_t size{};
        if(required && (!data || !GMalloc || !*GMalloc || !(*GMalloc)->GetAllocationSize(const_cast<void*>(data),size) || size<required))
            throw std::runtime_error("SDK fixture allocation extent unavailable");
        return required?size:std::size_t{};
    };
    auto separate=[](const void* a,std::size_t n,const void* b,std::size_t m) {
        const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
        if(n>UINTPTR_MAX-x || m>UINTPTR_MAX-y || (n && m && x<y+m && y<x+n))
            throw std::runtime_error("SDK cloned allocations overlap");
    };
    auto* filename=CastField<FStrProperty>(writer[0]);if(!filename)throw std::runtime_error("SDK fixture filename schema differs");
    const auto& oldText=static_cast<const FString*>(source.value(filename))->GetCharArray();
    const auto& newText=static_cast<const FString*>(prepared.value(filename))->GetCharArray();
    separate(oldText.GetData(),allocation(oldText.GetData(),oldText.Num()*sizeof(TCHAR)),
        newText.GetData(),allocation(newText.GetData(),newText.Num()*sizeof(TCHAR)));
    for(std::size_t i=0;i<writer.size();++i)if(auto* array=CastField<FArrayProperty>(writer[i])) {
        ++facts.arrayArguments;
        auto* old=static_cast<FScriptArray*>(source.value(array));auto* copy=static_cast<FScriptArray*>(prepared.value(array));
        const auto stride=array->GetInner()->GetElementSize();
        if(stride<=0 || old->NumUnchecked()<0 || copy->NumUnchecked()<0 || old->Max()<old->NumUnchecked() || copy->Max()<copy->NumUnchecked()
            || old->Max()>200000 || copy->Max()>200000)throw std::runtime_error("SDK fixture array bounds differ");
        separate(old->GetData(),allocation(old->GetData(),std::size_t(old->Max())*stride),
            copy->GetData(),allocation(copy->GetData(),std::size_t(copy->Max())*stride));
    }
    auto* array=CastField<FArrayProperty>(writer[3]);auto* row=array?CastField<FStructProperty>(array->GetInner()):nullptr;
    if(!row || !row->GetStruct() || row->GetStruct()->GetPathName()!=STR("/Script/PropWitchHuntModule.PlaceableStaticToSave"))
        throw std::runtime_error("SDK fixture actual cauldron row schema missing");
    auto* bytes=CastField<FArrayProperty>(row->GetStruct()->FindProperty(FName(STR("MaterialIndices"),FNAME_Find)));
    if(!bytes || !CastField<FByteProperty>(bytes->GetInner()) || bytes->GetInner()->GetElementSize()!=1)
        throw std::runtime_error("SDK fixture metadata is not bytes");
    FScriptArrayHelper before(array,source.value(array)),after(array,prepared.value(array));
    if(before.Num()!=after.Num())throw std::runtime_error("SDK fixture cauldron count differs");
    for(int i=0;i<after.Num();++i) {
        auto* old=bytes->ContainerPtrToValuePtr<FScriptArray>(before.GetRawPtr(i));
        auto* copy=bytes->ContainerPtrToValuePtr<FScriptArray>(after.GetRawPtr(i));
        if(old->NumUnchecked()<0 || old->Max()<old->NumUnchecked() || old->Max()>33000
            || copy->NumUnchecked()<0 || copy->Max()<copy->NumUnchecked() || copy->Max()>33000)
            throw std::runtime_error("SDK fixture metadata bound differs");
        separate(old->GetData(),allocation(old->GetData(),old->Max()),copy->GetData(),allocation(copy->GetData(),copy->Max()));
        std::vector<std::uint8_t> payload;
        if(copy->NumUnchecked()) {
            const auto* data=static_cast<const std::uint8_t*>(copy->GetData());payload.assign(data,data+copy->NumUnchecked());
        }
        const auto kind=witchit::save::marker_kind(payload);
        facts.cauldrons+=kind==1;facts.respawns+=kind==2;
        if(kind && copy->NumUnchecked()>old->NumUnchecked()) {
            facts.grownEmpty+=old->NumUnchecked()==0;facts.grownShort+=old->NumUnchecked()>0;
        }
    }
    return facts;
}
