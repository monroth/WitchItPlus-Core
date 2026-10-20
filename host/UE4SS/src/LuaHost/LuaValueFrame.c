/* Compile as C against the EXACT bundled LuaRaw headers and runtime. The
 * private structures below are not compatible with an arbitrary system Lua.
 * This translation unit contains only POD frames and never enters consumers
 * except through the function already on Lua's stack in lua_pcall. */
#include "LuaValueFrame.h"
#include <lauxlib.h>
#include <lstate.h>
#include <lobject.h>
#include <limits.h>
#include <string.h>

const void* uikit_lua_value_frame_identity(lua_State* L) { return L->ci; }
int uikit_lua_value_top(lua_State* L) { return (int)(L->top.p - (L->ci->func.p + 1)); }
int uikit_lua_value_max_top(void) { return LUAI_MAXSTACK - LUA_MINSTACK - 8; }

int uikit_lua_value_normalize_index(int top, int index, int allow_absent) {
    long long absolute;
    if (index == 0 || index <= LUA_REGISTRYINDEX) return 0;
    absolute = index > 0 ? (long long)index : (long long)top + index + 1;
    if (absolute < 1 || absolute > INT_MAX) return allow_absent ? -1 : 0;
    if (absolute > top) return allow_absent ? -1 : 0;
    return (int)absolute;
}
int uikit_lua_value_can_pop(lua_State* L, int new_top) {
    StkId target;
    if (new_top < 0 || new_top > uikit_lua_value_top(L)) return 0;
    target = L->ci->func.p + 1 + new_top;
    return L->tbclist.p < target;
}
void uikit_lua_value_pop_to(lua_State* L, int new_top) {
    /* Never bypass __close: callers reject any range containing a tbc slot. */
    L->top.p = L->ci->func.p + 1 + new_top;
}
int uikit_lua_value_current_closure(lua_State* L, lua_CFunction* function, unsigned int* count) {
    TValue* value = s2v(L->ci->func.p);
    if (!ttisCclosure(value)) return 0;
    *function = clCvalue(value)->f;
    *count = clCvalue(value)->nupvalues;
    return 1;
}
int uikit_lua_value_is_table(lua_State* L, int normalized_index) {
    return normalized_index > 0 && normalized_index <= uikit_lua_value_top(L)
        && ttistable(s2v(L->ci->func.p + normalized_index));
}

static void read_exact(lua_State* L, int index, ue4ss_lua_value_v1* output) {
    switch (lua_type(L, index)) {
    case LUA_TNONE: case LUA_TNIL: output->kind = UE4SS_LUA_VALUE_NIL; break;
    case LUA_TBOOLEAN:
        output->kind = UE4SS_LUA_VALUE_BOOLEAN;
        output->boolean_value = (uint32_t)lua_toboolean(L, index); break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, index)) {
            output->kind = UE4SS_LUA_VALUE_INTEGER;
            output->integer_value = (int64_t)lua_tointeger(L, index);
        } else {
            output->kind = UE4SS_LUA_VALUE_NUMBER;
            output->number_value = (double)lua_tonumber(L, index);
        }
        break;
    case LUA_TSTRING: {
        size_t size;
        output->kind = UE4SS_LUA_VALUE_STRING;
        /* Exact string type: lua_tolstring cannot coerce/allocate here. */
        output->string_data = lua_tolstring(L, index, &size);
        output->string_size = (uint64_t)size;
        break;
    }
    case LUA_TTABLE: output->kind = UE4SS_LUA_VALUE_TABLE; break;
    case LUA_TFUNCTION: output->kind = UE4SS_LUA_VALUE_FUNCTION; break;
    default: output->kind = UE4SS_LUA_VALUE_OTHER; break;
    }
}

