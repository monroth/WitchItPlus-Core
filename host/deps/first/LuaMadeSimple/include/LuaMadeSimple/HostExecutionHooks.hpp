#pragma once
#include <LuaMadeSimple/Common.hpp>
#include <cstdint>
struct lua_State;
namespace RC::LuaMadeSimple {
// Installed once by the host before executing Lua. Enter performs pure cached
// root resolution + VM admission before acquiring the actual bundled gate.
// A ticket grants no consumer context or module authority.
struct HostExecutionHooks {
    std::uint32_t (*Enter)(lua_State* actual, lua_State* cached_root, void** ticket) noexcept{};
    std::uint32_t (*EnterFinalizer)(lua_State* actual, lua_State* cached_root, void** ticket) noexcept{};
    void (*Leave)(void* ticket) noexcept{};
    // Called only under an inherited real scope/gate; prevents nonlocal errors
    // escaping through a different thread's active external Lua error handler.
    bool (*ValidateActual)(lua_State* actual,lua_State* root) noexcept{};
};
RC_LMS_API bool install_host_execution_hooks(const HostExecutionHooks&) noexcept;
RC_LMS_API bool has_host_execution_scope(lua_State* cached_root) noexcept;
RC_LMS_API bool host_actual_execution_allowed(lua_State*,lua_State*) noexcept;
class RC_LMS_API LuaExecutionScope {
    void* Ticket{};
    lua_State* Root{};
    LuaExecutionScope* Previous{};
    bool Owner{};
    bool Accessible{true};
protected:
    LuaExecutionScope(lua_State* actual, lua_State* cached_root, bool finalizing);
public:
    LuaExecutionScope(lua_State* actual, lua_State* cached_root);
    LuaExecutionScope(const LuaExecutionScope&) = delete;
    LuaExecutionScope& operator=(const LuaExecutionScope&) = delete;
    ~LuaExecutionScope();
    static bool Contains(lua_State* root) noexcept;
    static lua_State* ActiveRoot() noexcept;
    // Actual lua_close returned; keep the global gate while pure after-close
    // bookkeeping runs, but stop admitting any access to the freed state.
    void close_completed() noexcept { Accessible = false; }
};
// Only the trusted VM finalizer can acquire this scope. It covers the actual
// lua_close and its host GC callbacks until state memory has been freed.
class RC_LMS_API LuaFinalizationScope final : public LuaExecutionScope {
public:
    LuaFinalizationScope(lua_State* actual, lua_State* cached_root)
        : LuaExecutionScope(actual, cached_root, true) {}
};
}
