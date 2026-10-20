#pragma once
namespace RC::LuaHost {
// Host program bootstrap, before consumer DLL loading/registration.
bool install_lua_host_integration() noexcept;
// Actual OS image identity; no fn execution and no guessed std::function layout.
void* code_image(const void* address) noexcept;
void* host_code_image() noexcept;
}
