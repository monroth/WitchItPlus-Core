#pragma once

#include <LuaHost/Lifecycle.hpp>
#include <LuaHost/LuaValueBackend.hpp>
#include <LuaHost/LuaTokenBackend.hpp>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace RC::LuaMadeSimple { class Lua; }

namespace RC::LuaHost {

// Produced only by trusted host LuaMadeSimple/registered callback dispatch.
// Actual and root identities are host bookkeeping, never consumer ABI inputs.
class LuaReferences;
struct LuaStackBinding {
    lua_State* root{};
    lua_State* actual{};
    int (*trampoline)(lua_State*){};
    std::uint32_t first_consumer_upvalue{1};
    std::uint32_t consumer_upvalue_count{};
    LuaReferences* references{};
};

// Trusted host dispatch only. Not exported/advertised until real LuaMod/CppMod
// dispatch and teardown own the VM/module bindings. No lua_State is fabricated.
class HostApiRuntime {
public:
    struct ObjectOperations {
        void* backend{};
        ue4ss_lua_host_status (*read)(void*,const LuaValueContext&,ue4ss_lua_context_handle,std::int32_t,void**) noexcept{};
        ue4ss_lua_host_status (*push)(void*,const LuaValueContext&,ue4ss_lua_context_handle,void*) noexcept{};
    };
private:
    struct Entry;
    struct CodeOwner;
    struct Mask;
    struct OwnedEvent;
    struct EventRecord { ModuleHandle module; VmHandle vm; std::weak_ptr<OwnedEvent> slot; };
    struct Callback {
        ue4ss_lua_callback_handle handle;
        VmHandle vm;
        ModuleHandle module;
        ue4ss_lua_host_callback_v1 function;
    };
    struct NativeFunction {
        std::uint64_t ticket;
        VmHandle vm;
        ModuleHandle module;
        int (*function)(const RC::LuaMadeSimple::Lua&);
    };
    struct Closure {
        std::uint64_t emission;
        ue4ss_lua_callback_handle callback;
        VmHandle vm;
        ModuleHandle module;
        LuaStackBinding stack;
        void* userdata{};
        const void* identity{};
        std::uint32_t upvalues{};
    };
    std::uint64_t runtime_id_{};
    mutable std::mutex closures_mutex_;
    std::unordered_map<std::uint64_t, Closure> closures_;
    std::uint64_t next_closure_{1};
    static thread_local Entry* current_;
    static thread_local CodeOwner* owner_;
    Lifecycle& lifecycle_;
    LuaValueBackend values_{lifecycle_};
    LuaTokenBackend tokens_{lifecycle_, values_};
    ObjectOperations object_operations_{};
    bool scalar_ready_{};
    Status (*callback_origin_)(ModuleHandle, const void*) noexcept{};
    Status (*stack_origin_)(VmHandle,const LuaStackBinding&) noexcept{};
    ue4ss_lua_host_status (*callback_dispatch_)(VmHandle, ModuleHandle,
        const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>&){};
    void (*manager_disposer_)(ModuleHandle,const std::shared_ptr<void>&,void*,void (*)(void*)) noexcept{};
    mutable std::mutex callbacks_mutex_;
    std::unordered_map<std::uint64_t, Callback> callbacks_;
    std::uint64_t next_callback_{1};
    mutable std::mutex functions_mutex_;
    std::unordered_map<std::uint64_t, NativeFunction> functions_;
    std::uint64_t next_function_{1};
    mutable std::mutex events_mutex_;
    std::unordered_map<std::uint64_t, EventRecord> events_;
    std::uint64_t next_event_{1};

    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL current_context(
        ue4ss_lua_host_context_info_v1*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL register_callback(
        ue4ss_lua_host_callback_v1, ue4ss_lua_callback_handle*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL invoke(
        ue4ss_lua_vm_handle, ue4ss_lua_callback_handle) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL request_vm_close(
        ue4ss_lua_vm_handle) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL request_module_retire() noexcept;

    static Entry* resolve_value_context(ue4ss_lua_context_handle, ue4ss_lua_host_status&) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL stack_top(ue4ss_lua_context_handle, std::int32_t*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL stack_set_top(ue4ss_lua_context_handle, std::int32_t) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL value_read(ue4ss_lua_context_handle, std::int32_t, ue4ss_lua_value_v1*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL upvalue_read(ue4ss_lua_context_handle, std::uint32_t, ue4ss_lua_value_v1*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL value_push(ue4ss_lua_context_handle, const ue4ss_lua_value_v1*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL table_set_field(ue4ss_lua_context_handle, std::int32_t, const char*, std::uint64_t) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL object_read(ue4ss_lua_context_handle, std::int32_t, void**) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL object_push(ue4ss_lua_context_handle, void*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL callback_push(ue4ss_lua_context_handle, ue4ss_lua_callback_handle, std::uint32_t) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL callback_return(ue4ss_lua_context_handle, std::uint32_t) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL reference_create(ue4ss_lua_context_handle, std::int32_t, ue4ss_lua_reference_handle*) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL reference_push(ue4ss_lua_context_handle, ue4ss_lua_reference_handle) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL reference_release(ue4ss_lua_context_handle, ue4ss_lua_reference_handle) noexcept;
    static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL protected_call(ue4ss_lua_context_handle, std::int32_t, std::int32_t) noexcept;

public:
    explicit HostApiRuntime(Lifecycle&);
    HostApiRuntime(const HostApiRuntime&) = delete;
    HostApiRuntime& operator=(const HostApiRuntime&) = delete;
    ~HostApiRuntime();
    // Real implementations of the initial five functions; immutable host table.
    // These functions require a live TLS entry established by dispatch().
    static const ue4ss_uikit_lua_host_api_v1& table() noexcept;
    bool finish_scalar_bootstrap() noexcept;
    std::uint64_t ready_capabilities() const noexcept;
    ue4ss_lua_host_status dispatch(VmHandle, ModuleHandle,
                                 ue4ss_lua_host_callback_v1) noexcept;
    // Host vtable dispatch, preserving the same leased TLS entry as invoke().
    ue4ss_lua_host_status dispatch_host(VmHandle, ModuleHandle,
        const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>&);
    // Scope/code pins precede actual Lua inspection; the existing embedded gate
    // remains held across consumer entry and every borrowed value lifetime.
    ue4ss_lua_host_status dispatch_lua(VmHandle, ModuleHandle, const LuaStackBinding&,
        const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>&);
    // Non-Lua entry pins code and masks any outer module's Lua authority.
    Status dispatch_code(ModuleHandle, const std::function<void()>&);
    // Exclusive trusted finalizer entry; ownership propagates even though
    // normal callback admission is already retired. Lua authority is masked.
    Status dispatch_finalizer(ModuleHandle, const std::function<void()>&);
    // Queued consumer payloads retain a code pin until their function object
    // is destroyed; retired work is dropped before consumer code is entered.
    Result<std::function<void()>> wrap_event(std::function<void()>);
    // Detach idle queued payloads after admission closes and before exclusive
    // retire. Active snapshots retain the original code pin until they drain.
    Status revoke_queued_events(ModuleHandle);
    Status revoke_queued_vm_events(VmHandle);
    // Trusted registrar seams. Owners derive from the host code-entry TLS;
    // no consumer-supplied owner or function-pointer dispatch permission.
    Result<ModuleHandle> code_owner() const;
    // Pure host registration metadata. Multiple associated VMs refuse Lua
    // authority; an earlier code-only engine registration cannot guess a root.
    Result<VmHandle> registered_callback_vm(ModuleHandle) const;
    // Trusted Host body: clear inherited Native context without inventing one.
    // Owner/mask survive protected Lua return; ordinary C++ exceptions propagate.
    Status run_host_callback(lua_State* actual_or_null, void*, void (*)(void*));
    void run_code_manager_destruction(ModuleHandle, void*, void (*)(void*)) noexcept;
    Result<std::uint64_t> register_lms_function(VmHandle,
        int (*)(const RC::LuaMadeSimple::Lua&));
    Result<int> dispatch_lms_function(std::uint64_t, const RC::LuaMadeSimple::Lua&,
        const LuaStackBinding&) noexcept;
    LuaValueBackend& value_backend() noexcept { return values_; }
    bool install_object_operations(const ObjectOperations&) noexcept;
    // Bootstrap-only actual loaded-image validation, before retaining a Native
    // callback address. The trusted host supplies the policy, never consumers.
    bool install_callback_origin(Status (*)(ModuleHandle, const void*) noexcept) noexcept;
    bool install_stack_origin(Status (*)(VmHandle,const LuaStackBinding&) noexcept) noexcept;
    bool install_callback_dispatch(ue4ss_lua_host_status (*)(VmHandle, ModuleHandle,
        const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>&)) noexcept;
    bool install_manager_disposer(void (*)(ModuleHandle,const std::shared_ptr<void>&,void*,void (*)(void*)) noexcept) noexcept;
    static lua_State* active_root_for_stack(lua_State*) noexcept;
    static HostApiRuntime* callback_runtime(std::uint64_t) noexcept;
    Result<int> dispatch_lua_closure(lua_State*, void*, std::uint64_t) noexcept;
    void collect_lua_closure(void*, std::uint64_t) noexcept;
    Status cleanup_module(ModuleHandle);
    Status cleanup_vm(VmHandle);
    std::size_t callback_count() const;
};
}
