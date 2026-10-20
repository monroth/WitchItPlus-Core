#pragma once
#include "SaveRow.hpp"
#include "NativeFunctionParameters.hpp"
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/Core/HAL/UnrealMemory.hpp>
#include <windows.h>
#include <set>
#include <memory>
#include <algorithm>
#include <cmath>

namespace witchit::save {
using namespace RC::Unreal;
inline std::string narrow(const RC::StringType& value) {
    if(value.size()>1024)throw std::runtime_error("save asset text too long");
    const auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    if(!n)throw std::runtime_error("save asset UTF8 conversion failed");
    std::string s(n,'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),s.data(),n,nullptr,nullptr)!=n)throw std::runtime_error("save asset UTF8 conversion failed");
    return s;
}
inline FProperty* field(UStruct* type,const wchar_t* name) {
    if(!type)throw std::runtime_error("save typed struct missing");
    auto* p=type->FindProperty(FName(name,FNAME_Find));
    if(!p || p->GetArrayDim()!=1 || p->GetSize()<=0 || p->GetMinAlignment()<=0 || p->GetMinAlignment()>256 || p->GetOffset_Internal()<0
        || std::uint64_t(p->GetOffset_Internal())+p->GetSize()>std::uint64_t(type->GetPropertiesSize()))throw std::runtime_error("save typed field missing/out of bounds");
    return p;
}
inline std::vector<FProperty*> inputs(UFunction* fn) {
    std::vector<FProperty*> v;
    for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated))
        if(p->HasAnyPropertyFlags(CPF_Parm) && !p->HasAnyPropertyFlags(CPF_ReturnParm))v.push_back(p);
    return v;
}
struct ArrayRange {std::uintptr_t descriptor{},data{};std::size_t allocation{};int num{},stride{};std::string owner,id;};
inline bool intersects(std::uintptr_t a,std::size_t n,std::uintptr_t b,std::size_t m) {
    if(n>UINTPTR_MAX-a || m>UINTPTR_MAX-b)throw std::runtime_error("save range overflow");
    return n && m && a<b+m && b<a+n;
}
inline bool aliases(const ArrayRange& a,const ArrayRange& b) {
    return intersects(a.descriptor,sizeof(FScriptArray),b.descriptor,sizeof(FScriptArray))
        || intersects(a.data,a.allocation,b.data,b.allocation)
        || intersects(a.descriptor,sizeof(FScriptArray),b.data,b.allocation)
        || intersects(b.descriptor,sizeof(FScriptArray),a.data,a.allocation);
}
inline ArrayRange range(FArrayProperty* p,FScriptArray* a,std::string owner) {
    static_assert(sizeof(FScriptArray)==16,"matched Win64 array descriptor required");
    if(!p || !a || !p->GetInner())throw std::runtime_error("captured array schema missing");
    const auto stride=p->GetInner()->GetElementSize();const auto n=a->NumUnchecked(),max=a->Max();
    if(stride<=0 || stride>1048576 || n<0 || n>200000 || max<n || max>200000 || (max && !a->GetData()))throw std::runtime_error("invalid captured array bounds");
    std::size_t allocation{};
    if(a->GetData() && (!GMalloc || !*GMalloc || !(*GMalloc)->GetAllocationSize(a->GetData(),allocation)
        || allocation<std::size_t(max)*stride || allocation>256*1024*1024))throw std::runtime_error("native allocation extent unavailable");
    const auto descriptor=reinterpret_cast<std::uintptr_t>(a),data=reinterpret_cast<std::uintptr_t>(a->GetData());
    if(descriptor>9007199254740991ULL-sizeof(FScriptArray) || data>9007199254740991ULL-allocation)throw std::runtime_error("diagnostic address not exactly representable");
    return {descriptor,data,allocation,n,stride,owner,owner+":"+std::to_string(descriptor)+":"+std::to_string(data)};
}
using GetterParameters=witchit::native::Parameters;
inline std::pair<std::string,std::string> primary_id(UScriptStruct* type,void* value) {
    auto* name=CastField<FNameProperty>(field(type,L"PrimaryAssetName"));auto* asset=CastField<FStructProperty>(field(type,L"PrimaryAssetType"));
    if(!name || !asset || !asset->GetStruct())throw std::runtime_error("primary asset schema differs");
    auto* typeName=CastField<FNameProperty>(field(asset->GetStruct(),L"Name"));if(!typeName)throw std::runtime_error("primary asset type name missing");
    return {narrow(typeName->GetPropertyValue(typeName->ContainerPtrToValuePtr<void>(asset->ContainerPtrToValuePtr<void>(value))).ToString()),
        narrow(name->GetPropertyValue(name->ContainerPtrToValuePtr<void>(value)).ToString())};
}
inline std::array<double,3> coordinates(UScriptStruct* type,void* value,bool rotation) {
    std::array<double,3> result{};std::size_t i{};
    for(auto* key:rotation?std::array{L"Pitch",L"Yaw",L"Roll"}:std::array{L"X",L"Y",L"Z"}){
        auto* p=CastField<FNumericProperty>(field(type,key));if(!p || !p->IsFloatingPoint())throw std::runtime_error("native coordinate schema differs");
        auto n=p->GetFloatingPointPropertyValue(p->ContainerPtrToValuePtr<void>(value));if(!std::isfinite(n))throw std::runtime_error("nonfinite native coordinate");result[i++]=n;
    }
    return result;
}
class OwnedArray final {
    FArrayProperty* property_{};FArrayProperty* bytes_{};void* data_{};
    std::vector<ArrayRange> borrowed_;
    bool initialized_{};
    bool overlaps_borrowed(const void* pointer,std::size_t size) const {
        const auto address=reinterpret_cast<std::uintptr_t>(pointer);
        for(const auto& r:borrowed_)if(intersects(address,size,r.data,r.allocation) || intersects(address,size,r.descriptor,sizeof(FScriptArray)))return true;
        return false;
    }
    // A bad deep-copy implementation must never free a borrowed child while
    // rejecting it. Reset ONLY our own descriptor, without freeing its data.
    void detach_borrowed() noexcept {
        if(!initialized_ || !data_)return;
        auto* a=data();
        try {
            const auto count=a->NumUnchecked(),max=a->Max();const auto stride=property_->GetInner()->GetElementSize();
            if(count<0 || max<count || max>200000 || stride<=0 || overlaps_borrowed(a->GetData(),std::size_t(max)*stride)){
                std::construct_at(a);return;
            }
            if(!count)return;
            std::size_t size{};
            if(!a->GetData() || !GMalloc || !*GMalloc || !(*GMalloc)->GetAllocationSize(a->GetData(),size) || size<std::size_t(count)*stride){std::construct_at(a);return;}
            for(int i=0;i<count;++i){
                auto* r=static_cast<unsigned char*>(a->GetData())+std::size_t(i)*stride;
                auto* bytes=bytes_->ContainerPtrToValuePtr<FScriptArray>(r);
                const auto n=bytes->Max();
                if(n<0 || n>33000 || overlaps_borrowed(bytes->GetData(),std::size_t((std::max)(n,0))))std::construct_at(bytes);
            }
        }catch(...){std::construct_at(a);} // unknown clone storage remains quarantined
    }
public:
    OwnedArray(FArrayProperty* p,FArrayProperty* bytes,const void* source,const std::vector<ArrayRange>& borrowed)
        :property_(p),bytes_(bytes),borrowed_(borrowed) {
        data_=FMemory::Malloc(p->GetSize(),p->GetMinAlignment());
        if(!data_)throw std::bad_alloc();
        try {p->InitializeValue(data_);initialized_=true;p->CopyCompleteValue(data_,source);}
        catch(...){detach_borrowed();if(initialized_)p->DestroyValue(data_);FMemory::Free(data_);data_=nullptr;throw;}
    }
    ~OwnedArray(){if(data_){detach_borrowed();if(initialized_)property_->DestroyValue(data_);FMemory::Free(data_);}}
    OwnedArray(const OwnedArray&)=delete;
    FScriptArray* data() const{return static_cast<FScriptArray*>(data_);}
};
class OwnedStatics final {
    OwnedStatics()=default;
    FArrayProperty* property_{};FStructProperty* row_{};FArrayProperty* bytes_{};
    std::unique_ptr<OwnedArray> source_,owned_;
    std::vector<SaveRow> desired_;
    std::string owner_;
    void* row_at(FScriptArray* a,int i) const{return static_cast<unsigned char*>(a->GetData())+std::size_t(i)*row_->GetElementSize();}
    FScriptArray* bytes_at(void* r) const{return bytes_->ContainerPtrToValuePtr<FScriptArray>(r);}
    static double number(UStruct* type,void* container,const wchar_t* name) {
        auto* p=CastField<FNumericProperty>(field(type,name));
        if(!p || !p->IsFloatingPoint())throw std::runtime_error("save transform schema differs");
        auto v=p->GetFloatingPointPropertyValue(p->ContainerPtrToValuePtr<void>(container));
        if(!std::isfinite(v))throw std::runtime_error("save transform is nonfinite");return v;
    }
    SaveRow project(void* r) const {
        UScriptStruct* type=row_->GetStruct();SaveRow out;
        auto* primary=CastField<FStructProperty>(field(type,L"PrimaryAssetId"));
        if(!primary)throw std::runtime_error("save primary asset schema differs");
        UScriptStruct* primaryType=primary->GetStruct();auto* primaryData=primary->ContainerPtrToValuePtr<void>(r);
        auto* assetName=CastField<FNameProperty>(field(primaryType,L"PrimaryAssetName"));
        auto* assetType=CastField<FStructProperty>(field(primaryType,L"PrimaryAssetType"));
        if(!assetName || !assetType)throw std::runtime_error("save asset schema differs");
        auto* typeName=CastField<FNameProperty>(field(assetType->GetStruct(),L"Name"));
        if(!typeName)throw std::runtime_error("save asset type schema differs");
        out.asset_name=narrow(assetName->GetPropertyValue(assetName->ContainerPtrToValuePtr<void>(primaryData)).ToString());
        out.asset_type=narrow(typeName->GetPropertyValue(typeName->ContainerPtrToValuePtr<void>(assetType->ContainerPtrToValuePtr<void>(primaryData))).ToString());
        auto* id=CastField<FIntProperty>(field(type,L"Id"));auto* active=CastField<FBoolProperty>(field(type,L"Activated"));
        if(!id || !active)throw std::runtime_error("save ID/activation schema differs");
        out.id=id->GetPropertyValue(id->ContainerPtrToValuePtr<void>(r));out.activated=active->GetPropertyValue(active->ContainerPtrToValuePtr<void>(r));
        int at{};
        for(auto [name,rotation]:{std::pair{L"Location",false},std::pair{L"Rotation",true},std::pair{L"Scale",false}}) {
            auto* p=CastField<FStructProperty>(field(type,name));if(!p)throw std::runtime_error("save transform field differs");
            auto* v=p->ContainerPtrToValuePtr<void>(r);
            for(auto* key:rotation?std::array{L"Pitch",L"Yaw",L"Roll"}:std::array{L"X",L"Y",L"Z"})out.transform[at++]=number(p->GetStruct(),v,key);
        }
        auto* a=bytes_at(r);auto bounds=range(bytes_,a,owner_);
        if(bounds.num>33000)throw std::runtime_error("save metadata bound exceeded");
        auto* data=static_cast<const std::uint8_t*>(a->GetData());if(bounds.num)out.materials.assign(data,data+bounds.num);
        return out;
    }
    static bool same_fields(const SaveRow& a,const SaveRow& b) {
        return a.asset_type==b.asset_type && a.asset_name==b.asset_name && a.id==b.id && a.activated==b.activated && a.transform==b.transform;
    }
    std::vector<ArrayRange> ranges(FScriptArray* a,const std::string& owner) const {
        std::vector<ArrayRange> out;out.push_back(range(property_,a,owner));
        for(int i=0;i<a->NumUnchecked();++i)out.push_back(range(bytes_,bytes_at(row_at(a,i)),owner));
        return out;
    }
public:
    static std::vector<SaveRow> snapshot(FArrayProperty* property,FScriptArray* array) {
        OwnedStatics reader;reader.property_=property;reader.owner_="native-snapshot";
        reader.row_=property?CastField<FStructProperty>(property->GetInner()):nullptr;
        if(!reader.row_ || !reader.row_->GetStruct() || reader.row_->GetStruct()->GetPathName()!=L"/Script/PropWitchHuntModule.PlaceableStaticToSave")
            throw std::runtime_error("native statics schema differs");
        reader.bytes_=CastField<FArrayProperty>(field(reader.row_->GetStruct(),L"MaterialIndices"));
        if(!reader.bytes_ || !CastField<FByteProperty>(reader.bytes_->GetInner()))throw std::runtime_error("native metadata bytes missing");
        const auto bounds=range(property,array,reader.owner_);std::vector<SaveRow> rows;rows.reserve(bounds.num);
        for(int i=0;i<bounds.num;++i)rows.push_back(reader.project(reader.row_at(array,i)));
        return rows;
    }
    OwnedStatics(FArrayProperty* destination,FArrayProperty* sourceProperty,FScriptArray* original,
                 const std::vector<SaveRow>& source,const std::vector<SaveRow>& desired,std::string owner)
        :property_(destination),owner_(std::move(owner)) {
        row_=CastField<FStructProperty>(property_->GetInner());auto* sourceRow=CastField<FStructProperty>(sourceProperty->GetInner());
        if(!row_ || !sourceRow || !row_->GetStruct() || !sourceRow->GetStruct() || row_->GetStruct()!=sourceRow->GetStruct() || row_->GetStruct()->GetPathName()!=L"/Script/PropWitchHuntModule.PlaceableStaticToSave")throw std::runtime_error("unknown native statics schema");
        bytes_=CastField<FArrayProperty>(field(row_->GetStruct(),L"MaterialIndices"));
        if(!bytes_ || !CastField<FByteProperty>(bytes_->GetInner()) || bytes_->GetInner()->GetElementSize()!=1)throw std::runtime_error("metadata is not a byte array");
        auto oldRanges=ranges(original,"borrowed:"+owner_);
        if(source.size()!=std::size_t(oldRanges[0].num) || desired.size()!=source.size())throw std::runtime_error("native/source/desired row count differs");
        std::set<std::int32_t> ids;
        for(std::size_t i=0;i<source.size();++i){
            auto actual=project(row_at(original,int(i)));
            if(!same_fields(actual,source[i]) || actual.materials!=source[i].materials || !same_fields(actual,desired[i]) || !ids.insert(actual.id).second)throw std::runtime_error("native typed source differs");
            for(std::size_t j=1;j<i+1;++j)if(aliases(oldRanges[i+1],oldRanges[j]))throw std::runtime_error("source metadata aliases another row");
        }
        // Real reflected deep copies retain all native fields, including fields
        // outside the Lua projection. No original descriptor is ever replaced.
        source_=std::make_unique<OwnedArray>(property_,bytes_,original,oldRanges);
        auto sourceRanges=ranges(source_->data(),"snapshot:"+owner_);
        for(const auto& a:sourceRanges)for(const auto& b:oldRanges)if(aliases(a,b))throw std::runtime_error("source snapshot aliases borrowed storage");
        auto borrowed=oldRanges;borrowed.insert(borrowed.end(),sourceRanges.begin(),sourceRanges.end());
        owned_=std::make_unique<OwnedArray>(property_,bytes_,source_->data(),borrowed);
        auto ownedRanges=ranges(owned_->data(),owner_);
        for(const auto& a:ownedRanges)for(const auto& b:borrowed)if(aliases(a,b))throw std::runtime_error("owned deep copy aliases source before resize");
        desired_=desired;
        for(std::size_t i=0;i<desired.size();++i){
            FScriptArrayHelper helper(bytes_,bytes_at(row_at(owned_->data(),int(i))));
            helper.EmptyAndAddValues(int(desired[i].materials.size()));
            if(!desired[i].materials.empty())std::copy(desired[i].materials.begin(),desired[i].materials.end(),helper.GetRawPtr());
        }
        validate(original);
    }
    void validate(FScriptArray* original) const {
        if(!property_->Identical(source_->data(),original))throw std::runtime_error("source changed during owned preparation");
        auto a=ranges(owned_->data(),owner_),b=ranges(source_->data(),"snapshot:"+owner_),c=ranges(original,"borrowed:"+owner_);
        if(a[0].num!=int(desired_.size()))throw std::runtime_error("owned row count differs");
        for(std::size_t i=0;i<a.size();++i){
            for(auto* other:{&b,&c})for(const auto& r:*other)if(aliases(a[i],r))throw std::runtime_error("owned allocation aliases source");
            for(std::size_t j=0;j<i;++j){
                // Byte descriptors are contained within owned parent row data;
                // child DATA must nevertheless be separate from parent storage.
                if(j==0){if(intersects(a[i].data,a[i].allocation,a[0].data,a[0].allocation))throw std::runtime_error("child data aliases owned row storage");}
                else if(aliases(a[i],a[j]))throw std::runtime_error("owned children alias");
            }
        }
        for(std::size_t i=0;i<desired_.size();++i){
            auto* owned=row_at(owned_->data(),int(i));auto* captured=row_at(source_->data(),int(i));
            auto actual=project(owned);
            if(!same_fields(actual,desired_[i]) || actual.materials!=desired_[i].materials)throw std::runtime_error("owned payload readback differs");
            for(auto* p:TFieldRange<FProperty>(row_->GetStruct(),EFieldIterationFlags::IncludeDeprecated))
                if(p!=bytes_ && !p->Identical(p->ContainerPtrToValuePtr<void>(owned),p->ContainerPtrToValuePtr<void>(captured)))throw std::runtime_error("unknown native field changed");
        }
    }
    FScriptArray* source() const{return source_->data();}
    FScriptArray* owned() const{return owned_->data();}
};
}
