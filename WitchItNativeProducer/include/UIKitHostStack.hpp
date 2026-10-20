#pragma once
#include "UIKitLuaHost.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace witchit::uikit {
template<class Handle> constexpr bool same_handle(Handle a,Handle b) noexcept {
    return a.id==b.id && a.generation==b.generation;
}
struct HostReference {
    ue4ss_lua_reference_handle handle{};
    explicit operator bool() const noexcept{return handle.id!=0;}
};
class HostFailure final:public std::runtime_error {
public:
    ue4ss_lua_host_status status;
    explicit HostFailure(ue4ss_lua_host_status s):std::runtime_error("Native host operation refused"),status(s){}
};
inline void host_require(ue4ss_lua_host_status s){if(s!=UE4SS_LUA_HOST_OK)throw HostFailure{s};}
inline ue4ss_lua_value_v1 host_value(ue4ss_lua_value_kind kind) noexcept {
    ue4ss_lua_value_v1 result{};
    result.abi_version=UE4SS_UIKIT_LUA_HOST_ABI_VERSION;
    result.struct_size=sizeof(result);result.kind=kind;return result;
}
inline bool host_values_available(const ue4ss_uikit_lua_host_api_v1* api,std::uint64_t required=UE4SS_LUA_HOST_CAP_VALUES_V1) noexcept {
    if(!api || ue4ss_lua_host_validate_prefix(api->abi_version,api->struct_size,
        sizeof(ue4ss_uikit_lua_host_api_v1))!=UE4SS_LUA_HOST_OK)return false;
    if((required!=UE4SS_LUA_HOST_CAP_VALUES_V1 && required!=UE4SS_LUA_HOST_CAP_SCALARS_V1) || !(api->capabilities&required))return false;
    return api->current_context && api->register_callback && api->invoke &&
        api->request_vm_close && api->request_module_retire && api->stack_top &&
        api->stack_set_top && api->value_read && api->upvalue_read && api->value_push &&
        api->table_set_field && api->reference_create && api->reference_push &&
        api->reference_release && api->protected_call && api->callback_push &&
        api->callback_return && (required==UE4SS_LUA_HOST_CAP_SCALARS_V1 || (api->object_read && api->object_push));
}
inline const ue4ss_uikit_lua_host_api_v1* resolve_host_values(std::uint64_t required=UE4SS_LUA_HOST_CAP_VALUES_V1) noexcept {
#if defined(_WIN32)
    auto module=GetModuleHandleW(L"UE4SS.dll");if(!module)return nullptr;
    using Accessor=ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *)(uint32_t,uint32_t,const ue4ss_uikit_lua_host_api_v1**);
    auto accessor=reinterpret_cast<Accessor>(GetProcAddress(module,"UE4SS_GetUIKitLuaHostApi_v1"));
    if(!accessor)return nullptr;
    const ue4ss_uikit_lua_host_api_v1* api{};
    if(accessor(UE4SS_UIKIT_LUA_HOST_ABI_VERSION,sizeof(*api),&api)!=UE4SS_LUA_HOST_OK)return nullptr;
    return host_values_available(api,required)?api:nullptr;
#else
    // Portable ABI tests do not provide a replacement VM or simulated host.
    return nullptr;
#endif
}

/* This object exists only inside one host-authorized entry. It never owns a VM
 * or closes it. The host checks the active entry afresh on EVERY operation. */
