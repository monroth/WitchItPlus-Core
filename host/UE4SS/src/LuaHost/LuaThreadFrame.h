#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
enum uikit_lua_thread_operation { UIKIT_THREAD_CREATE, UIKIT_THREAD_RESET, UIKIT_THREAD_RELEASE };
struct uikit_lua_thread_request {
    enum uikit_lua_thread_operation operation;
    lua_State* thread;
    int reference;
    int reset_status;
};
/* Caller holds an actual VM lease/finalizer and the existing LuaRaw gate. */
int uikit_lua_thread_run(lua_State*, struct uikit_lua_thread_request*);
#ifdef __cplusplus
}
#endif
