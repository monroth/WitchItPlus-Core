#include <LuaHost/HostApiRuntime.hpp>

extern "C" UE4SS_UIKIT_LUA_HOST_EXPORT ue4ss_lua_host_status UE4SS_LUA_HOST_CALL
UE4SS_GetUIKitLuaHostApi_v1(uint32_t version, uint32_t minimum_size,
                           const ue4ss_uikit_lua_host_api_v1** output)
{
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = nullptr;
    auto status = ue4ss_lua_host_validate_prefix(version, minimum_size,
                                               UE4SS_UIKIT_LUA_HOST_PREFIX_SIZE);
    if (status != UE4SS_LUA_HOST_OK) return status;
    const auto& table = RC::LuaHost::HostApiRuntime::table();
    if (minimum_size > table.struct_size) return UE4SS_LUA_HOST_UNSUPPORTED_SIZE;
    *output = &table;
    return UE4SS_LUA_HOST_OK;
}
