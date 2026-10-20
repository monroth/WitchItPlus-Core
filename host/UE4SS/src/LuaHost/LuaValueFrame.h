#ifndef UIKIT_LUA_VALUE_FRAME_H
#define UIKIT_LUA_VALUE_FRAME_H

#include <LuaHost/UIKitLuaHost.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
enum uikit_lua_value_operation {
    UIKIT_VALUE_READ, UIKIT_VALUE_UPVALUE_READ, UIKIT_VALUE_PUSH,
    UIKIT_VALUE_TABLE_SET, UIKIT_VALUE_GROW, UIKIT_VALUE_RESERVE
};
#define UIKIT_LUA_VALUE_BOOTSTRAP_RESERVE 5
// All request storage is POD owned by the C++ caller ABOVE lua_pcall.
struct uikit_lua_value_request {
    enum uikit_lua_value_operation operation;
    int index;
    int count;
    const ue4ss_lua_value_v1* input;
    ue4ss_lua_value_v1* output;
    const char* key;
    size_t key_size;
    ue4ss_lua_host_status status;
};

const void* uikit_lua_value_frame_identity(lua_State*);
int uikit_lua_value_top(lua_State*);
int uikit_lua_value_normalize_index(int top, int index, int allow_absent);
int uikit_lua_value_can_pop(lua_State*, int new_top);
// Caller has already checked can_pop; no allocation, user code, or longjmp.
void uikit_lua_value_pop_to(lua_State*, int new_top);
int uikit_lua_value_current_closure(lua_State*, lua_CFunction*, unsigned int*);
int uikit_lua_value_is_table(lua_State*, int normalized_index);
// Bootstrap uses only existing physical capacity. No lua_checkstack is called
// outside lua_pcall. Missing capacity fails closed; the resolver can establish
// the four-slot reserve at trusted entry with its own protected setup.
int uikit_lua_value_run(lua_State*, struct uikit_lua_value_request*);
int uikit_lua_value_call(lua_State*, int nargs, int nresults);
int uikit_lua_value_max_top(void);
#ifdef __cplusplus
}
#endif
#endif
