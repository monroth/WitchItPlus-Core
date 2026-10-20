#pragma once
#include <LuaMadeSimple/Common.hpp>
#include <cstddef>
struct lua_State;

namespace RC::LuaMadeSimple {
/* Exact compiler-instantiated storage type identity, not a registry name or
 * guessed vtable/layout. No RTTI requirement. Only host-owned SDK emissions may
 * establish provenance; a consumer-provided address is never authority. */
template<class T> const void* host_userdata_type() noexcept {
    // Writable distinct objects avoid read-only COMDAT/data folding on MSVC.
    static unsigned char identity{};return &identity;
}
struct HostUserdataEmission {
    lua_State* actual_stack{};
    void* storage{};
    const void* storage_type{};
    std::size_t size{},alignment{};
    int stack_index{};
    const char* metatable_name{};
};
struct HostUserdataObserver {
    void (*completed)(const HostUserdataEmission&) noexcept{};
    void (*destroying)(lua_State*,void*,const void*) noexcept{};
    void (*finalizer)(lua_State*,const void*,int (*)(lua_State*)) noexcept{};
};
/* Trusted HOST installation only. The observer and its callbacks remain alive
 * through all Lua VM finalization; no consumer callback can be installed here.
 * Installation/clear occurs at quiescent host boundaries. Completed may capture
 * metadata through POD protected C frames; destroying performs native catalogue
 * removal only, never Lua/consumer work. No observer means no provenance claim. */
RC_LMS_API void set_host_userdata_observer(const HostUserdataObserver*) noexcept;
RC_LMS_API void notify_host_userdata_completed(const HostUserdataEmission&) noexcept;
RC_LMS_API void notify_host_userdata_destroying(lua_State*,void*,const void*) noexcept;
RC_LMS_API void notify_host_userdata_finalizer(lua_State*,const void*,int (*)(lua_State*)) noexcept;
}
