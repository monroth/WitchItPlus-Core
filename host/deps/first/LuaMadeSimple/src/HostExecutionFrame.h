#pragma once
#include <lua.h>
#ifdef __cplusplus
extern "C" {
#endif
int lms_execution_anchor_thread(lua_State*, lua_State*);
int lms_execution_new_thread(lua_State*, lua_State** output);
#ifdef __cplusplus
}
#endif
