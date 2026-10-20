#pragma once

#include <LuaHost/Lifecycle.hpp>
#include <LuaHost/UIKitLuaHost.h>
#include <cstdint>

struct lua_State;

namespace RC::LuaHost {

// HOST INTERNAL ONLY. The actual stack comes from the trusted LuaMadeSimple
// callback/coroutine entry. Never construct this with a consumer-supplied state,
// a VM root substituted for the callback stack, or a state after lua_close.
// The ScopeLease must outlive this pin. The pin must outlive all consumer code
// using its context and all borrowed strings read in that scoped entry.
class LuaValueSerialization {
    friend class LuaValueBackend;
    friend class LuaObjectBackend;
    Lifecycle& lifecycle_;
    const ScopeLease& scope_;
    lua_State* stack_{};
    const void* frame_{};
    ue4ss_lua_host_status status_{UE4SS_LUA_HOST_INVALID_HANDLE};
    bool locked_{};
public:
    LuaValueSerialization(Lifecycle&, const ScopeLease&, lua_State* verified_actual_stack) noexcept;
    ~LuaValueSerialization();
    LuaValueSerialization(const LuaValueSerialization&) = delete;
    LuaValueSerialization& operator=(const LuaValueSerialization&) = delete;
    ue4ss_lua_host_status status() const noexcept { return status_; }
    bool binds_actual_stack(lua_State* actual) const noexcept { return stack_ == actual; }
};

// No resolver is implemented here. HostApiRuntime's real TLS entry must create
// this view only after checking its owner binding and actual callback stack.
// The optional closure mapping is published by the HOST trampoline producer:
// first_consumer_upvalue is a 1-based physical C-closure index; count excludes
// host-private bookkeeping. Identity is checked against the active C closure.
struct LuaValueContext {
    const ScopeLease& scope;
    const LuaValueSerialization& serialization;
    int (*verified_trampoline)(lua_State*){};
    std::uint32_t first_consumer_upvalue{1};
    std::uint32_t consumer_upvalue_count{};
};

// Real embedded-Lua operations, with no exports, second Lua, per-VM mutex,
// callback resolver, token registry, UObject extraction, or reference routing.
// The incomplete ABI tail MUST NOT advertise CAP_VALUES_V1 yet.
class LuaValueBackend {
    friend class LuaObjectBackend;
    Lifecycle& lifecycle_;
    ue4ss_lua_host_status admit(const LuaValueContext&, ue4ss_lua_context_handle) const;
public:
    explicit LuaValueBackend(Lifecycle& lifecycle) : lifecycle_(lifecycle) {}
    ue4ss_lua_host_status stack_top(const LuaValueContext&, ue4ss_lua_context_handle, std::int32_t*) noexcept;
    ue4ss_lua_host_status stack_set_top(const LuaValueContext&, ue4ss_lua_context_handle, std::int32_t) noexcept;
    ue4ss_lua_host_status value_read(const LuaValueContext&, ue4ss_lua_context_handle, std::int32_t, ue4ss_lua_value_v1*) noexcept;
    ue4ss_lua_host_status upvalue_read(const LuaValueContext&, ue4ss_lua_context_handle, std::uint32_t, ue4ss_lua_value_v1*) noexcept;
    ue4ss_lua_host_status value_push(const LuaValueContext&, ue4ss_lua_context_handle, const ue4ss_lua_value_v1*) noexcept;
    ue4ss_lua_host_status table_set_field(const LuaValueContext&, ue4ss_lua_context_handle, std::int32_t, const char*, std::uint64_t) noexcept;
    ue4ss_lua_host_status protected_call(const LuaValueContext&, ue4ss_lua_context_handle, std::int32_t nargs, std::int32_t nresults) noexcept;
};
}
