#pragma once
// Exact-frame scalar delivery on the HOST's active callback stack. No cached
// Lua wrapper/root, raw Lua symbol, VM construction or caller-selected context.
#include "UIKitLuaHost.h"
#include "OperationLedger.hpp"
#include "LoadLedger.hpp"
#include <utility>
#include <cstdint>

namespace witchit {
class NativeBoundarySinks final {
    using API=ue4ss_uikit_lua_host_api_v1;
    const API* api_{};
    ue4ss_lua_vm_handle vm_{};
    ue4ss_lua_module_handle module_{};
    ue4ss_lua_reference_handle save_{},load_{};
    std::uint64_t revision_{};
    template<class T> static bool same(T a,T b) noexcept {
        return a.id==b.id && a.generation==b.generation;
    }
    static bool operations(const API* api) noexcept {
        return api && ue4ss_lua_host_validate_prefix(api->abi_version,api->struct_size,sizeof(API))==UE4SS_LUA_HOST_OK
            && api->current_context && api->stack_top && api->stack_set_top
            && api->value_read && api->value_push && api->protected_call
            && api->reference_create && api->reference_push && api->reference_release;
    }
    bool context(ue4ss_lua_host_context_info_v1& info) const noexcept {
        info={UE4SS_UIKIT_LUA_HOST_ABI_VERSION,sizeof(info),{},{},{}};
        return api_ && api_->current_context(&info)==UE4SS_LUA_HOST_OK
            && info.context.id && same(vm_,info.vm) && same(module_,info.module);
    }
    static ue4ss_lua_value_v1 value(unsigned kind) noexcept {
        ue4ss_lua_value_v1 v{};v.abi_version=UE4SS_UIKIT_LUA_HOST_ABI_VERSION;v.struct_size=sizeof(v);v.kind=kind;return v;
    }
    static bool push(const API* api,ue4ss_lua_context_handle context,const char* text) noexcept {
        auto v=value(UE4SS_LUA_VALUE_STRING);v.string_data=text;
        if(text)while(text[v.string_size])++v.string_size;
        return api->value_push(context,&v)==UE4SS_LUA_HOST_OK;
    }
    static bool push(const API* api,ue4ss_lua_context_handle context,const std::string& text) noexcept {
        auto v=value(UE4SS_LUA_VALUE_STRING);v.string_data=text.data();v.string_size=text.size();
        return api->value_push(context,&v)==UE4SS_LUA_HOST_OK;
    }
    static bool push(const API* api,ue4ss_lua_context_handle context,bool flag) noexcept {
        auto v=value(UE4SS_LUA_VALUE_BOOLEAN);v.boolean_value=flag;return api->value_push(context,&v)==UE4SS_LUA_HOST_OK;
    }
    static bool push(const API* api,ue4ss_lua_context_handle context,std::uint64_t serial) noexcept {
        if(serial>INT64_MAX)return false;
        auto v=value(UE4SS_LUA_VALUE_INTEGER);v.integer_value=static_cast<std::int64_t>(serial);
        return api->value_push(context,&v)==UE4SS_LUA_HOST_OK;
    }
    template<class Guard,class... Args> bool emit(ue4ss_lua_reference_handle sink,Guard&& guard,Args&&...args) noexcept {
        try {
            ue4ss_lua_host_context_info_v1 info{};
            if(!sink.id || !context(info) || !guard())return false;
            const auto api=api_;const auto revision=revision_;
            std::int32_t top{};if(api->stack_top(info.context,&top)!=UE4SS_LUA_HOST_OK)return false;
            struct Restore {const API* api;ue4ss_lua_context_handle context;std::int32_t top;
                ~Restore(){(void)api->stack_set_top(context,top);}} restore{api,info.context,top};
            if(api->reference_push(info.context,sink)!=UE4SS_LUA_HOST_OK)return false;
            if(!(push(api,info.context,std::forward<Args>(args)) && ...))return false;
            if(api->protected_call(info.context,sizeof...(Args),1)!=UE4SS_LUA_HOST_OK)return false;
            auto result=value(UE4SS_LUA_VALUE_NIL);
            return api->value_read(info.context,-1,&result)==UE4SS_LUA_HOST_OK
                && result.kind==UE4SS_LUA_VALUE_BOOLEAN && result.boolean_value
                && api_==api && revision_==revision && context(info) && guard();
        } catch(...) {return false;}
    }
public:
    // NativeProducer must pass only the capability-validated matched host API.
    // Diagnostics may exercise this scalar implementation against the actual
    // host while CAP_VALUES is0; they do not activate a producer or UIKit.
    bool attach(const API* api) noexcept {
        if(!operations(api))return false;
        ue4ss_lua_host_context_info_v1 info{UE4SS_UIKIT_LUA_HOST_ABI_VERSION,sizeof(info),{},{},{}};
        if(api->current_context(&info)!=UE4SS_LUA_HOST_OK || !info.context.id || !info.vm.id || !info.module.id)return false;
        std::int32_t top{};if(api->stack_top(info.context,&top)!=UE4SS_LUA_HOST_OK || top!=2)return false;
        for(int at=1;at<=2;++at){auto v=value(UE4SS_LUA_VALUE_NIL);
            if(api->value_read(info.context,at,&v)!=UE4SS_LUA_HOST_OK || v.kind!=UE4SS_LUA_VALUE_FUNCTION)return false;}
        ue4ss_lua_reference_handle save{},load{};
        if(api->reference_create(info.context,1,&save)!=UE4SS_LUA_HOST_OK)return false;
        if(api->reference_create(info.context,2,&load)!=UE4SS_LUA_HOST_OK){
            (void)api->reference_release(info.context,save);return false;}
        // A foreign VM/module cannot replace this live bridge's callback pair.
        if(api_ && (!same(vm_,info.vm) || !same(module_,info.module))){
            (void)api->reference_release(info.context,save);(void)api->reference_release(info.context,load);return false;}
        stop();api_=api;vm_=info.vm;module_=info.module;save_=save;load_=load;++revision_;
        return true;
    }
    bool bound_current() const noexcept {ue4ss_lua_host_context_info_v1 info{};return save_.id && load_.id && context(info);}
    void stop() noexcept {
        const auto api=api_;ue4ss_lua_host_context_info_v1 info{};
        const bool accessible=context(info);
        const auto save=save_,load=load_;
        // Revoke delivery before releasing references: GC/cleanup cannot
        // observe or invoke the old callback pair through this producer.
        api_=nullptr;vm_={};module_={};save_={};load_={};++revision_;
        if(accessible){(void)api->reference_release(info.context,save);(void)api->reference_release(info.context,load);}
        // Otherwise the host's owner/VM retirement cleanup releases its refs.
    }
    template<class Guard> bool save(const char* phase,const SaveEntry& e,const std::string& target,bool success,Guard&& guard) noexcept {
        return emit(save_,std::forward<Guard>(guard),phase,e.serial,e.owner_key,e.requested_name,
            e.compressed,e.autosave,target,success,e.writer_admitted,e.actual_name);
    }
    template<class Guard> bool load(const char* phase,const LoadEntry& e,Guard&& guard) noexcept {
        return emit(load_,std::forward<Guard>(guard),phase,e.serial,e.owner_key,e.actual_target,
            e.compressed,e.recovery,e.recovery_intent_known,e.reader_done,e.reader_success,e.application_done,
            e.layout_name,e.layout_world,e.layout_path);
    }
    // Same tuple as Save; the tenth scalar is a publication state for this
    // distinct factual phase, and the eleventh is its owned recovery path.
    template<class Guard> bool publication(const SaveEntry& e,const std::string& target,bool success,
        const std::string& state,const std::string& backup,Guard&& guard) noexcept {
        return emit(save_,std::forward<Guard>(guard),"disk_outcome",e.serial,e.owner_key,e.requested_name,
            e.compressed,e.autosave,target,success,e.writer_admitted,state,backup);
    }
};
}
