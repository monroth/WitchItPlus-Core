#pragma once
#include <LuaHost/Lifecycle.hpp>
namespace RC::LuaHost {
bool install_lms_auxiliary_bindings() noexcept;
void revoke_lms_auxiliary_module(ModuleHandle) noexcept;
void revoke_lms_auxiliary_vm(VmHandle) noexcept;
}
