#pragma once

#include <LuaHost/Lifecycle.hpp>
#include <atomic>
#include <unordered_map>

struct lua_State;

namespace RC::LuaHost {

// Internal host component. The root is supplied by the host's verified VM
// binding, never by a consumer. Production uses the existing embedded LuaRaw
// gate and runtime. No raw state or registry index escapes through these handles.
class LuaReferences {
    struct Record { ReferenceHandle handle; int index; ModuleHandle module; bool committed{}, busy{}; };
    Lifecycle& lifecycle_;
    VmHandle vm_;
    lua_State* root_;
    std::unordered_map<std::uint64_t, Record> records_;
    std::atomic<std::size_t> size_{};
    bool cleaning_{};
public:
    LuaReferences(Lifecycle&, VmHandle, lua_State* verified_host_root);
    LuaReferences(const LuaReferences&) = delete;
    LuaReferences& operator=(const LuaReferences&) = delete;
    // Destruction never touches Lua. Host teardown must first call cleanup_vm
    // while it owns the finalization claim and before lua_close.
    ~LuaReferences();
    Result<ReferenceHandle> retain(const ScopeLease&, int stack_index);
    Status push(const ScopeLease&, ReferenceHandle);
    Status release(const ScopeLease&, ReferenceHandle);
    // Host resolver supplies the verified ACTUAL callback/coroutine stack.
    // The indexed value/result lives on that stack, sharing this VM's registry.
    // Ordinary tail routes must use these overloads. No consumer state is accepted.
    // Existing root methods above remain source-compatible for trusted old callers.
    Result<ReferenceHandle> retain(const ScopeLease&, int stack_index, lua_State* verified_actual_stack);
    Status push(const ScopeLease&, ReferenceHandle, lua_State* verified_actual_stack);
    Status release(const ScopeLease&, ReferenceHandle, lua_State* verified_actual_stack);
    Status cleanup_vm();
    Status cleanup_module(const ScopeLease& trusted_cleanup_scope);
    std::size_t size() const noexcept { return size_.load(); }
};
}
