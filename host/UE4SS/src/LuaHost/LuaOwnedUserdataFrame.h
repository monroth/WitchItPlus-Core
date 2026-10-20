#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <lua.h>
#include <stddef.h>
#include <stdint.h>
enum { UIKIT_OWNED_ALLOC=1, UIKIT_OWNED_ARM=2, UIKIT_OWNED_COMMIT=3,
       UIKIT_OWNED_GET=4, UIKIT_OWNED_REMOVE=5, UIKIT_OWNED_READ_GC=6 };
struct uikit_owned_userdata_request {
    lua_State* actual;
    int action, values, reference, top;
    size_t size, observed_size;
    uint64_t ticket;
    void* storage;
    int (*expected_gc)(lua_State*);
};
int uikit_owned_userdata_run(lua_State*,struct uikit_owned_userdata_request*);
int uikit_owned_userdata_gc(lua_State*);
// Private HOST symbol; a stable C boundary raises Lua error only after C++ owners
// have returned. Dispatch never runs Native instructions without its real pin.
int uikit_owned_userdata_authorize_gc(lua_State*);
int uikit_owned_userdata_dispatch_gc(lua_State*,void*,size_t);
#ifdef __cplusplus
}
#endif
