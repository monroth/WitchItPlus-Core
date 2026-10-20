#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <cassert>
namespace RC::LuaMadeSimple {
namespace {
std::mutex PolicyMutex;
HostExecutionHooks Hooks;
std::atomic<bool> Installed{};
// Internal TLS has no DLL data interface; only scope methods cross the DLL boundary.
thread_local LuaExecutionScope* CurrentExecution{};
}
bool install_host_execution_hooks(const HostExecutionHooks& hooks) noexcept {
    if (!hooks.Enter || !hooks.EnterFinalizer || !hooks.Leave) return false;
    try {
        std::lock_guard lock{PolicyMutex};
        if (Installed.load(std::memory_order_relaxed)) return false;
        Hooks = hooks;
        Installed.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
bool LuaExecutionScope::Contains(lua_State* root) noexcept {
    if (!root) return false;
    for (auto* current = CurrentExecution; current; current = current->Previous)
        if (current->Accessible && current->Root == root) return true;
    return false;
}
lua_State* LuaExecutionScope::ActiveRoot() noexcept {
    for(auto* current=CurrentExecution;current;current=current->Previous)
        if(current->Accessible) return current->Root;
    return nullptr;
}
bool has_host_execution_scope(lua_State* root) noexcept { return LuaExecutionScope::Contains(root); }
bool host_actual_execution_allowed(lua_State* actual,lua_State* root) noexcept {
    return actual && LuaExecutionScope::Contains(root) &&
        Installed.load(std::memory_order_acquire) && Hooks.ValidateActual && Hooks.ValidateActual(actual,root);
}
LuaExecutionScope::LuaExecutionScope(lua_State* actual, lua_State* root)
    : LuaExecutionScope(actual, root, false) {}
LuaExecutionScope::LuaExecutionScope(lua_State* actual, lua_State* root, bool finalizing) : Root(root) {
    if (!actual || !root) throw std::runtime_error("Lua execution has no cached actual VM identity");
    // Nested LMS calls borrow the protected outer call. Do not install a new
    // TLS stack frame or acquire a token that luaL_error/yield could jump over.
    if (!finalizing && Contains(root)) return;
    if (!Installed.load(std::memory_order_acquire))
        throw std::runtime_error("Lua execution lifetime policy is not installed");
    const auto enter = finalizing ? Hooks.EnterFinalizer : Hooks.Enter;
    if (enter(actual, root, &Ticket) != 0 || !Ticket)
        throw std::runtime_error("Host refused unbound/closing Lua execution");
    Previous = CurrentExecution;
    CurrentExecution = this;
    Owner = true;
}
LuaExecutionScope::~LuaExecutionScope() {
    if (!Owner) return;
    assert(CurrentExecution == this);
    CurrentExecution = Previous;
    Hooks.Leave(Ticket); // release gate first, VM lifetime last
}
}
