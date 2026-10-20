#pragma once

#include <LuaHost/UIKitLuaHost.h>
#include <cstdint>
#include <memory>
#include <thread>

namespace RC::LuaHost {

enum class Status : std::uint32_t {
    ok = UE4SS_LUA_HOST_OK, invalid_handle = UE4SS_LUA_HOST_INVALID_HANDLE,
    stale_generation = UE4SS_LUA_HOST_STALE_GENERATION, retired = UE4SS_LUA_HOST_RETIRED,
    wrong_thread = UE4SS_LUA_HOST_WRONG_THREAD, wrong_owner = UE4SS_LUA_HOST_WRONG_OWNER,
    pending = UE4SS_LUA_HOST_PENDING, ready = UE4SS_LUA_HOST_READY,
    busy = UE4SS_LUA_HOST_BUSY, internal_error = UE4SS_LUA_HOST_INTERNAL_ERROR
};
enum class VmState { opening, open, closing, closed };
enum class ModuleState { starting, live, retiring, unloaded };
enum class HostState { running, stopping, stopped };

struct VmHandle { std::uint64_t id{}, generation{}; bool operator==(const VmHandle&) const = default; };
struct ModuleHandle { std::uint64_t id{}, generation{}; bool operator==(const ModuleHandle&) const = default; };
struct ContextHandle { std::uint64_t id{}, generation{}; bool operator==(const ContextHandle&) const = default; };
struct ReferenceHandle { std::uint64_t id{}, generation{}; bool operator==(const ReferenceHandle&) const = default; };

template<class T> struct Result { Status status{Status::invalid_handle}; T value{}; };
namespace Detail { struct State; }
class Lifecycle;

/* Trusted host Lua execution pin. It grants no Native module or Lua value
 * authority; ordinary UE4SS Lua still needs its actual VM to survive C returns. */
class VmExecutionLease {
    friend class Lifecycle;
    std::shared_ptr<Detail::State> state_;
    VmHandle vm_{};
    std::thread::id thread_{};
    VmExecutionLease(std::shared_ptr<Detail::State>, VmHandle);
public:
    VmExecutionLease() = default;
    VmExecutionLease(const VmExecutionLease&) = delete;
    VmExecutionLease& operator=(const VmExecutionLease&) = delete;
    VmExecutionLease(VmExecutionLease&&) noexcept;
    VmExecutionLease& operator=(VmExecutionLease&&) noexcept;
    ~VmExecutionLease();
    void reset() noexcept;
    explicit operator bool() const noexcept { return bool(state_); }
    VmHandle vm() const noexcept { return vm_; }
};

/* Host-only callback pin. It must be gained by a stable host trampoline before
 * entering consumer code, not by consumer code after its DLL entry. */
class CallbackLease {
    friend class Lifecycle;
    std::shared_ptr<Detail::State> state_;
    ContextHandle handle_{};
    ModuleHandle module_{};
    CallbackLease(std::shared_ptr<Detail::State>, ContextHandle, ModuleHandle);
public:
    CallbackLease() = default;
    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;
    CallbackLease(CallbackLease&&) noexcept;
    CallbackLease& operator=(CallbackLease&&) noexcept;
    ~CallbackLease();
    void reset() noexcept;
    explicit operator bool() const noexcept { return bool(state_); }
    ContextHandle handle() const noexcept { return handle_; }
    ModuleHandle module() const noexcept { return module_; }
};

class ScopeLease {
    friend class Lifecycle;
    std::shared_ptr<Detail::State> state_;
    ContextHandle handle_{};
    VmHandle vm_{};
    ModuleHandle module_{};
    ScopeLease(std::shared_ptr<Detail::State>, ContextHandle, VmHandle, ModuleHandle);
public:
    ScopeLease() = default;
    ScopeLease(const ScopeLease&) = delete;
    ScopeLease& operator=(const ScopeLease&) = delete;
    ScopeLease(ScopeLease&&) noexcept;
    ScopeLease& operator=(ScopeLease&&) noexcept;
    ~ScopeLease();
    void reset() noexcept;
    explicit operator bool() const noexcept { return bool(state_); }
    ContextHandle handle() const noexcept { return handle_; }
    VmHandle vm() const noexcept { return vm_; }
    ModuleHandle module() const noexcept { return module_; }
};

/* Pure lifetime bookkeeping. No Lua state, callback, serialization gate,
 * event queue, wait, native destruction or FreeLibrary occurs in this class.
 * Only trusted host integration can call its admission/finalizer methods. */
class Lifecycle {
    std::shared_ptr<Detail::State> state_;
public:
    Lifecycle();
    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;
    Result<VmHandle> create_vm();
    Result<ModuleHandle> create_module();
    Status publish_vm(VmHandle);
    Status publish_module(ModuleHandle);
    Result<VmExecutionLease> enter_vm_execution(VmHandle);
    Status validate_vm_execution(const VmExecutionLease&) const;
    Result<CallbackLease> enter_callback(ModuleHandle);
    Result<ScopeLease> enter_scope(VmHandle, const CallbackLease&);
    Status validate_context(ContextHandle, const CallbackLease&) const;
    Status validate_scope(const ScopeLease&) const;
    Result<ReferenceHandle> create_reference(const ScopeLease&);
    Status validate_reference(ReferenceHandle, const ScopeLease&) const;
    Status release_reference(ReferenceHandle, const ScopeLease&);
    // Host-only rollback/cleanup; it does not admit new consumer work after retirement.
    Status release_reference_for_cleanup(ReferenceHandle, const ScopeLease&);
    Status validate_vm_finalizer(VmHandle) const;
    Status validate_module_finalizer(ModuleHandle) const;
    Status release_reference_at_vm_finalizer(ReferenceHandle, VmHandle);
    // Trusted finalizer-only scope: pins both the retiring module and its VM.
    // Normal consumer/reference admission rejects this scope flavor.
    Result<ScopeLease> enter_module_cleanup(VmHandle, ModuleHandle);
    Status validate_module_cleanup(const ScopeLease&) const;
    Status release_reference_at_module_cleanup(ReferenceHandle, const ScopeLease&);
    Status request_vm_close(VmHandle);
    Status request_module_retire(ModuleHandle);
    Status claim_vm_finalization(VmHandle);
    Status claim_module_finalization(ModuleHandle);
    Status finish_vm_finalization(VmHandle);
    Status finish_module_finalization(ModuleHandle);
    Status begin_shutdown();
    Status finish_shutdown();
    HostState host_state() const;
    Status validate_closed_vm(VmHandle formerly_live) const;
    Result<VmState> vm_state(VmHandle) const;
    Result<ModuleState> module_state(ModuleHandle) const;
};
}
