#pragma once
#include <lua.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lms_host_function_request {
    lua_Integer id, kind;
    const char* global_name;
    lua_CFunction trampoline;
};
int lms_host_function_publish(lua_State*, const struct lms_host_function_request*);
struct lms_internal_function_request {
    lua_CFunction verified_host;
    const void* identity;
    int anchor;
};
int lms_internal_function_publish(lua_State*, struct lms_internal_function_request*);
const void* lms_internal_function_identity(lua_State*);
int lms_internal_function_validate(lua_State*, int anchor);
void lms_internal_function_discard(lua_State*, int anchor);
void lms_managed_debug_hook(lua_State*, lua_Debug*);

#ifdef __cplusplus
}
#endif
