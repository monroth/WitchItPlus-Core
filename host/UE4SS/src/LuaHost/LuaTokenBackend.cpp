#include <LuaHost/LuaTokenBackend.hpp>
#include "LuaTokenFrame.h"
#include "LuaValueFrame.h"
#include <limits>
#include <cstring>
namespace RC::LuaHost {
void LuaTokenBackend::collected(void* domain, std::uint64_t id, const void* address) noexcept {
    auto& b = *static_cast<LuaTokenBackend*>(domain);
    try { std::lock_guard lock{b.mutex_}; auto it = b.records_.find(id);
        if (it != b.records_.end() && it->second.Address == address) b.records_.erase(it);
    } catch (...) {}
}
ue4ss_lua_host_status LuaTokenBackend::push(const LuaValueContext& v, ue4ss_lua_context_handle c,
    lua_State* actual, const ue4ss_lua_token_v1& token) noexcept {
    std::uint64_t id{}; int old = -1;
    try {
        auto s = values_.stack_top(v, c, &old); if (s != UE4SS_LUA_HOST_OK) return s;
        if (!actual || !v.serialization.binds_actual_stack(actual) || uikit_lua_value_top(actual) != old) return UE4SS_LUA_HOST_WRONG_OWNER;
        uikit_lua_token_request r{}; r.operation = UIKIT_TOKEN_PUSH; r.domain = this; r.collected = collected; r.token = token;
        { std::lock_guard lock{mutex_}; if (next_ == std::numeric_limits<std::uint64_t>::max()) return UE4SS_LUA_HOST_INTERNAL_ERROR;
          id = next_++; r.record_id = id; records_.emplace(id, Record{v.scope.vm(), v.scope.module(), {}, token}); }
        s = uikit_lua_token_run(actual, &r) == LUA_OK ? r.status : ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
        std::int32_t top{}; if (s == UE4SS_LUA_HOST_OK) s = values_.stack_top(v, c, &top);
        if (s == UE4SS_LUA_HOST_OK) { std::lock_guard lock{mutex_}; auto it = records_.find(id);
            if (it == records_.end()) s = UE4SS_LUA_HOST_RETIRED; else it->second.Address = r.address; }
        if (s != UE4SS_LUA_HOST_OK) { { std::lock_guard lock{mutex_}; records_.erase(id); } uikit_lua_value_pop_to(actual, old); }
        return s;
    } catch (...) {
        if (id) { std::lock_guard lock{mutex_}; records_.erase(id); }
        if (actual && old >= 0) uikit_lua_value_pop_to(actual, old);
        return UE4SS_LUA_HOST_INTERNAL_ERROR;
    }
}
ue4ss_lua_host_status LuaTokenBackend::read_impl(const LuaValueContext& v, ue4ss_lua_context_handle c,
    lua_State* actual, std::int32_t index, bool upvalue, ue4ss_lua_value_v1* out) noexcept {
    if (!out) return UE4SS_LUA_HOST_INVALID_HANDLE;
    auto s = ue4ss_lua_host_validate_prefix(out->abi_version, out->struct_size, sizeof(*out)); if (s != UE4SS_LUA_HOST_OK) return s;
    try {
        std::int32_t top{}; s = values_.stack_top(v, c, &top); if (s != UE4SS_LUA_HOST_OK) return s;
        if (!actual || !v.serialization.binds_actual_stack(actual) || uikit_lua_value_top(actual) != top) return UE4SS_LUA_HOST_WRONG_OWNER;
        uikit_lua_token_request r{}; r.domain = this; r.collected = collected;
        r.operation = upvalue ? UIKIT_TOKEN_UPVALUE_READ : UIKIT_TOKEN_READ;
        if (!upvalue) { r.index = uikit_lua_value_normalize_index(top, index, 0); if (!r.index) return UE4SS_LUA_HOST_INVALID_HANDLE; }
        else {
            lua_CFunction function{}; unsigned physical{};
            if (index <= 0 || static_cast<std::uint32_t>(index) > v.consumer_upvalue_count || !v.first_consumer_upvalue || !v.verified_trampoline) return UE4SS_LUA_HOST_INVALID_HANDLE;
            if (!uikit_lua_value_current_closure(actual, &function, &physical) || function != v.verified_trampoline) return UE4SS_LUA_HOST_WRONG_OWNER;
            const auto end = std::uint64_t{v.first_consumer_upvalue} + v.consumer_upvalue_count - 1;
            if (end > physical) return UE4SS_LUA_HOST_INVALID_HANDLE;
            r.index = static_cast<int>(std::uint64_t{v.first_consumer_upvalue} + index - 1);
        }
        if (uikit_lua_token_run(actual, &r) != LUA_OK) return UE4SS_LUA_HOST_INTERNAL_ERROR;
        if (r.status != UE4SS_LUA_HOST_OK) return r.status;
        s = values_.stack_top(v, c, &top); if (s != UE4SS_LUA_HOST_OK) return s;
        ue4ss_lua_value_v1 result{}; result.abi_version = 1; result.struct_size = sizeof(result); result.kind = UE4SS_LUA_VALUE_OTHER;
        if (r.recognized) { std::lock_guard lock{mutex_}; auto it = records_.find(r.record_id);
            if (it != records_.end() && it->second.Address == r.address) {
                if (it->second.Vm != v.scope.vm() || it->second.Module != v.scope.module()) return UE4SS_LUA_HOST_WRONG_OWNER;
                if (std::memcmp(&it->second.Token, &r.token, sizeof(r.token))) return UE4SS_LUA_HOST_INVALID_HANDLE;
                result.kind = UE4SS_LUA_VALUE_TOKEN; result.token = it->second.Token;
            }
        } *out = result; return UE4SS_LUA_HOST_OK;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status LuaTokenBackend::read(const LuaValueContext& v, ue4ss_lua_context_handle c, lua_State* a, std::int32_t i, ue4ss_lua_value_v1* o) noexcept { return read_impl(v,c,a,i,false,o); }
ue4ss_lua_host_status LuaTokenBackend::read_upvalue(const LuaValueContext& v, ue4ss_lua_context_handle c, lua_State* a, std::uint32_t i, ue4ss_lua_value_v1* o) noexcept {
    if (i > std::numeric_limits<std::int32_t>::max()) return UE4SS_LUA_HOST_INVALID_HANDLE;
    return read_impl(v,c,a,static_cast<std::int32_t>(i),true,o);
}
Status LuaTokenBackend::cleanup_vm(VmHandle vm) {
    auto s = lifecycle_.validate_vm_finalizer(vm); if (s != Status::ok) return s;
    std::lock_guard lock{mutex_}; std::erase_if(records_, [&](const auto& p) { return p.second.Vm == vm; }); return Status::ok;
}
Status LuaTokenBackend::cleanup_module(ModuleHandle m) {
    auto s = lifecycle_.validate_module_finalizer(m); if (s != Status::ok) return s;
    std::lock_guard lock{mutex_}; std::erase_if(records_, [&](const auto& p) { return p.second.Module == m; }); return Status::ok;
}
std::size_t LuaTokenBackend::size() const { std::lock_guard lock{mutex_}; return records_.size(); }
}
