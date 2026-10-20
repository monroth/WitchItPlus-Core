#include <LuaHost/UIKitLuaHost.h>

#include <stddef.h>
#include <stdio.h>

_Static_assert(sizeof(ue4ss_lua_vm_handle) == 2 * sizeof(uint64_t), "VM handle contains ID and generation");
_Static_assert(sizeof(ue4ss_lua_module_handle) == 2 * sizeof(uint64_t), "module handle contains ID and generation");
_Static_assert(sizeof(ue4ss_lua_context_handle) == 2 * sizeof(uint64_t), "context handle contains ID and generation");
_Static_assert(sizeof(ue4ss_lua_reference_handle) == 2 * sizeof(uint64_t), "reference handle contains ID and generation");
_Static_assert(offsetof(ue4ss_uikit_lua_host_api_v1, abi_version) == 0, "ABI version starts the table");
_Static_assert(offsetof(ue4ss_uikit_lua_host_api_v1, struct_size) == sizeof(uint32_t), "table has a size prefix");

int main(void)
{
    const uint32_t required = (uint32_t)sizeof(ue4ss_uikit_lua_host_api_v1);
    if (ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, required, required) != UE4SS_LUA_HOST_OK ||
        ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, required - 1, required) !=
            UE4SS_LUA_HOST_UNSUPPORTED_SIZE ||
        ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, 0, 0) !=
            UE4SS_LUA_HOST_UNSUPPORTED_SIZE ||
        ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, 0, required) !=
            UE4SS_LUA_HOST_UNSUPPORTED_SIZE ||
        ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, 7, 0) !=
            UE4SS_LUA_HOST_UNSUPPORTED_SIZE ||
        ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, 8, 0) != UE4SS_LUA_HOST_OK ||
        ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION + 1, required, required) !=
            UE4SS_LUA_HOST_UNSUPPORTED_ABI)
    {
        fputs("FAIL C11 ABI header and prefix validation\n", stderr);
        return 1;
    }
    puts("PASS C11 ABI header and prefix validation");
    return 0;
}
