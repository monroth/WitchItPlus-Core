#pragma once
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/Core/HAL/UnrealMemory.hpp>
#include <vector>
#include <stdexcept>

namespace witchit::native {
using namespace RC::Unreal;
// Initialized storage for exact, bounded native getter schemas. No Lua
// struct conversion, source frame overwrite, or borrowed OUT descriptor.
class Parameters final {
    void* data_{};std::vector<FProperty*> initialized_;
public:
    explicit Parameters(UFunction* fn,bool native_required=true) {
        if(!fn || (native_required && !fn->HasAnyFunctionFlags(EFunctionFlags::FUNC_Native)) || !fn->GetParmsSize() || fn->GetParmsSize()>4096)throw std::runtime_error("native getter schema unavailable");
        std::size_t field_count{};
        for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm))++field_count;
        // Allocate cleanup tracking before any native value owns resources.
        initialized_.reserve(field_count);
        data_=FMemory::Malloc(fn->GetParmsSize(),16);if(!data_)throw std::bad_alloc();FMemory::Memzero(data_,fn->GetParmsSize());
        try {for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm)){
            if(p->GetArrayDim()!=1 || p->GetOffset_Internal()<0 || p->GetSize()<=0 || p->GetOffset_Internal()+p->GetSize()>fn->GetParmsSize())throw std::runtime_error("getter field outside parameter block");
            p->InitializeValue(value(p));initialized_.push_back(p);
        }}catch(...){clear();throw;}
    }
    ~Parameters(){clear();}
    void clear() noexcept {if(data_){for(auto i=initialized_.rbegin();i!=initialized_.rend();++i)try{(*i)->DestroyValue(value(*i));}catch(...){std::terminate();}FMemory::Free(data_);data_=nullptr;}}
    void* value(FProperty* p) const{return p->ContainerPtrToValuePtr<void>(data_);}
    void* data() const{return data_;}
};
}
