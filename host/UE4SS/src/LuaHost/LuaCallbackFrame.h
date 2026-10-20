#pragma once
#include <LuaHost/UIKitLuaHost.h>
#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>
struct uikit_lua_callback_request {
    uint64_t runtime, callback, generation, emission;
    uint32_t upvalues;
    int caller_offset;
    void* userdata;
    const void* closure;
};
int uikit_lua_callback_push(lua_State*,struct uikit_lua_callback_request*);
void uikit_lua_callback_commit(lua_State*,uint32_t);
int uikit_lua_callback_entry(lua_State*);
const void* uikit_lua_callback_current_identity(lua_State*);
ue4ss_lua_host_status uikit_host_callback_dispatch(lua_State*,uint64_t,void*,uint64_t,int*);
void uikit_host_callback_collected(uint64_t,void*,uint64_t);
#ifdef __cplusplus
}
#endif
