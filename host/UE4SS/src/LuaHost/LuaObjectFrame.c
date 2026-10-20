#include "LuaObjectFrame.h"
#include "LuaValueFrame.h"
#include <lauxlib.h>
#include <lstate.h>
#include <lobject.h>
#include <stdint.h>

static int pinned_value(lua_State* L,int reference,int kind,const void* identity) {
    if(reference<=0 || !identity)return 0;
    if(lua_rawgeti(L,LUA_REGISTRYINDEX,reference)!=kind || lua_topointer(L,-1)!=identity){lua_pop(L,1);return 0;}
    return 1;
}
static int private_factory_matches(lua_State* L,struct uikit_lua_object_request* r) {
    int matches;
    if(!r->private_registry_key)return 1;
    lua_pushlightuserdata(L,(void*)r->private_registry_key);lua_rawget(L,LUA_REGISTRYINDEX);
    matches=lua_type(L,-1)==LUA_TTABLE && lua_topointer(L,-1)==r->metatable_identity;
    lua_pop(L,1);return matches;
}
static void get_factory_slot(lua_State* L,struct uikit_lua_object_request* r) {
    if(r->private_registry_key){lua_pushlightuserdata(L,(void*)r->private_registry_key);lua_rawget(L,LUA_REGISTRYINDEX);}
    else {lua_pushstring(L,r->factory_name);lua_rawget(L,LUA_REGISTRYINDEX);}
}
static void set_factory_slot(lua_State* L,struct uikit_lua_object_request* r) {
    if(r->private_registry_key)lua_pushlightuserdata(L,(void*)r->private_registry_key);
    else lua_pushstring(L,r->factory_name);
    lua_rawgeti(L,LUA_REGISTRYINDEX,r->metatable_ref);lua_rawset(L,LUA_REGISTRYINDEX);
}
static void alias_named_factory_if_absent(lua_State* L,struct uikit_lua_object_request* r) {
    /* lua_pushstring may run normal GC. Keep that exact key alive, then use
     * only raw non-GC-check operations between the absence check and write. */
    lua_pushstring(L,r->factory_name);lua_pushvalue(L,-1);lua_rawget(L,LUA_REGISTRYINDEX);
    if(!lua_isnil(L,-1)){
        r->named_factory_published=lua_type(L,-1)==LUA_TTABLE && lua_topointer(L,-1)==r->metatable_identity;
        lua_pop(L,2);return; /* Any present named entry is preserved. */
    }
    lua_pop(L,1);lua_rawgeti(L,LUA_REGISTRYINDEX,r->metatable_ref);lua_rawset(L,LUA_REGISTRYINDEX);
    r->named_factory_published=1;
}
static int pinned_metatable(lua_State* L,struct uikit_lua_object_request* r) {
    if(r->operation!=UIKIT_OBJECT_FACTORY_ALIAS && !private_factory_matches(L,r))return 0;
    if(!pinned_value(L,r->metatable_ref,LUA_TTABLE,r->metatable_identity))return 0;
    lua_pushliteral(L,"__gc");lua_rawget(L,-2);
    if(!r->destructor || lua_tocfunction(L,-1)!=r->destructor){lua_pop(L,2);return 0;}
    /* Exact SDK automatic finalizer is a zero-upvalue C function. A Lua closure
     * or mutable C closure masquerading as a destructor is not accepted. */
    if(lua_getupvalue(L,-1,1)!=NULL){lua_pop(L,3);return 0;}
    lua_pop(L,1);return 1;
}
static int method_container(lua_State* L,int at,struct uikit_lua_object_request* r,int capture) {
    int absolute=lua_absindex(L,at);
    if(lua_type(L,absolute)!=LUA_TUSERDATA || lua_rawlen(L,absolute)!=r->metamethods_size ||
       !r->metamethods_alignment || (uintptr_t)lua_touserdata(L,absolute)%r->metamethods_alignment)return 0;
    if(lua_getiuservalue(L,absolute,2)!=LUA_TNUMBER || !lua_isinteger(L,-1) || lua_tointeger(L,-1)!=r->storage_tag){lua_pop(L,1);return 0;}lua_pop(L,1);
    if(!lua_getmetatable(L,absolute))return 0;
    if(capture)r->metamethods_metatable_identity=lua_topointer(L,-1);
    else if(lua_topointer(L,-1)!=r->metamethods_metatable_identity){lua_pop(L,1);return 0;}
    lua_pushliteral(L,"__gc");lua_rawget(L,-2);
    if(capture)r->metamethods_destructor=lua_tocfunction(L,-1);
    if(!r->metamethods_destructor || lua_tocfunction(L,-1)!=r->metamethods_destructor){lua_pop(L,2);return 0;}
    if(lua_getupvalue(L,-1,1)!=NULL){lua_pop(L,3);return 0;}
    lua_pop(L,2);return 1;
}
static void push_pair_value(lua_State* L,const struct uikit_lua_object_pair* pair) {
    if(pair->kind==UIKIT_OBJECT_PAIR_FUNCTION) {
        lua_pushinteger(L,pair->id);lua_pushinteger(L,pair->function_kind);
        lua_pushcclosure(L,pair->function,2);
    } else if(pair->kind==UIKIT_OBJECT_PAIR_RAW_FUNCTION)lua_pushcfunction(L,pair->function);
    else lua_pushboolean(L,pair->boolean);
}
static void fill_pairs(lua_State* L,int table,const struct uikit_lua_object_pair* pairs,size_t count) {
    size_t i;table=lua_absindex(L,table);
    for(i=0;i<count;++i){lua_pushlstring(L,pairs[i].key,pairs[i].key_size);push_pair_value(L,&pairs[i]);lua_rawset(L,table);}
}
static int capture_factory(lua_State* L,int mt,struct uikit_lua_object_request* r) {
    mt=lua_absindex(L,mt);
    if(lua_type(L,mt)!=LUA_TTABLE)return 0;
    r->metatable_identity=lua_topointer(L,mt);
    lua_pushliteral(L,"__gc");lua_rawget(L,mt);r->destructor=lua_tocfunction(L,-1);
    if(!r->destructor || lua_getupvalue(L,-1,1)!=NULL)return 0;
    lua_pop(L,1);lua_pushvalue(L,mt);r->metatable_ref=luaL_ref(L,LUA_REGISTRYINDEX);
    if(lua_rawgeti(L,mt,1)!=LUA_TTABLE)return 0;
    r->members_identity=lua_topointer(L,-1);r->members_ref=luaL_ref(L,LUA_REGISTRYINDEX);
    if(lua_rawgeti(L,mt,2)!=LUA_TUSERDATA || !method_container(L,-1,r,1))return 0;
    r->metamethods_identity=lua_topointer(L,-1);r->metamethods_ref=luaL_ref(L,LUA_REGISTRYINDEX);
    return 1;
}
static int verify(lua_State* L,int at,struct uikit_lua_object_request* r) {
    int absolute=lua_absindex(L,at);
    if(lua_type(L,absolute)!=LUA_TUSERDATA || lua_touserdata(L,absolute)!=r->storage ||
       lua_rawlen(L,absolute)!=r->size || !r->alignment ||
       (uintptr_t)r->storage%r->alignment)return 0;
    if(!lua_getmetatable(L,absolute))return 0;
    if(lua_topointer(L,-1)!=r->metatable_identity){lua_pop(L,1);return 0;}
    lua_pop(L,1);
    if(!pinned_metatable(L,r))return 0;
    lua_pop(L,1);
    if(lua_getiuservalue(L,absolute,2)!=LUA_TNUMBER || !lua_isinteger(L,-1) ||
       lua_tointeger(L,-1)!=r->storage_tag){lua_pop(L,1);return 0;}
    lua_pop(L,1);
    if(lua_getiuservalue(L,absolute,3)!=LUA_TTABLE || lua_topointer(L,-1)!=r->members_identity){lua_pop(L,1);return 0;}lua_pop(L,1);
    if(lua_getiuservalue(L,absolute,4)!=LUA_TUSERDATA || lua_topointer(L,-1)!=r->metamethods_identity){lua_pop(L,1);return 0;}
    if(!method_container(L,-1,r,0)){lua_pop(L,1);return 0;}lua_pop(L,1);
    return 1;
}
static int perform(lua_State* L) {
    struct uikit_lua_object_request* r=(struct uikit_lua_object_request*)lua_touserdata(L,1);
    if(!lua_checkstack(L,12)){r->status=UE4SS_LUA_HOST_INTERNAL_ERROR;return 0;}
    switch(r->operation) {
    case UIKIT_OBJECT_INSPECT:
        if(lua_type(L,2)!=LUA_TUSERDATA){r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        r->storage=lua_touserdata(L,2);r->size=lua_rawlen(L,2);return 0;
    case UIKIT_OBJECT_VERIFY:
        if(!verify(L,2,r))r->status=UE4SS_LUA_HOST_INVALID_HANDLE;
        return 0;
    case UIKIT_OBJECT_ALLOCATE: {
        int mt,members,methods,userdata;
        if(!pinned_metatable(L,r)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}mt=lua_gettop(L);
        if(!pinned_value(L,r->members_ref,LUA_TTABLE,r->members_identity)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}members=lua_gettop(L);
        if(!pinned_value(L,r->metamethods_ref,LUA_TUSERDATA,r->metamethods_identity)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}methods=lua_gettop(L);
        if(!method_container(L,methods,r,0)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}
        (void)mt;
        r->storage=lua_newuserdatauv(L,r->size,5);userdata=lua_gettop(L);
        if(!r->alignment || (uintptr_t)r->storage%r->alignment){r->status=UE4SS_LUA_HOST_INTERNAL_ERROR;return 0;}
        lua_pushinteger(L,r->storage_tag);lua_setiuservalue(L,userdata,2);
        lua_pushvalue(L,members);lua_setiuservalue(L,userdata,3);
        lua_pushvalue(L,methods);lua_setiuservalue(L,userdata,4);
        lua_pushboolean(L,r->polymorphic);lua_setiuservalue(L,userdata,5);
        /* No metatable/finalizer is attached before C++ placement construction.
         * Any allocation failure can collect only blank raw userdata. */
        return 1;
    }
    case UIKIT_OBJECT_COMMIT:
        if(lua_type(L,2)!=LUA_TUSERDATA || lua_touserdata(L,2)!=r->storage ||
           lua_rawlen(L,2)!=r->size){r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        if(!pinned_metatable(L,r)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}
        /* In the exact bundled lapi.c this admitted lua_setmetatable call and
         * lgc.c luaC_checkfinalizer do not allocate or call Lua/user callbacks.
         * Once attached, ownership is with Lua; no manual destructor follows. */
        lua_setmetatable(L,2);r->published=1;return 0;
    case UIKIT_OBJECT_CAPTURE: {
        int absolute=2;
        if(lua_type(L,absolute)!=LUA_TUSERDATA || lua_touserdata(L,absolute)!=r->storage ||
           lua_rawlen(L,absolute)!=r->size || !r->alignment || (uintptr_t)r->storage%r->alignment){r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        if(lua_getiuservalue(L,absolute,2)!=LUA_TNUMBER || !lua_isinteger(L,-1) ||
           lua_tointeger(L,-1)!=r->storage_tag){r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}lua_pop(L,1);
        if(!lua_getmetatable(L,absolute)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}
        r->metatable_identity=lua_topointer(L,-1);
        lua_pushliteral(L,"__gc");lua_rawget(L,-2);r->destructor=lua_tocfunction(L,-1);
        if(!r->destructor || lua_getupvalue(L,-1,1)!=NULL){r->status=UE4SS_LUA_HOST_PENDING;return 0;}lua_pop(L,1);
        r->metatable_ref=luaL_ref(L,LUA_REGISTRYINDEX);
        if(lua_getiuservalue(L,absolute,3)!=LUA_TTABLE){r->status=UE4SS_LUA_HOST_PENDING;return 0;}
        r->members_identity=lua_topointer(L,-1);r->members_ref=luaL_ref(L,LUA_REGISTRYINDEX);
        if(lua_getiuservalue(L,absolute,4)!=LUA_TUSERDATA){r->status=UE4SS_LUA_HOST_PENDING;return 0;}
        if(!method_container(L,-1,r,1)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}
        r->metamethods_identity=lua_topointer(L,-1);r->metamethods_ref=luaL_ref(L,LUA_REGISTRYINDEX);
        return 0;
    }
    case UIKIT_OBJECT_FACTORY_LOOKUP:
        get_factory_slot(L,r);
        if(lua_isnil(L,-1)){r->factory_found=0;return 0;}
        r->factory_found=1;
        if(!capture_factory(L,-1,r))r->status=UE4SS_LUA_HOST_INVALID_HANDLE;
        return 0;
    case UIKIT_OBJECT_FACTORY_BUILD: {
        int auxmt,mt,members,aux;
        get_factory_slot(L,r);
        if(!lua_isnil(L,-1)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}lua_pop(L,1);
        luaL_getmetatable(L,"MetaMethodContainer");
        if(lua_isnil(L,-1)) {
            lua_pop(L,1);lua_createtable(L,0,2);
            lua_pushliteral(L,"__name");lua_pushliteral(L,"MetaMethodContainer");lua_rawset(L,-3);
            lua_pushliteral(L,"__gc");lua_pushcfunction(L,r->metamethods_destructor);lua_rawset(L,-3);
        } else {
            if(lua_type(L,-1)!=LUA_TTABLE){r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
            lua_pushliteral(L,"__gc");lua_rawget(L,-2);
            if(lua_tocfunction(L,-1)!=r->metamethods_destructor || lua_getupvalue(L,-1,1)!=NULL){r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
            lua_pop(L,1);
        }
        auxmt=lua_gettop(L);r->metamethods_metatable_identity=lua_topointer(L,auxmt);
        lua_pushvalue(L,auxmt);r->auxiliary_metatable_ref=luaL_ref(L,LUA_REGISTRYINDEX);
        lua_createtable(L,2,(int)r->metatable_count+1);mt=lua_gettop(L);
        lua_pushliteral(L,"__name");lua_pushstring(L,r->factory_name);lua_rawset(L,mt);
        fill_pairs(L,mt,r->metatable_pairs,r->metatable_count);
        lua_createtable(L,0,(int)r->member_count);members=lua_gettop(L);fill_pairs(L,members,r->member_pairs,r->member_count);
        lua_pushvalue(L,members);lua_rawseti(L,mt,1);
        r->auxiliary_storage=lua_newuserdatauv(L,r->metamethods_size,5);aux=lua_gettop(L);
        if(!r->metamethods_alignment || (uintptr_t)r->auxiliary_storage%r->metamethods_alignment){r->status=UE4SS_LUA_HOST_INTERNAL_ERROR;return 0;}
        lua_pushinteger(L,r->storage_tag);lua_setiuservalue(L,aux,2);
        lua_pushboolean(L,0);lua_setiuservalue(L,aux,5);
        lua_pushvalue(L,aux);lua_rawseti(L,mt,2);
        r->metatable_identity=lua_topointer(L,mt);lua_pushvalue(L,mt);r->metatable_ref=luaL_ref(L,LUA_REGISTRYINDEX);
        r->members_identity=lua_topointer(L,members);lua_pushvalue(L,members);r->members_ref=luaL_ref(L,LUA_REGISTRYINDEX);
        r->metamethods_identity=lua_topointer(L,aux);lua_pushvalue(L,aux);r->metamethods_ref=luaL_ref(L,LUA_REGISTRYINDEX);
        return 1; /* blank auxiliary stays rooted above the protected call */
    }
    case UIKIT_OBJECT_FACTORY_ATTACH:
        if(lua_type(L,2)!=LUA_TUSERDATA || lua_touserdata(L,2)!=r->auxiliary_storage ||
           lua_rawlen(L,2)!=r->metamethods_size ||
           !pinned_value(L,r->auxiliary_metatable_ref,LUA_TTABLE,r->metamethods_metatable_identity)){
            r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        lua_setmetatable(L,2);r->auxiliary_published=1;return 0;
    case UIKIT_OBJECT_FACTORY_COMMIT:
        if(lua_type(L,2)!=LUA_TUSERDATA || lua_touserdata(L,2)!=r->auxiliary_storage ||
           !r->auxiliary_published || !method_container(L,2,r,0)){
            r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        get_factory_slot(L,r);
        if(!lua_isnil(L,-1)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}lua_pop(L,1);
        /* Publish auxiliary metadata first. The object registry write is last;
         * all allocation/longjmp remains inside this protected POD C frame. */
        lua_pushliteral(L,"MetaMethodContainer");
        lua_rawgeti(L,LUA_REGISTRYINDEX,r->auxiliary_metatable_ref);lua_rawset(L,LUA_REGISTRYINDEX);
        /* Auxiliary-name pushing may run normal GC and nested host calls.
         * Recheck the target immediately before the private raw write. */
        get_factory_slot(L,r);
        if(!lua_isnil(L,-1)){r->status=UE4SS_LUA_HOST_PENDING;return 0;}lua_pop(L,1);
        set_factory_slot(L,r);r->factory_published=1;
        if(r->private_registry_key)alias_named_factory_if_absent(L,r);
        else r->named_factory_published=1;
        return 0;
    case UIKIT_OBJECT_FACTORY_NAMED_ALIAS:
        if(!r->private_registry_key || !pinned_metatable(L,r)){
            r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        lua_pop(L,1);alias_named_factory_if_absent(L,r);return 0;
    case UIKIT_OBJECT_FACTORY_ALIAS:
        if(!r->private_registry_key || !pinned_metatable(L,r)){
            r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        lua_pop(L,1);
        get_factory_slot(L,r);
        if(!lua_isnil(L,-1) && (lua_type(L,-1)!=LUA_TTABLE || lua_topointer(L,-1)!=r->metatable_identity)){
            r->status=UE4SS_LUA_HOST_INVALID_HANDLE;return 0;}
        lua_pop(L,1);set_factory_slot(L,r);r->factory_published=1;return 0;
    case UIKIT_OBJECT_RELEASE:
        if(r->auxiliary_metatable_ref>0){luaL_unref(L,LUA_REGISTRYINDEX,r->auxiliary_metatable_ref);r->auxiliary_metatable_ref=0;}
        if(r->metatable_ref>0){luaL_unref(L,LUA_REGISTRYINDEX,r->metatable_ref);r->metatable_ref=0;}
        if(r->members_ref>0){luaL_unref(L,LUA_REGISTRYINDEX,r->members_ref);r->members_ref=0;}
        if(r->metamethods_ref>0){luaL_unref(L,LUA_REGISTRYINDEX,r->metamethods_ref);r->metamethods_ref=0;}
        return 0;
    }
    r->status=UE4SS_LUA_HOST_INTERNAL_ERROR;return 0;
}
int uikit_lua_object_run(lua_State* L,struct uikit_lua_object_request* r) {
    int top=uikit_lua_value_top(L),copy=r->operation==UIKIT_OBJECT_INSPECT ||
        r->operation==UIKIT_OBJECT_VERIFY || r->operation==UIKIT_OBJECT_COMMIT || r->operation==UIKIT_OBJECT_CAPTURE ||
        r->operation==UIKIT_OBJECT_FACTORY_COMMIT || r->operation==UIKIT_OBJECT_FACTORY_ATTACH;
    int slots=2+copy,status;lu_byte hooks;
    r->status=UE4SS_LUA_HOST_OK;
    if(L->stack_last.p-L->top.p<slots+1)return LUA_ERRMEM;
    if(L->ci->top.p<L->top.p+slots)L->ci->top.p=L->top.p+slots;
    hooks=L->allowhook;L->allowhook=0;
    lua_pushcfunction(L,perform);lua_pushlightuserdata(L,r);
    if(copy)lua_pushvalue(L,r->index);
    status=lua_pcall(L,1+copy,LUA_MULTRET,0);L->allowhook=hooks;
    if(status!=LUA_OK || r->status!=UE4SS_LUA_HOST_OK)uikit_lua_value_pop_to(L,top);
    return status;
}
