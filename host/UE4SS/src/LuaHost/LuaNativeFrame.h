#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
/* Host-private boundary; does not create or replace the current Lua CallInfo.
 * invoke must contain C++ exceptions and must not own host lifetime objects.
 * Native nonlocal Lua errors return here before outer C++ owners unwind. */
int uikit_lua_native_run(lua_State*, void* request, void (*invoke)(void*));
lua_State* uikit_lua_native_current_actual(void);
#ifdef __cplusplus
}
#endif
