#ifndef UIKIT_LUA_REFERENCE_FRAME_H
#define UIKIT_LUA_REFERENCE_FRAME_H
#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
enum uikit_lua_reference_operation {
    UIKIT_REFERENCE_RETAIN, UIKIT_REFERENCE_PUSH, UIKIT_REFERENCE_RELEASE
};
struct uikit_lua_reference_request {
    enum uikit_lua_reference_operation operation;
    int index;
};
int uikit_lua_reference_vm_matches(lua_State* actual, lua_State* root);
int uikit_lua_reference_normalize(lua_State*, int index);
int uikit_lua_reference_top(lua_State*);
void uikit_lua_reference_pop_to(lua_State*, int top);
int uikit_lua_reference_run(lua_State*, struct uikit_lua_reference_request*, int value_index);
#ifdef __cplusplus
}
#endif
#endif