class HostStack final {
    const ue4ss_uikit_lua_host_api_v1* api_{};
    ue4ss_lua_host_context_info_v1 info_{};
public:
    explicit HostStack(const ue4ss_uikit_lua_host_api_v1* api,std::uint64_t required=UE4SS_LUA_HOST_CAP_VALUES_V1):api_(api) {
        if(!host_values_available(api_,required))throw HostFailure{UE4SS_LUA_HOST_UNSUPPORTED_SIZE};
        info_.abi_version=UE4SS_UIKIT_LUA_HOST_ABI_VERSION;info_.struct_size=sizeof(info_);
        host_require(api_->current_context(&info_));
        if(!info_.context.id || !info_.vm.id || !info_.module.id)
            throw HostFailure{UE4SS_LUA_HOST_INVALID_HANDLE};
    }
    const auto& info() const noexcept{return info_;}
    const auto* api() const noexcept{return api_;}
    int top() const{int32_t n{};host_require(api_->stack_top(info_.context,&n));return n;}
    void set_top(int n) const{host_require(api_->stack_set_top(info_.context,n));}
    void restore(int n) const noexcept{(void)api_->stack_set_top(info_.context,n);}
    ue4ss_lua_value_v1 read(int at) const {
        auto result=host_value(UE4SS_LUA_VALUE_NIL);
        host_require(api_->value_read(info_.context,at,&result));return result;
    }
    ue4ss_lua_value_v1 upvalue(unsigned at) const {
        auto result=host_value(UE4SS_LUA_VALUE_NIL);
        host_require(api_->upvalue_read(info_.context,at,&result));return result;
    }
    void push(const ue4ss_lua_value_v1& value) const{host_require(api_->value_push(info_.context,&value));}
    void nil() const{push(host_value(UE4SS_LUA_VALUE_NIL));}
    void boolean(bool b) const{auto v=host_value(UE4SS_LUA_VALUE_BOOLEAN);v.boolean_value=b;push(v);}
    void integer(int64_t i) const{auto v=host_value(UE4SS_LUA_VALUE_INTEGER);v.integer_value=i;push(v);}
    void number(double n) const{auto v=host_value(UE4SS_LUA_VALUE_NUMBER);v.number_value=n;push(v);}
    void string(std::string_view s) const{
        auto v=host_value(UE4SS_LUA_VALUE_STRING);v.string_data=s.data();v.string_size=s.size();push(v);
    }
    void table() const{push(host_value(UE4SS_LUA_VALUE_TABLE));}
    void set_field(int at,std::string_view key) const{
        host_require(api_->table_set_field(info_.context,at,key.data(),key.size()));
    }
    bool is_function(int at) const{return read(at).kind==UE4SS_LUA_VALUE_FUNCTION;}
    bool is_boolean(int at) const{return read(at).kind==UE4SS_LUA_VALUE_BOOLEAN;}
    bool get_boolean(int at) const{auto v=read(at);return v.kind==UE4SS_LUA_VALUE_BOOLEAN && v.boolean_value;}
    std::optional<std::string> get_string(int at) const{
        auto v=read(at);if(v.kind!=UE4SS_LUA_VALUE_STRING)return {};
        if(v.string_size>65536 || (!v.string_data && v.string_size))throw HostFailure{UE4SS_LUA_HOST_INTERNAL_ERROR};
        return std::string(v.string_data?v.string_data:"",static_cast<size_t>(v.string_size));
    }
    void token(ue4ss_lua_token_v1 t) const{auto v=host_value(UE4SS_LUA_VALUE_TOKEN);v.token=t;push(v);}
    std::optional<ue4ss_lua_token_v1> get_token(int at) const{
        auto v=read(at);if(v.kind!=UE4SS_LUA_VALUE_TOKEN)return {};return v.token;
    }
    HostReference reference(int at) const{
        HostReference r;host_require(api_->reference_create(info_.context,at,&r.handle));
        if(!r)throw HostFailure{UE4SS_LUA_HOST_INVALID_HANDLE};
        return r;
    }
    HostReference take_reference() const{
        auto r=reference(-1);set_top(top()-1);return r;
    }
    void push_reference(HostReference r) const{
        if(!r){nil();return;}host_require(api_->reference_push(info_.context,r.handle));
    }
    void release(HostReference r) const{
        if(r)host_require(api_->reference_release(info_.context,r.handle));
    }
    bool call(int nargs,int nresults) const{
        auto status=api_->protected_call(info_.context,nargs,nresults);
        if(status==UE4SS_LUA_HOST_OK)return true;
        if(status==UE4SS_LUA_HOST_INTERNAL_ERROR || status==UE4SS_LUA_HOST_YIELD_FORBIDDEN)return false;
        throw HostFailure{status};
    }
    void callback(ue4ss_lua_callback_handle h,unsigned nup) const{
        host_require(api_->callback_push(info_.context,h,nup));
    }
    void* object(int at) const{
        void* result{};auto status=api_->object_read(info_.context,at,&result);
        if(status==UE4SS_LUA_HOST_INVALID_HANDLE)return nullptr;
        host_require(status);return result;
    }
    void object(void* address) const{host_require(api_->object_push(info_.context,address));}
};
}
