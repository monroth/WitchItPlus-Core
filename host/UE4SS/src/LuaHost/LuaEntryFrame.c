/* Host-only identity inspection after leases + the existing embedded gate.
 * Compile against the exact bundled LuaRaw. No consumer supplied state. */
#include "LuaEntryFrame.h"
#include <lstate.h>
int uikit_lua_entry_matches_root(lua_State* actual, lua_State* root) {
    return G(actual)->mainthread == root;
}
int uikit_lua_entry_reserve(lua_State* actual) {
    struct uikit_lua_value_request request = {0};
    request.operation = UIKIT_VALUE_RESERVE;
    request.count = 0;
    return uikit_lua_value_run(actual, &request);
}
