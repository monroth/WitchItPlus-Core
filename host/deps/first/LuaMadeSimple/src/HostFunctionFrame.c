/* All allocation below lua_pcall, with only POD C frames. Exact bundled Lua. */
#include "HostFunctionFrame.h"
#include <lstate.h>
#include <lfunc.h>
#include <lapi.h>
#include <lauxlib.h>
static int publish(lua_State* L) {
    const struct lms_host_function_request* r = lua_touserdata(L, 1);
    lua_pushinteger(L, r->id); lua_pushinteger(L, r->kind);
    lua_pushcclosure(L, r->trampoline, 2);
    if (r->global_name) { lua_setglobal(L, r->global_name); return 0; }
    return 1;
}
int lms_host_function_publish(lua_State* L, const struct lms_host_function_request* r) {
    const int top = (int)(L->top.p - (L->ci->func.p + 1));
    lu_byte hooks;
    int status;
    if (L->stack_last.p - L->top.p < 3) return LUA_ERRMEM;
    if (L->ci->top.p < L->top.p + 2) L->ci->top.p = L->top.p + 2;
    hooks = L->allowhook; L->allowhook = 0;
    lua_pushcfunction(L, publish); lua_pushlightuserdata(L, (void*)r);
    status = lua_pcall(L, 1, r->global_name ? 0 : 1, 0);
    L->allowhook = hooks;
    if (status != LUA_OK) L->top.p = L->ci->func.p + 1 + top;
    return status;
}

/* Native internal callbacks retain their original zero upvalues. An actual
 * zero-upvalue CClosure gives the host a private identity without inserting
 * authority or metadata into consumer-visible upvalues. The registry anchor
 * prevents address reuse until the VM's pure metadata is revoked. */
extern int lms_dispatch_internal_function(lua_State*, int*);
static int internal_trampoline(lua_State* L) {
    int results = 0;
    if (lms_dispatch_internal_function(L, &results) != 0)
        return luaL_error(L, "Host refused expired internal callback");
    return results;
}
static int publish_internal(lua_State* L) {
    struct lms_internal_function_request* r = lua_touserdata(L, 1);
    if (r->verified_host) {
        lua_pushcfunction(L, r->verified_host);
        return 1;
    }
    {
        CClosure* closure = luaF_newCclosure(L, 0);
        closure->f = internal_trampoline;
        setclCvalue(L, s2v(L->top.p), closure);
        api_incr_top(L);
        r->identity = closure;
        lua_pushvalue(L, -1);
        r->anchor = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 1;
}
int lms_internal_function_publish(lua_State* L, struct lms_internal_function_request* r) {
    const int top = (int)(L->top.p - (L->ci->func.p + 1));
    lu_byte hooks; int status;
    r->identity = NULL; r->anchor = LUA_NOREF;
    if (L->stack_last.p - L->top.p < 3) return LUA_ERRMEM;
    if (L->ci->top.p < L->top.p + 2) L->ci->top.p = L->top.p + 2;
    hooks = L->allowhook; L->allowhook = 0;
    lua_pushcfunction(L, publish_internal); lua_pushlightuserdata(L, r);
    status = lua_pcall(L, 1, 1, 0);
    L->allowhook = hooks;
    if (status != LUA_OK) {
        L->top.p = L->ci->func.p + 1 + top;
        if (r->anchor >= 0) luaL_unref(L, LUA_REGISTRYINDEX, r->anchor);
        r->identity = NULL; r->anchor = LUA_NOREF;
    }
    return status;
}
const void* lms_internal_function_identity(lua_State* L) {
    const TValue* function = s2v(L->ci->func.p);
    return ttisCclosure(function) ? (const void*)clCvalue(function) : NULL;
}
int lms_internal_function_validate(lua_State* L,int anchor) {
    const TValue* current=s2v(L->ci->func.p);
    int valid;
    if(anchor<0 || !ttisCclosure(current) || clCvalue(current)->nupvalues!=0 ||
       clCvalue(current)->f!=internal_trampoline) return 0;
    if(L->stack_last.p-L->top.p<1 || L->ci->top.p<L->top.p+1) return 0;
    lua_rawgeti(L,LUA_REGISTRYINDEX,anchor);
    valid=ttisCclosure(s2v(L->top.p-1)) && clCvalue(s2v(L->top.p-1))==clCvalue(current);
    lua_pop(L,1);return valid;
}
void lms_internal_function_discard(lua_State* L, int anchor) {
    if (anchor >= 0) luaL_unref(L, LUA_REGISTRYINDEX, anchor);
    lua_pop(L, 1);
}

extern int lms_dispatch_debug_hook(lua_State*, lua_Debug*);
void lms_managed_debug_hook(lua_State* L,lua_Debug* debug) {
    if(lms_dispatch_debug_hook(L,debug)!=0)
        luaL_error(L,"Host refused expired Native debug hook");
}
