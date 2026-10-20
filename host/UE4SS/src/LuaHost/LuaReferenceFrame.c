/* Pure C POD frames, linked into the EXISTING embedded LuaRaw. The bundled
 * private structures below require the exact bundled runtime, never system Lua. */
#include "LuaReferenceFrame.h"
#include <lauxlib.h>
#include <lstate.h>
#include <lobject.h>
#include <limits.h>

int uikit_lua_reference_vm_matches(lua_State* actual, lua_State* root) {
    return actual && root && G(actual) == G(root);
}
int uikit_lua_reference_top(lua_State* L) { return (int)(L->top.p - (L->ci->func.p + 1)); }
int uikit_lua_reference_normalize(lua_State* L, int index) {
    const int top = uikit_lua_reference_top(L);
    long long absolute;
    if (!index || index <= LUA_REGISTRYINDEX) return 0;
    absolute = index > 0 ? index : (long long)top + index + 1;
    return absolute >= 1 && absolute <= top && absolute <= INT_MAX ? (int)absolute : 0;
}
void uikit_lua_reference_pop_to(lua_State* L, int top) {
    /* Only helper-added results/error objects are discarded; originals and
     * their existing Lua 5.4 to-be-closed obligations are never popped. */
    L->top.p = L->ci->func.p + 1 + top;
}
static int perform(lua_State* L) {
    struct uikit_lua_reference_request* request =
        (struct uikit_lua_reference_request*)lua_touserdata(L, 1);
    switch (request->operation) {
    case UIKIT_REFERENCE_RETAIN:
        lua_pushvalue(L, 2);
        request->index = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    case UIKIT_REFERENCE_PUSH:
        if (!lua_checkstack(L, 5)) return luaL_error(L, "host reference result capacity unavailable");
        if (request->index == LUA_REFNIL) lua_pushnil(L);
        else lua_rawgeti(L, LUA_REGISTRYINDEX, request->index);
        return 1;
    case UIKIT_REFERENCE_RELEASE:
        luaL_unref(L, LUA_REGISTRYINDEX, request->index);
        return 0;
    }
    return luaL_error(L, "host reference operation invalid");
}
int uikit_lua_reference_run(lua_State* L, struct uikit_lua_reference_request* request, int value_index) {
    const int old_top = uikit_lua_reference_top(L);
    const int retain = request->operation == UIKIT_REFERENCE_RETAIN;
    const int slots = retain ? 3 : 2;
    lu_byte old_allowhook;
    int status;
    // Existing capacity only: no allocator or longjmp before lua_pcall.
    if (L->stack_last.p - L->top.p < slots + 1) return LUA_ERRMEM;
    if (L->ci->top.p < L->top.p + slots) L->ci->top.p = L->top.p + slots;
    old_allowhook = L->allowhook;
    L->allowhook = 0;
    lua_pushcfunction(L, perform);
    lua_pushlightuserdata(L, request);
    if (retain) lua_pushvalue(L, value_index);
    status = lua_pcall(L, retain ? 2 : 1, LUA_MULTRET, 0);
    L->allowhook = old_allowhook;
    if (status != LUA_OK) uikit_lua_reference_pop_to(L, old_top);
    return status;
}
