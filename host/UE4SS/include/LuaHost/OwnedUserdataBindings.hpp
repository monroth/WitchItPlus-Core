#pragma once
#include <LuaHost/Lifecycle.hpp>
struct lua_State;
namespace RC::LuaHost {
bool install_owned_userdata_bindings() noexcept;
// Called under the actual exclusive VM finalization admission, BEFORE lua_close.
// Every Native destruction completes or close remains blocked; no lost record.
Status cleanup_owned_userdata_vm(VmHandle,lua_State*) noexcept;
Status forget_owned_userdata_vm(VmHandle) noexcept;
}
