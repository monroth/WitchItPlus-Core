#include "LuaThreadFrame.h"
#include <lauxlib.h>
#include <lstate.h>
static int perform(lua_State* L) {
    struct uikit_lua_thread_request* request =
        (struct uikit_lua_thread_request*)lua_touserdata(L, 1);
    if (!lua_checkstack(L, 5)) return luaL_error(L, "host coroutine helper capacity unavailable");
    switch (request->operation) {
    case UIKIT_THREAD_CREATE: {
        lua_State* thread = lua_newthread(L);
        int reference = luaL_ref(L, LUA_REGISTRYINDEX);
        request->thread = thread;
        request->reference = reference;
        return 0;
    }
    case UIKIT_THREAD_RESET:
        if (!request->thread || G(request->thread) != G(L))
            return luaL_error(L, "host coroutine owner mismatch");
        request->reset_status = lua_resetthread(request->thread);
        return 0;
    case UIKIT_THREAD_RELEASE:
        luaL_unref(L, LUA_REGISTRYINDEX, request->reference);
        return 0;
    }
    return luaL_error(L, "host coroutine operation invalid");
}
int uikit_lua_thread_run(lua_State* L, struct uikit_lua_thread_request* request) {
    const int old_top = (int)(L->top.p - (L->ci->func.p + 1));
    const lu_byte allowhook = L->allowhook;
    int status;
    /* No allocation/longjmp across caller C++ frames before pcall. */
    if (L->stack_last.p - L->top.p < 3) return LUA_ERRMEM;
    if (L->ci->top.p < L->top.p + 2) L->ci->top.p = L->top.p + 2;
    L->allowhook = 0;
    lua_pushcfunction(L, perform);
    lua_pushlightuserdata(L, request);
    status = lua_pcall(L, 1, 0, 0);
    L->allowhook = allowhook;
    L->top.p = L->ci->func.p + 1 + old_top;
    return status;
}
