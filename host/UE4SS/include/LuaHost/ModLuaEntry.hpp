#pragma once
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
namespace RC::LuaHost {
// One ingress adapter for the complete host operation, including argument and
// registry work before pcall and result/cleanup work after it. Never use a
// per-SDK-method scope: raw Lua calls in between need the same lease/gate.
// No Native module/context authority is granted by this scope.
class ModLuaEntry final {
#ifndef UE4SS_LUA_LIFECYCLE_BASELINE
    LuaMadeSimple::LuaExecutionScope execution_;
#endif
public:
    explicit ModLuaEntry(const LuaMadeSimple::Lua& lua)
#ifndef UE4SS_LUA_LIFECYCLE_BASELINE
        : execution_(lua.get_lua_state(), lua.get_root_lua_state())
#endif
        { (void)lua; }
    explicit ModLuaEntry(lua_State* actual)
#ifndef UE4SS_LUA_LIFECYCLE_BASELINE
        : execution_(actual, LuaMadeSimple::cached_root_lua_state(actual))
#endif
        { (void)actual; }
};
}
