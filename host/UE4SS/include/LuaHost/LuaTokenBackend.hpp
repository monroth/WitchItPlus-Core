#pragma once
#include <LuaHost/LuaValueBackend.hpp>
#include <cstddef>
#include <mutex>
#include <unordered_map>
namespace RC::LuaHost {
// Host internal: actual must be the same verified stack as view.serialization.
// Token game bridge/owner/id/kind are separate from host VM/module generations.
class LuaTokenBackend {
    struct Record { VmHandle Vm; ModuleHandle Module; const void* Address{}; ue4ss_lua_token_v1 Token; };
    Lifecycle& lifecycle_; LuaValueBackend& values_;
    mutable std::mutex mutex_; std::unordered_map<std::uint64_t, Record> records_;
    std::uint64_t next_{1};
    static void collected(void*, std::uint64_t, const void*) noexcept;
    ue4ss_lua_host_status read_impl(const LuaValueContext&, ue4ss_lua_context_handle,
                                   lua_State*, std::int32_t, bool, ue4ss_lua_value_v1*) noexcept;
public:
    LuaTokenBackend(Lifecycle& life, LuaValueBackend& values) : lifecycle_(life), values_(values) {}
    ue4ss_lua_host_status push(const LuaValueContext&, ue4ss_lua_context_handle, lua_State*, const ue4ss_lua_token_v1&) noexcept;
    ue4ss_lua_host_status read(const LuaValueContext&, ue4ss_lua_context_handle, lua_State*, std::int32_t, ue4ss_lua_value_v1*) noexcept;
    ue4ss_lua_host_status read_upvalue(const LuaValueContext&, ue4ss_lua_context_handle, lua_State*, std::uint32_t, ue4ss_lua_value_v1*) noexcept;
    Status cleanup_vm(VmHandle); Status cleanup_module(ModuleHandle);
    std::size_t size() const;
};
}
