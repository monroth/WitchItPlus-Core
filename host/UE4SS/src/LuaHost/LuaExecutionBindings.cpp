#include <LuaHost/LuaExecutionBindings.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <memory>
#include <lua.hpp>
#include <lstate.h>
#include "LuaNativeFrame.h"

namespace RC::LuaHost {
namespace {
struct FExecution {
    VmExecutionLease Lease;
    lua_State* Actual;
    bool Locked{};
    FExecution(VmExecutionLease lease, lua_State* actual)
        : Lease(std::move(lease)), Actual(actual) {}
    ~FExecution() {
        if (Locked) LuaUnlock(nullptr); // bundled gate never dereferences its L argument
        // Member Lease is released after the real global gate, outside locks.
    }
};
std::uint32_t enter_execution(lua_State* actual, lua_State* root, void** output) noexcept {
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = nullptr;
    if (!actual || !root) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        // This lookup reads only SDK cache metadata. No G(actual), Lua registry,
        // stack, or actual VM memory is touched before the VM lease and gate.
        if (LuaMadeSimple::cached_root_lua_state(actual) != root)
            return UE4SS_LUA_HOST_WRONG_OWNER;
        auto& host = HostBindings::instance();
        const auto vm = host.find_vm(root);
        if (vm.status != Status::ok) return static_cast<std::uint32_t>(vm.status);
        auto lease = host.lifecycle().enter_vm_execution(vm.value);
        if (lease.status != Status::ok) return static_cast<std::uint32_t>(lease.status);
        auto execution = std::make_unique<FExecution>(std::move(lease.value), actual);
        // Exact bundled luauser.c ignores L while acquiring its single recursive
        // host gate. No bindings/control mutex crosses this acquisition.
        LuaLock(actual);
        execution->Locked = true;
        const auto status = host.lifecycle().validate_vm_execution(execution->Lease);
        if (status != Status::ok) return static_cast<std::uint32_t>(status);
        *output = execution.release();
        return UE4SS_LUA_HOST_OK;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
bool validate_actual(lua_State* actual,lua_State* root) noexcept {
    // Caller already checked the immutable SDK epoch and owns a genuine scope
    // with the actual global gate. No field is read before that admission.
    if(!actual || !root || G(actual)!=G(root)) return false;
    const auto protected_actual=uikit_lua_native_current_actual();
    if(actual==protected_actual) return true;
    if(protected_actual) return actual->errorJmp==nullptr;
    return actual==root || actual->errorJmp==nullptr;
}
void leave_execution(void* opaque) noexcept { delete static_cast<FExecution*>(opaque); }
std::uint32_t enter_finalizer(lua_State* actual, lua_State* root, void** output) noexcept {
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = nullptr;
    if (!actual || actual != root) return UE4SS_LUA_HOST_WRONG_OWNER;
    try {
        if (LuaMadeSimple::cached_root_lua_state(actual) != root)
            return UE4SS_LUA_HOST_WRONG_OWNER;
        auto& host = HostBindings::instance();
        const auto vm = host.find_vm(root);
        if (vm.status != Status::ok) return static_cast<std::uint32_t>(vm.status);
        auto status = host.lifecycle().validate_vm_finalizer(vm.value);
        if (status != Status::ok) return static_cast<std::uint32_t>(status);
        auto execution = std::make_unique<FExecution>(VmExecutionLease{}, actual);
        LuaLock(actual);
        execution->Locked = true;
        status = host.lifecycle().validate_vm_finalizer(vm.value);
        if (status != Status::ok) return static_cast<std::uint32_t>(status);
        *output = execution.release();
        return UE4SS_LUA_HOST_OK;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
}
bool install_lua_execution_lifetime() noexcept {
    return LuaMadeSimple::install_host_execution_hooks({enter_execution, enter_finalizer, leave_execution,validate_actual});
}
}
