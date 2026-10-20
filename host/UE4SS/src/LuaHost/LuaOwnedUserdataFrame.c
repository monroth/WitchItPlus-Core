#include "LuaOwnedUserdataFrame.h"
#include "LuaNativeFrame.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char owned_weak_key;
static void push_weak(lua_State* L) {
    lua_rawgetp(L,LUA_REGISTRYINDEX,&owned_weak_key);
    if(lua_istable(L,-1))return;
    lua_pop(L,1);lua_newtable(L);lua_newtable(L);
    lua_pushliteral(L,"v");lua_setfield(L,-2,"__mode");lua_setmetatable(L,-2);
    lua_pushvalue(L,-1);lua_rawsetp(L,LUA_REGISTRYINDEX,&owned_weak_key);
}
static void invoke(void* opaque) {
    struct uikit_owned_userdata_request* r=opaque;lua_State* L=r->actual;
    int userdata, old;
    // Rollback consumes only already-reserved stack slots; it must not need
    // another allocation while returning from an allocation failure.
    if(r->action!=UIKIT_OWNED_REMOVE)luaL_checkstack(L,12,"owned userdata HOST frame");
    switch(r->action) {
    case UIKIT_OWNED_ALLOC:
        r->storage=lua_newuserdatauv(L,r->size,r->values);
        lua_pushvalue(L,-1);r->reference=luaL_ref(L,LUA_REGISTRYINDEX);break;
    case UIKIT_OWNED_ARM:
        userdata=lua_absindex(L,-1);
        if(!lua_getmetatable(L,userdata))luaL_error(L,"owned userdata has no typed GC metatable");
        old=lua_absindex(L,-1);lua_getfield(L,old,"__gc");
        // The SDK automatic helper is the exact typed compiler-instantiated
        // function. Custom/mismatched metatables cannot establish ownership.
        if(!lua_iscfunction(L,-1)||lua_tocfunction(L,-1)!=r->expected_gc)
            luaL_error(L,"owned userdata typed GC identity mismatch");
        lua_pop(L,1);lua_newtable(L);lua_pushnil(L);
        while(lua_next(L,old)) {lua_pushvalue(L,-2);lua_pushvalue(L,-2);lua_rawset(L,-5);lua_pop(L,1);}
        lua_pushcfunction(L,uikit_owned_userdata_gc);lua_setfield(L,-2,"__gc");
        lua_setmetatable(L,userdata);lua_pop(L,1);break;
    case UIKIT_OWNED_COMMIT:
        push_weak(L);lua_rawgeti(L,LUA_REGISTRYINDEX,r->reference);
        lua_rawseti(L,-2,(lua_Integer)r->ticket);lua_pop(L,1);
        luaL_unref(L,LUA_REGISTRYINDEX,r->reference);r->reference=LUA_NOREF;break;
    case UIKIT_OWNED_GET:
        push_weak(L);lua_rawgeti(L,-1,(lua_Integer)r->ticket);lua_remove(L,-2);
        r->storage=lua_type(L,-1)==LUA_TUSERDATA?lua_touserdata(L,-1):NULL;
        r->observed_size=r->storage?lua_rawlen(L,-1):0;break;
    case UIKIT_OWNED_REMOVE:
        if(r->reference!=LUA_NOREF)luaL_unref(L,LUA_REGISTRYINDEX,r->reference);
        lua_rawgetp(L,LUA_REGISTRYINDEX,&owned_weak_key);
        if(lua_istable(L,-1)){lua_pushnil(L);lua_rawseti(L,-2,(lua_Integer)r->ticket);}
        lua_pop(L,1);lua_settop(L,r->top);break;
    case UIKIT_OWNED_READ_GC:
        r->storage=lua_type(L,1)==LUA_TUSERDATA?lua_touserdata(L,1):NULL;
        r->observed_size=r->storage?lua_rawlen(L,1):0;break;
    }
}
int uikit_owned_userdata_run(lua_State* L,struct uikit_owned_userdata_request* r) {
    r->actual=L;return uikit_lua_native_run(L,r,invoke);
}
int uikit_owned_userdata_gc(lua_State* L) {
    struct uikit_owned_userdata_request r={0};int status;
    if(!uikit_owned_userdata_authorize_gc(L)) {
        /* There is no admitted VM on which even luaL_error may safely run.
         * Never report destructor completion or borrow freed state memory. */
        fputs("HOST owned userdata GC entered without VM admission\n",stderr);
        abort();
    }
    r.action=UIKIT_OWNED_READ_GC;
    if(uikit_owned_userdata_run(L,&r)!=LUA_OK)return luaL_error(L,"HOST owned userdata inspection failed");
    status=uikit_owned_userdata_dispatch_gc(L,r.storage,r.observed_size);
    if(status)return luaL_error(L,"HOST owned userdata destruction incomplete; module remains pinned");
    return 0;
}
