/* Pure C POD frames beneath pcall, exact bundled LuaRaw definitions. */
#include "LuaCallbackFrame.h"
#include "LuaValueFrame.h"
#include <lauxlib.h>
#include <lstate.h>
#include <lobject.h>
#include <lapi.h>
#include <lgc.h>
#include <string.h>
struct descriptor { uint64_t runtime, callback, generation, emission; };
static const char metatable_key;
static int collected(lua_State* L) {
    if(lua_type(L,1)==LUA_TUSERDATA && lua_rawlen(L,1)==sizeof(struct descriptor)) {
        void* memory=lua_touserdata(L,1);struct descriptor d;
        memcpy(&d,memory,sizeof(d));uikit_host_callback_collected(d.runtime,memory,d.emission);
    }
    return 0;
}
static void metatable(lua_State* L) {
    lua_rawgetp(L,LUA_REGISTRYINDEX,&metatable_key);
    if(lua_isnil(L,-1)) {
        lua_pop(L,1);lua_createtable(L,0,2);
        lua_pushcfunction(L,collected);lua_setfield(L,-2,"__gc");
        lua_pushliteral(L,"UE4SS host callback");lua_setfield(L,-2,"__metatable");
        lua_pushvalue(L,-1);lua_rawsetp(L,LUA_REGISTRYINDEX,&metatable_key);
    } else {
        if(!lua_istable(L,-1))luaL_error(L,"Invalid host callback metatable");
        lua_getfield(L,-1,"__gc");
        if(lua_tocfunction(L,-1)!=collected)luaL_error(L,"Changed host callback finalizer");
        lua_pop(L,1);
    }
}
static int push(lua_State* L) {
    struct uikit_lua_callback_request* r=lua_touserdata(L,1);
    unsigned i;struct descriptor* d;
    if(!lua_checkstack(L,(int)r->upvalues+4+UIKIT_LUA_VALUE_BOOTSTRAP_RESERVE))
        return luaL_error(L,"Host callback stack capacity");
    lua_pushinteger(L,(lua_Integer)r->runtime);
    d=lua_newuserdatauv(L,sizeof(*d),0);
    *d=(struct descriptor){r->runtime,r->callback,r->generation,r->emission};r->userdata=d;
    metatable(L);lua_setmetatable(L,-2);
    for(i=0;i<r->upvalues;++i) {
        const TValue* source=s2v(L->stack.p+r->caller_offset+(int)i);
        setobj2s(L,L->top.p,source);api_incr_top(L);
    }
    lua_pushcclosure(L,uikit_lua_callback_entry,(int)r->upvalues+2);
    r->closure=clCvalue(s2v(L->top.p-1));
    return 1;
}
int uikit_lua_callback_push(lua_State* L,struct uikit_lua_callback_request* r) {
    const int top=uikit_lua_value_top(L);lu_byte hooks;int status;
    if(r->upvalues>253 || (int)r->upvalues>top || !uikit_lua_value_can_pop(L,top-(int)r->upvalues))return LUA_ERRRUN;
    if(L->stack_last.p-L->top.p<3)return LUA_ERRMEM;
    r->caller_offset=(int)((L->ci->func.p+1+top-(int)r->upvalues)-L->stack.p);
    if(L->ci->top.p<L->top.p+2)L->ci->top.p=L->top.p+2;
    hooks=L->allowhook;L->allowhook=0;
    lua_pushcfunction(L,push);lua_pushlightuserdata(L,r);status=lua_pcall(L,1,1,0);
    L->allowhook=hooks;
    if(status!=LUA_OK)uikit_lua_value_pop_to(L,top);
    return status;
}
void uikit_lua_callback_commit(lua_State* L,uint32_t count) {
    StkId target=L->top.p-1-count;
    setobjs2s(L,target,L->top.p-1);L->top.p=target+1;
}
const void* uikit_lua_callback_current_identity(lua_State* L) {
    TValue* f=s2v(L->ci->func.p);return ttisCclosure(f)?clCvalue(f):NULL;
}
int uikit_lua_callback_entry(lua_State* L) {
    uint64_t runtime;void* memory;struct descriptor d;int results=0;ue4ss_lua_host_status status;
    if(lua_type(L,lua_upvalueindex(1))!=LUA_TNUMBER || !lua_isinteger(L,lua_upvalueindex(1)) ||
       lua_type(L,lua_upvalueindex(2))!=LUA_TUSERDATA || lua_rawlen(L,lua_upvalueindex(2))!=sizeof(d))
        return luaL_error(L,"Invalid host callback descriptor");
    runtime=(uint64_t)lua_tointeger(L,lua_upvalueindex(1));memory=lua_touserdata(L,lua_upvalueindex(2));
    memcpy(&d,memory,sizeof(d));
    if(d.runtime!=runtime)return luaL_error(L,"Changed host callback descriptor");
    status=uikit_host_callback_dispatch(L,runtime,memory,d.emission,&results);
    /* All C++ leases/pins have returned before a Lua error or yield can unwind. */
    if(status!=UE4SS_LUA_HOST_OK)return luaL_error(L,"Host callback refused (%d)",(int)status);
    return results;
}