static int perform(lua_State* L) {
    struct uikit_lua_value_request* request =
        (struct uikit_lua_value_request*)lua_touserdata(L, 1);
    switch (request->operation) {
    case UIKIT_VALUE_READ: case UIKIT_VALUE_UPVALUE_READ:
        read_exact(L, 2, request->output);
        return 0;
    case UIKIT_VALUE_PUSH:
        if (!lua_checkstack(L, 1 + UIKIT_LUA_VALUE_BOOTSTRAP_RESERVE)) {
            request->status = UE4SS_LUA_HOST_INTERNAL_ERROR; return 0;
        }
        switch (request->input->kind) {
        case UE4SS_LUA_VALUE_NIL: lua_pushnil(L); break;
        case UE4SS_LUA_VALUE_BOOLEAN: lua_pushboolean(L, request->input->boolean_value != 0); break;
        case UE4SS_LUA_VALUE_INTEGER: lua_pushinteger(L, (lua_Integer)request->input->integer_value); break;
        case UE4SS_LUA_VALUE_NUMBER: lua_pushnumber(L, (lua_Number)request->input->number_value); break;
        case UE4SS_LUA_VALUE_STRING:
            lua_pushlstring(L, request->input->string_data ? request->input->string_data : "",
                            (size_t)request->input->string_size); break;
        case UE4SS_LUA_VALUE_TABLE: lua_createtable(L, 0, 0); break;
        default: request->status = UE4SS_LUA_HOST_INVALID_HANDLE; return 0;
        }
        return 1;
    case UIKIT_VALUE_TABLE_SET:
        if (!lua_checkstack(L, 2)) { request->status = UE4SS_LUA_HOST_INTERNAL_ERROR; return 0; }
        lua_pushlstring(L, request->key ? request->key : "", request->key_size);
        lua_pushvalue(L, 3);
        lua_rawset(L, 2);
        return 0;
    case UIKIT_VALUE_GROW:
        if (!lua_checkstack(L, request->count + UIKIT_LUA_VALUE_BOOTSTRAP_RESERVE)) {
            request->status = UE4SS_LUA_HOST_INTERNAL_ERROR; return 0;
        }
        { int i; for (i = 0; i < request->count; ++i) lua_pushnil(L); }
        return request->count;
    case UIKIT_VALUE_RESERVE:
        if (!lua_checkstack(L, request->count + UIKIT_LUA_VALUE_BOOTSTRAP_RESERVE))
            request->status = UE4SS_LUA_HOST_INTERNAL_ERROR;
        return 0;
    }
    request->status = UE4SS_LUA_HOST_INTERNAL_ERROR;
    return 0;
}

int uikit_lua_value_run(lua_State* L, struct uikit_lua_value_request* request) {
    const int old_top = uikit_lua_value_top(L);
    int copies = request->operation == UIKIT_VALUE_TABLE_SET ? 2 :
        ((request->operation == UIKIT_VALUE_READ || request->operation == UIKIT_VALUE_UPVALUE_READ) ? 1 : 0);
    const int slots = 2 + copies;
    int arguments = 1 + copies;
    int status;
    lu_byte old_allowhook;
    /* Direct private inspection does not allocate. LUA_EXTRA_STACK is not used. */
    if (L->stack_last.p - L->top.p < slots + 1) return LUA_ERRMEM;
    if (L->ci->top.p < L->top.p + slots) L->ci->top.p = L->top.p + slots;
    /* Internal frames are invisible to debug hooks. A return hook can replace
     * both the caller's string slot and this frame's copy, run GC, and otherwise
     * invalidate a just-read borrow before lua_pcall returns. The exact bundled
     * flag is saved/restored under the host gate, including on pcall errors.
     * The separate uikit_lua_value_call path keeps real callee hooks enabled. */
    old_allowhook = L->allowhook;
    L->allowhook = 0;
    lua_pushcfunction(L, perform);       /* zero-upvalue C function: no allocation */
    lua_pushlightuserdata(L, request);   /* no allocation */
    if (request->operation == UIKIT_VALUE_READ || request->operation == UIKIT_VALUE_TABLE_SET)
        lua_pushvalue(L, request->index); /* original positive ordinary slot */
    else if (request->operation == UIKIT_VALUE_UPVALUE_READ)
        lua_pushvalue(L, lua_upvalueindex(request->index));
    if (request->operation == UIKIT_VALUE_TABLE_SET) lua_pushvalue(L, old_top);
    status = lua_pcall(L, arguments, LUA_MULTRET, 0);
    L->allowhook = old_allowhook;
    if (status != LUA_OK || request->status != UE4SS_LUA_HOST_OK)
        uikit_lua_value_pop_to(L, old_top);
    return status;
}

int uikit_lua_value_call(lua_State* L, int nargs, int nresults) {
    StkId desired = L->top.p - nargs + nresults;
    /* reserve was protected; establish caller result capacity without allocating */
    if (desired > L->stack_last.p) return LUA_ERRMEM;
    if (L->ci->top.p < desired) L->ci->top.p = desired;
    return lua_pcall(L, nargs, nresults, 0);
}
