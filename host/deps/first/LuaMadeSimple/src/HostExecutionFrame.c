/* Exact bundled Lua. Only POD C frames can be skipped by allocation longjmp. */
#include "HostExecutionFrame.h"
#include <lauxlib.h>
#include <lstate.h>
struct thread_request { lua_State* actual; };
static int create_thread(lua_State* L) {
    struct thread_request* request = lua_touserdata(L, 1);
    lua_State* actual = lua_newthread(L);
    // Existing SDK new_thread leaves the child on the caller's stack. Add a
    // separate VM-owned registry pin until actual lua_close so cached wrapper
    // identity never refers to a collected/reused child while the VM is open.
    lua_pushvalue(L, -1);
    (void)luaL_ref(L, LUA_REGISTRYINDEX);
    request->actual = actual;
    return 1;
}
int lms_execution_new_thread(lua_State* L, lua_State** output) {
    const int top = (int)(L->top.p - (L->ci->func.p + 1));
    const lu_byte allowhook = L->allowhook;
    struct thread_request request = {0};
    int status;
    *output = NULL;
    if (L->stack_last.p - L->top.p < 3) return LUA_ERRMEM;
    if (L->ci->top.p < L->top.p + 2) L->ci->top.p = L->top.p + 2;
    L->allowhook = 0;
    lua_pushcfunction(L, create_thread);
    lua_pushlightuserdata(L, &request);
    status = lua_pcall(L, 1, 1, 0);
    L->allowhook = allowhook;
    if (status != LUA_OK) L->top.p = L->ci->func.p + 1 + top;
    else *output = request.actual;
    return status;
}

static int anchor_current_thread(lua_State* L) {
    lua_pushthread(L);
    (void)luaL_ref(L,LUA_REGISTRYINDEX);
    return 0;
}
int lms_execution_anchor_thread(lua_State* L,lua_State* root) {
    int status;
    const int top=(int)(L->top.p-(L->ci->func.p+1));
    const lu_byte hooks=L->allowhook;
    if(!root || G(L)!=G(root)) return LUA_ERRRUN;
    if(L->stack_last.p-L->top.p<2) return LUA_ERRMEM;
    if(L->ci->top.p<L->top.p+1) L->ci->top.p=L->top.p+1;
    L->allowhook=0;
    lua_pushcfunction(L,anchor_current_thread);
    status=lua_pcall(L,0,0,0);
    L->allowhook=hooks;
    if(status!=LUA_OK) L->top.p=L->ci->func.p+1+top;
    return status;
}
