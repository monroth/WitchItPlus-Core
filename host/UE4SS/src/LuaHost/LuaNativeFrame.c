#include "LuaNativeFrame.h"
#include <lstate.h>
#include <ldo.h>
#if defined(_MSC_VER)
static __declspec(thread) lua_State* ProtectedActual;
#else
static _Thread_local lua_State* ProtectedActual;
#endif
lua_State* uikit_lua_native_current_actual(void) { return ProtectedActual; }
struct native_request { void* opaque; void (*invoke)(void*); };
static void invoke_native(lua_State* L, void* opaque) {
    struct native_request* request = opaque;
    /* Match LuaRaw's C-call lock protocol. lua_error/allocation failure throws
     * with its API lock held; luaD_pcall catches that acquisition. Existing
     * outer host serialization stays held throughout Native execution. */
    lua_unlock(L);
    request->invoke(request->opaque);
    lua_lock(L);
}
int uikit_lua_native_run(lua_State* L, void* opaque, void (*invoke)(void*)) {
    struct native_request request = {opaque, invoke};
    int status;
    lua_State* previous;
    ptrdiff_t oldtop;
    lua_lock(L);
    oldtop = savestack(L, L->top.p);
    /* Crossing this C++ callback ABI is non-yieldable, like normal lua_pcall. */
    incnny(L);
    previous=ProtectedActual;ProtectedActual=L;
    status = luaD_pcall(L, invoke_native, &request, oldtop, L->errfunc);
    ProtectedActual=previous;
    decnny(L);
    lua_unlock(L);
    return status;
}
