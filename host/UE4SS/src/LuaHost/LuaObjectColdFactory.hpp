#pragma once
#include "LuaObjectTypes.hpp"
#include "LuaObjectFrame.h"
namespace RC::LuaHost {
// Caller holds real admitted VM/thread/module context and actual global gate.
// Captured registry refs remain caller-owned until authority.cache adopts them.
ue4ss_lua_host_status prepare_cold_object_factory(const LuaValueContext&,LuaObjectAuthority&,lua_State*,
    LuaObjectKind,LuaPreparedObject&,uikit_lua_object_request&,LuaObjectFactoryRecord&) noexcept;
}
