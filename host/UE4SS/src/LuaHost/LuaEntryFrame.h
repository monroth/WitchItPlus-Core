#pragma once
#include "LuaValueFrame.h"
#ifdef __cplusplus
extern "C" {
#endif
int uikit_lua_entry_matches_root(lua_State*, lua_State*);
int uikit_lua_entry_reserve(lua_State*);
#ifdef __cplusplus
}
#endif
