/* Exact bundled LuaRaw; all allocating Lua calls remain in protected POD C. */
#include "LuaTokenFrame.h"
#include "LuaValueFrame.h"
#include <lauxlib.h>
#include <lstate.h>
#include <string.h>
static char TokenMetaKey;
#define TOKEN_MAGIC UINT64_C(0x5545345353544f4b)
struct token_payload { uint64_t magic; void* domain; uint64_t record_id; ue4ss_lua_token_v1 token; };
static int token_gc(lua_State* L) {
    struct token_payload* payload;
    void* domain = lua_touserdata(L, lua_upvalueindex(1));
    void (*notify)(void*, uint64_t, const void*) = NULL;
    if (lua_type(L, lua_upvalueindex(2)) != LUA_TUSERDATA || lua_rawlen(L, lua_upvalueindex(2)) != sizeof(notify)) return 0;
    memcpy(&notify, lua_touserdata(L, lua_upvalueindex(2)), sizeof(notify));
    if (lua_type(L, 1) != LUA_TUSERDATA || lua_rawlen(L, 1) != sizeof(*payload)) return 0;
    payload = (struct token_payload*)lua_touserdata(L, 1);
    if (!notify || payload->magic != TOKEN_MAGIC || payload->domain != domain) return 0;
    notify(domain, payload->record_id, payload); payload->magic = 0; return 0;
}
static int valid_meta(lua_State* L, int index, void* domain, void (*notify)(void*, uint64_t, const void*)) {
    void (*stored)(void*, uint64_t, const void*) = NULL; int valid = 0;
    index = lua_absindex(L, index); lua_pushliteral(L, "__gc"); lua_rawget(L, index);
    if (lua_iscfunction(L, -1) && lua_tocfunction(L, -1) == token_gc && lua_getupvalue(L, -1, 1)) {
        const int same = lua_type(L, -1) == LUA_TLIGHTUSERDATA && lua_touserdata(L, -1) == domain; lua_pop(L, 1);
        if (same && lua_getupvalue(L, -1, 2)) {
            if (lua_type(L, -1) == LUA_TUSERDATA && lua_rawlen(L, -1) == sizeof(stored)) {
                memcpy(&stored, lua_touserdata(L, -1), sizeof(stored)); valid = stored == notify;
            } lua_pop(L, 1);
        }
    } lua_pop(L, 1); return valid;
}
static int perform(lua_State* L) {
    struct uikit_lua_token_request* r = (struct uikit_lua_token_request*)lua_touserdata(L, 1);
    struct token_payload* p;
    if (!lua_checkstack(L, 10)) { r->status = UE4SS_LUA_HOST_INTERNAL_ERROR; return 0; }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &TokenMetaKey);
    if (r->operation == UIKIT_TOKEN_PUSH) {
        if (lua_isnil(L, -1)) {
            void* storage; lua_pop(L, 1); lua_createtable(L, 0, 3);
            lua_pushlightuserdata(L, r->domain); storage = lua_newuserdatauv(L, sizeof(r->collected), 0);
            memcpy(storage, &r->collected, sizeof(r->collected));
            lua_pushcclosure(L, token_gc, 2); lua_setfield(L, -2, "__gc");
            lua_pushliteral(L, "UE4SS.UIKitToken.v1"); lua_setfield(L, -2, "__metatable");
            lua_pushliteral(L, "UE4SS.UIKitToken.v1"); lua_setfield(L, -2, "__name");
            lua_pushvalue(L, -1); lua_rawsetp(L, LUA_REGISTRYINDEX, &TokenMetaKey);
        }
        if (!lua_istable(L, -1) || !valid_meta(L, -1, r->domain, r->collected)) { r->status = UE4SS_LUA_HOST_WRONG_OWNER; return 0; }
        p = (struct token_payload*)lua_newuserdatauv(L, sizeof(*p), 0);
        p->magic = TOKEN_MAGIC; p->domain = r->domain; p->record_id = r->record_id; p->token = r->token; r->address = p;
        lua_pushvalue(L, -2); lua_setmetatable(L, -2); return 1;
    }
    if (lua_type(L, 2) != LUA_TUSERDATA || lua_rawlen(L, 2) != sizeof(*p) ||
        !lua_istable(L, -1) || !valid_meta(L, -1, r->domain, r->collected)) return 0;
    if (!lua_getmetatable(L, 2) || !lua_rawequal(L, -1, -2)) return 0;
    p = (struct token_payload*)lua_touserdata(L, 2);
    if (p->magic != TOKEN_MAGIC || p->domain != r->domain) return 0;
    r->recognized = 1; r->record_id = p->record_id; r->address = p; r->token = p->token; return 0;
}
int uikit_lua_token_run(lua_State* L, struct uikit_lua_token_request* r) {
    const int top = uikit_lua_value_top(L), read = r->operation != UIKIT_TOKEN_PUSH, slots = read ? 3 : 2;
    int status; lu_byte old;
    if (L->stack_last.p - L->top.p < slots + 1) return LUA_ERRMEM;
    if (L->ci->top.p < L->top.p + slots) L->ci->top.p = L->top.p + slots;
    old = L->allowhook; L->allowhook = 0;
    lua_pushcfunction(L, perform); lua_pushlightuserdata(L, r);
    if (r->operation == UIKIT_TOKEN_READ) lua_pushvalue(L, r->index);
    else if (r->operation == UIKIT_TOKEN_UPVALUE_READ) lua_pushvalue(L, lua_upvalueindex(r->index));
    status = lua_pcall(L, read ? 2 : 1, LUA_MULTRET, 0); L->allowhook = old;
    if (status != LUA_OK || r->status != UE4SS_LUA_HOST_OK) uikit_lua_value_pop_to(L, top);
    return status;
}
