#define NOMINMAX

#include <filesystem>
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/SysError.hpp>
#include <Helpers/String.hpp>
#include <Mod/CppMod.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>

namespace RC
{
    CppMod::CppMod(UE4SSProgram& program, StringType&& mod_name, StringType&& mod_path) : Mod(program, std::move(mod_name), std::move(mod_path))
    {
        m_dlls_path = m_mod_path / STR("dlls");

        if (!std::filesystem::exists(m_dlls_path))
        {
            Output::send<LogLevel::Warning>(STR("Could not find the dlls folder for mod {}\n"), m_mod_name);
            set_installable(false);
            return;
        }

        auto dll_path = m_dlls_path / STR("main.dll");
        if (!std::filesystem::exists(dll_path))
        {
            dll_path = m_dlls_path / fmt::format(STR("{}.dll"), mod_name);

            if (!std::filesystem::exists(dll_path))
            {
                Output::send<LogLevel::Warning>(STR("Failed to load C++ mod {}, dlls folder must contain either main.dll or {}\n"),
                                                m_mod_name, ensure_str(dll_path.filename()));
                set_installable(false);
                return;
            }
        }

        m_dll_filename = ensure_str(dll_path.filename());

        // Add mods dlls directory to search path for dynamic/shared linked libraries in mods
        m_dlls_path_cookie = AddDllDirectory(m_dlls_path.c_str());
        m_main_dll_module = LoadLibraryExW(dll_path.c_str(), NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

        if (!m_main_dll_module)
        {
            Output::send<LogLevel::Warning>(STR("Failed to load dll <{}> for mod {}, error: {}\n"),
                                            ensure_str(dll_path), m_mod_name, SysError(GetLastError()).c_str());
            set_installable(false);
            return;
        }

        m_start_mod_func = reinterpret_cast<start_type>(GetProcAddress(m_main_dll_module, "start_mod"));
        m_uninstall_mod_func = reinterpret_cast<uninstall_type>(GetProcAddress(m_main_dll_module, "uninstall_mod"));

        if (!m_start_mod_func || !m_uninstall_mod_func)
        {
            Output::send<LogLevel::Warning>(STR("Failed to find exported mod lifecycle functions for mod {}\n"), m_mod_name);

            FreeLibrary(m_main_dll_module);
            m_main_dll_module = NULL;

            set_installable(false);
            return;
        }
        auto owner = LuaHost::HostBindings::instance().attach_module(m_main_dll_module);
        if (owner.status != LuaHost::Status::ok)
        {
            FreeLibrary(m_main_dll_module);
            m_main_dll_module = NULL;
            set_installable(false);
            return;
        }
        m_host_module = owner.value;
    }

    auto CppMod::start_mod() -> void
    {
        try
        {
            LuaHost::HostBindings::instance().api().dispatch_code(m_host_module, [&] {
                m_mod.store(m_start_mod_func(), std::memory_order_release);
            });
            m_is_started = m_mod.load(std::memory_order_acquire) != nullptr;
        }
        catch (std::exception& e)
        {
            if (!Output::has_internal_error())
            {
                Output::send<LogLevel::Warning>(STR("Failed to load dll <{}> for mod {}, because: {}\n"),
                                                ensure_str((m_dlls_path / m_dll_filename)),
                                                m_mod_name,
                                                ensure_str(e.what()));
            }
            else
            {
                printf_s("Internal Error: %s\n", e.what());
            }
        }
    }

    auto CppMod::host_retirement_complete() const -> bool
    {
        return !m_host_module.id || m_host_retired.load(std::memory_order_acquire);
    }

    auto CppMod::host_retirement_requested() const -> bool
    {
        return !host_retirement_complete() &&
               LuaHost::HostBindings::instance().module_is_retiring(m_host_module);
    }

    auto CppMod::run_code(const std::function<void(CppUserModBase&)>& callback) -> void
    {
        LuaHost::HostBindings::instance().api().dispatch_code(m_host_module, [&] {
            if (auto* mod = m_mod.load(std::memory_order_acquire)) callback(*mod);
        });
    }

    auto CppMod::run_lua(const LuaMadeSimple::Lua& lua, const std::function<void(CppUserModBase&)>& callback) -> void
    {
        auto& host = LuaHost::HostBindings::instance();
        auto* root = lua.get_root_lua_state();
        auto vm = host.find_vm(root);
        if (vm.status != LuaHost::Status::ok) return;
        auto stack = host.stack_binding(root, lua.get_lua_state());
        if (stack.status != LuaHost::Status::ok) return;
        host.api().dispatch_lua(vm.value, m_host_module, stack.value, [&](auto) {
            if (auto* mod = m_mod.load(std::memory_order_acquire)) callback(*mod);
            return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
        });
    }

    auto CppMod::uninstall() -> void
    {
        if (host_retirement_complete() || m_host_uninstall_failed || m_host_uninstall_running) return;
        auto& host = LuaHost::HostBindings::instance();
        // Pending returns immediately. The program keeps the CppMod object and
        // DLL alive and retries on its event thread after existing leases drain.
        if (host.claim_module_retire(m_host_module) != LuaHost::Status::ready) return;
        struct Running {
            bool& flag;
            explicit Running(bool& value) : flag(value) { flag = true; }
            ~Running() { flag = false; }
        } running{m_host_uninstall_running};
        bool cleaned{};
        host.api().dispatch_finalizer(m_host_module, [&] {
            if (host.cleanup_module(m_host_module) != LuaHost::Status::ok) return;
            cleaned = true;
            Output::send(STR("Stopping C++ mod '{}' for uninstall\n"), m_mod_name);
            m_is_started = false;
            if (!m_host_uninstall_called)
            {
                m_host_uninstall_called = true;
                try
                {
                    if (auto* mod = m_mod.load(std::memory_order_acquire); mod && m_uninstall_mod_func)
                        m_uninstall_mod_func(mod);
                    m_mod.store(nullptr, std::memory_order_release);
                }
                catch (...)
                {
                    // Never retry a potentially partial consumer deletion or unload
                    // its code after an unconfirmed teardown.
                    m_host_uninstall_failed = true;
                    Output::send<LogLevel::Error>(STR("C++ mod '{}' threw during uninstall; DLL retained\n"), m_mod_name);
                    return;
                }
            }
            if (!cleaned || m_host_uninstall_failed) return;
            if (m_main_dll_module)
            {
                if (!FreeLibrary(m_main_dll_module))
                {
                    m_host_uninstall_failed = true;
                    Output::send<LogLevel::Error>(STR("C++ mod '{}' DLL unload failed; finalization retained\n"), m_mod_name);
                    return;
                }
                m_main_dll_module = NULL;
            }
            if (m_dlls_path_cookie)
            {
                RemoveDllDirectory(m_dlls_path_cookie);
                m_dlls_path_cookie = nullptr;
            }
        });
        if (!cleaned || m_host_uninstall_failed) return;
        m_start_mod_func = nullptr;
        m_uninstall_mod_func = nullptr;
        if (host.finish_module_retire(m_host_module) == LuaHost::Status::ok)
            m_host_retired.store(true, std::memory_order_release);
    }

    auto CppMod::fire_on_lua_start(StringViewType mod_name,
                                   LuaMadeSimple::Lua& lua,
                                   LuaMadeSimple::Lua& main_lua,
                                   LuaMadeSimple::Lua& async_lua,
                                   LuaMadeSimple::Lua* hook_lua) -> void
    {
        run_lua(lua, [&](CppUserModBase& mod) {
            // Call new API
            mod.on_lua_start(mod_name, lua, main_lua, async_lua, hook_lua);

            ue4ss_lua_host_context_info_v1 active{1, sizeof(active), {}, {}, {}};
            if (LuaHost::HostApiRuntime::table().current_context(&active) != UE4SS_LUA_HOST_OK) return;
            // Call old deprecated API for backwards compatibility
            std::vector<LuaMadeSimple::Lua*> hook_luas;
            if (hook_lua)
            {
                hook_luas.push_back(hook_lua);
            }
            mod.on_lua_start(mod_name, lua, main_lua, async_lua, hook_luas);
        });
    }

    auto CppMod::fire_on_lua_start(LuaMadeSimple::Lua& lua,
                                   LuaMadeSimple::Lua& main_lua,
                                   LuaMadeSimple::Lua& async_lua,
                                   LuaMadeSimple::Lua* hook_lua) -> void
    {
        run_lua(lua, [&](CppUserModBase& mod) {
            // Call new API
            mod.on_lua_start(lua, main_lua, async_lua, hook_lua);

            ue4ss_lua_host_context_info_v1 active{1, sizeof(active), {}, {}, {}};
            if (LuaHost::HostApiRuntime::table().current_context(&active) != UE4SS_LUA_HOST_OK) return;
            // Call old deprecated API for backwards compatibility
            std::vector<LuaMadeSimple::Lua*> hook_luas;
            if (hook_lua)
            {
                hook_luas.push_back(hook_lua);
            }
            mod.on_lua_start(lua, main_lua, async_lua, hook_luas);
        });
    }

    auto CppMod::fire_on_lua_stop(StringViewType mod_name,
                                  LuaMadeSimple::Lua& lua,
                                  LuaMadeSimple::Lua& main_lua,
                                  LuaMadeSimple::Lua& async_lua,
                                  LuaMadeSimple::Lua* hook_lua) -> void
    {
        run_lua(lua, [&](CppUserModBase& mod) {
            // Call new API
            mod.on_lua_stop(mod_name, lua, main_lua, async_lua, hook_lua);

            ue4ss_lua_host_context_info_v1 active{1, sizeof(active), {}, {}, {}};
            if (LuaHost::HostApiRuntime::table().current_context(&active) != UE4SS_LUA_HOST_OK) return;
            // Call old deprecated API for backwards compatibility
            std::vector<LuaMadeSimple::Lua*> hook_luas;
            if (hook_lua)
            {
                hook_luas.push_back(hook_lua);
            }
            mod.on_lua_stop(mod_name, lua, main_lua, async_lua, hook_luas);
        });
    }

    auto CppMod::fire_on_lua_stop(LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua& main_lua, LuaMadeSimple::Lua& async_lua, LuaMadeSimple::Lua* hook_lua) -> void
    {
        run_lua(lua, [&](CppUserModBase& mod) {
            // Call new API
            mod.on_lua_stop(lua, main_lua, async_lua, hook_lua);

            ue4ss_lua_host_context_info_v1 active{1, sizeof(active), {}, {}, {}};
            if (LuaHost::HostApiRuntime::table().current_context(&active) != UE4SS_LUA_HOST_OK) return;
            // Call old deprecated API for backwards compatibility
            std::vector<LuaMadeSimple::Lua*> hook_luas;
            if (hook_lua)
            {
                hook_luas.push_back(hook_lua);
            }
            mod.on_lua_stop(lua, main_lua, async_lua, hook_luas);
        });
    }

    auto CppMod::fire_unreal_init() -> void
    {
        run_code([&](CppUserModBase& mod) {
            mod.on_unreal_init();
        });
    }

    auto CppMod::fire_ui_init() -> void
    {
        run_code([&](CppUserModBase& mod) {
            mod.on_ui_init();
        });
    }

    auto CppMod::fire_program_start() -> void
    {
        run_code([&](CppUserModBase& mod) {
            mod.on_program_start();
        });
    }

    auto CppMod::fire_update() -> void
    {
        run_code([&](CppUserModBase& mod) {
            mod.on_update();
        });
    }

    auto CppMod::fire_dll_load(StringViewType dll_name) -> void
    {
        run_code([&](CppUserModBase& mod) {
            mod.on_dll_load(dll_name);
        });
    }

    auto CppMod::fire_on_cpp_mods_loaded() -> void
    {
        run_code([&](CppUserModBase& mod) {
            mod.on_cpp_mods_loaded();
        });
    }

    CppMod::~CppMod()
    {
        if (!host_retirement_complete()) uninstall();
        // Program deletion paths check completion before deleting this object.
        // Fail closed even if an external legacy deletion violates that rule.
        if (m_main_dll_module && !m_host_module.id)
        {
            FreeLibrary(m_main_dll_module);
            m_main_dll_module = NULL;
        }
        if (m_main_dll_module)
        {
            Output::send<LogLevel::Error>(STR("C++ mod '{}' destroyed before leases drained; DLL retained\n"), m_mod_name);
        }
        else if (m_dlls_path_cookie)
        {
            RemoveDllDirectory(m_dlls_path_cookie);
            m_dlls_path_cookie = nullptr;
        }
    }
} // namespace RC
