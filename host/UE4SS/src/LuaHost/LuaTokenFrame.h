#ifndef UIKIT_LUA_TOKEN_FRAME_H
#define UIKIT_LUA_TOKEN_FRAME_H
#include <LuaHost/UIKitLuaHost.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
enum uikit_lua_token_operation { UIKIT_TOKEN_PUSH, UIKIT_TOKEN_READ, UIKIT_TOKEN_UPVALUE_READ };
struct uikit_lua_token_request {
    enum uikit_lua_token_operation operation; int index; void* domain;
    void (*collected)(void*, uint64_t, const void*);
    uint64_t record_id; ue4ss_lua_token_v1 token;
    const void* address; int recognized; ue4ss_lua_host_status status;
};
int uikit_lua_token_run(lua_State*, struct uikit_lua_token_request*);
#ifdef __cplusplus
}
#endif
#endif
