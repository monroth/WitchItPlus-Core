#ifndef UIKIT_LUA_OBJECT_FRAME_H
#define UIKIT_LUA_OBJECT_FRAME_H
#include <LuaHost/UIKitLuaHost.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
enum uikit_lua_object_operation { UIKIT_OBJECT_INSPECT,UIKIT_OBJECT_VERIFY,UIKIT_OBJECT_ALLOCATE,UIKIT_OBJECT_COMMIT,UIKIT_OBJECT_CAPTURE,UIKIT_OBJECT_RELEASE,UIKIT_OBJECT_FACTORY_LOOKUP,UIKIT_OBJECT_FACTORY_BUILD,UIKIT_OBJECT_FACTORY_COMMIT,UIKIT_OBJECT_FACTORY_ATTACH,UIKIT_OBJECT_FACTORY_ALIAS,UIKIT_OBJECT_FACTORY_NAMED_ALIAS };
enum uikit_lua_object_pair_kind {UIKIT_OBJECT_PAIR_FUNCTION,UIKIT_OBJECT_PAIR_RAW_FUNCTION,UIKIT_OBJECT_PAIR_BOOLEAN};
struct uikit_lua_object_pair {
    const char* key;size_t key_size;
    enum uikit_lua_object_pair_kind kind;
    lua_Integer id,function_kind;
    lua_CFunction function;
    int boolean;
};
struct uikit_lua_object_request {
    enum uikit_lua_object_operation operation;
    int index;
    size_t size;
    size_t alignment;
    lua_Integer storage_tag;
    int polymorphic;
    int metatable_ref,members_ref,metamethods_ref;
    const void* metatable_identity;
    const void* members_identity;
    const void* metamethods_identity;
    lua_CFunction destructor;
    size_t metamethods_size,metamethods_alignment;
    const void* metamethods_metatable_identity;
    lua_CFunction metamethods_destructor;
    void* storage;
    int published;
    const char* factory_name;
    const void* private_registry_key;
    const struct uikit_lua_object_pair* member_pairs;
    size_t member_count;
    const struct uikit_lua_object_pair* metatable_pairs;
    size_t metatable_count;
    int factory_found;
    int auxiliary_metatable_ref;
    void* auxiliary_storage;
    int auxiliary_published;
    int factory_published;
    int named_factory_published;
    ue4ss_lua_host_status status;
};
/* Pure C, exact bundled LuaRaw, POD request owned ABOVE lua_pcall. Bootstrap
 * uses existing physical stack capacity; it never reallocates outside pcall. */
int uikit_lua_object_run(lua_State*,struct uikit_lua_object_request*);
#ifdef __cplusplus
}
#endif
#endif
