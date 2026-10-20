#pragma once

#include <vector>
#include <atomic>
#include <functional>
#include <LuaHost/Lifecycle.hpp>

#include <Unreal/Core/Windows/MinimalWindowsApi.hpp>

#include <Mod/CppUserModBase.hpp>
#include <Mod/Mod.hpp>

#include <String/StringType.hpp>

namespace RC
{
    namespace LuaMadeSimple
    {
        class Lua;
    }

    class CppMod : public Mod
    {
      private:
        typedef CppUserModBase* (*start_type)();
        typedef void (*uninstall_type)(CppUserModBase*);

      private:
        StringType m_dll_filename{};
        std::filesystem::path m_dlls_path;

        Unreal::Windows::HMODULE m_main_dll_module = NULL;
        void* m_dlls_path_cookie = NULL;
        start_type m_start_mod_func = nullptr;
        uninstall_type m_uninstall_mod_func = nullptr;

        std::atomic<CppUserModBase*> m_mod{nullptr};
        LuaHost::ModuleHandle m_host_module{};
        std::atomic_bool m_host_retired{false};
        bool m_host_uninstall_called{};
        bool m_host_uninstall_running{};
        bool m_host_uninstall_failed{};
        auto run_code(const std::function<void(CppUserModBase&)>&) -> void;
        auto run_lua(const LuaMadeSimple::Lua&, const std::function<void(CppUserModBase&)>&) -> void;

      public:
        CppMod(UE4SSProgram&, StringType&& mod_name, StringType&& mod_path);
        CppMod(CppMod&) = delete;
        CppMod(CppMod&&) = delete;
        ~CppMod() override;

      public:
        auto start_mod() -> void override;
        auto uninstall() -> void override;
        auto host_retirement_complete() const -> bool;
        auto host_retirement_requested() const -> bool;

        auto fire_on_lua_start(StringViewType mod_name,
                               LuaMadeSimple::Lua& lua,
                               LuaMadeSimple::Lua& main_lua,
                               LuaMadeSimple::Lua& async_lua,
                               LuaMadeSimple::Lua* hook_lua) -> void;

        auto fire_on_lua_start(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua, LuaMadeSimple::Lua* hook_lua) -> void;

        auto fire_on_lua_stop(StringViewType mod_name,
                              LuaMadeSimple::Lua& lua,
                              LuaMadeSimple::Lua& main_lua,
                              LuaMadeSimple::Lua& async_lua,
                              LuaMadeSimple::Lua* hook_lua) -> void;

        auto fire_on_lua_stop(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua, LuaMadeSimple::Lua* hook_lua) -> void;

        auto fire_unreal_init() -> void override;
        auto fire_ui_init() -> void override;
        auto fire_program_start() -> void override;
        auto fire_update() -> void override;
        auto fire_dll_load(StringViewType dll_name) -> void;
        auto fire_on_cpp_mods_loaded() -> void;
    };
} // namespace RC
