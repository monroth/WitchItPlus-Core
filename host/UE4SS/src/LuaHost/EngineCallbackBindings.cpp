#include <LuaHost/EngineCallbackBindings.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaHost/HostIntegration.hpp>
#include <Unreal/Hooks/Internal/CallbackOwnerBridge.hpp>
#include <memory>
#include <mutex>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
namespace RC::LuaHost {
namespace {
using Token = Unreal::Hook::Internal::FCallbackOwnerToken;
using Bridge = Unreal::Hook::Internal::FCallbackOwnerBridge;
thread_local const Token* destroying_owner{};
struct DeferredVm {
    std::mutex Mutex;
    VmHandle Vm;
};
ModuleHandle module(const Token& owner) { return {owner.ModuleId, owner.Generation}; }
bool host_admitted(const Token& owner) {
    if (!owner.VmId) return true;
    auto state = HostBindings::instance().lifecycle().vm_state({owner.VmId, owner.VmGeneration});
    return state.status == Status::ok && state.value == VmState::open;
}
bool host_dispatch(const Token& owner, void* payload, void (*body)(void*)) {
    if (!body) return false;
    auto& host = HostBindings::instance();
    if (!owner.VmId) { return host.api().run_host_callback(nullptr, payload, body) == Status::ok; }
    return host.dispatch_host_callback_lua({owner.VmId, owner.VmGeneration}, payload, body) == Status::ok;
}
void host_destroy(const Token&, void* payload, void (*body)(void*)) noexcept {
    try { HostBindings::instance().api().run_host_callback(nullptr, payload, body); } catch (...) {}
}
bool admitted(const Token& owner) {
    auto state = HostBindings::instance().lifecycle().module_state(module(owner));
    if (state.status != Status::ok || state.value != ModuleState::live) return false;
    if (!owner.VmId) return true;
    const auto vm = HostBindings::instance().lifecycle().vm_state({owner.VmId, owner.VmGeneration});
    return vm.status == Status::ok && vm.value == VmState::open;
}
bool dispatch(const Token& owner, void* payload, void (*body)(void*)) {
    if (!body) return false;
    auto& host = HostBindings::instance();
    VmHandle vm{owner.VmId, owner.VmGeneration};
    if (!vm.id) {
        // Code-only registration may precede on_lua_start. Once a unique
        // actual VM is proven, remember it: removing callback metadata on close
        // must never turn this old Lua-associated body back into code-only work.
        auto deferred = std::static_pointer_cast<DeferredVm>(owner.DeferredVm);
        if (deferred) { std::lock_guard lock{deferred->Mutex}; vm = deferred->Vm; }
        if (!vm.id) {
            const auto associated = host.api().registered_callback_vm(module(owner));
            if (associated.status == Status::ok) {
                if (!deferred) return false;
                std::lock_guard lock{deferred->Mutex};
                if (!deferred->Vm.id) deferred->Vm = associated.value;
                vm = deferred->Vm;
            } else if (associated.status == Status::retired || associated.status == Status::stale_generation) return false;
        }
    }
    if (vm.id && vm.generation) {
        return host.dispatch_callback_lua(vm, module(owner),
            [&](auto) { body(payload); return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK}; }) == UE4SS_LUA_HOST_OK;
    }
    return host.api().dispatch_code(module(owner), [&] { body(payload); }) == Status::ok;
}
void destroy(const Token& owner, void* payload, void (*body)(void*)) noexcept {
    struct Restore {
        const Token* previous{destroying_owner};
        explicit Restore(const Token& current) { destroying_owner = &current; }
        ~Restore() { destroying_owner = previous; }
    } restore{owner};
    HostBindings::instance().api().run_code_manager_destruction(module(owner), payload, body);
}
Token capture(const void* origin) {
    Token token{};
    const auto image = code_image(origin);
    if (!image) return token;
    if (image == host_code_image()) {
        token.Kind = Token::EKind::Host;
        token.Admitted = host_admitted; token.Dispatch = host_dispatch; token.Destroy = host_destroy;
        // Root comes from the real trusted execution scope, never callback
        // payload layout or a consumer-selected owner/state. No synthetic module.
        if (auto* root = LuaMadeSimple::LuaExecutionScope::ActiveRoot()) {
            const auto vm = HostBindings::instance().find_vm(root);
            if (vm.status != Status::ok) { token.Kind = Token::EKind::Rejected; return token; }
            token.VmId = vm.value.id; token.VmGeneration = vm.value.generation;
        }
        return token;
    }
    auto& host = HostBindings::instance();
    const auto resolved = host.module_for_image(image);
    if (resolved.status != Status::ok) return token;
    token.ModuleId = resolved.value.id; token.Generation = resolved.value.generation;
    token.Admitted = admitted; token.Dispatch = dispatch; token.Destroy = destroy;
    // Pin before owner refusal, so a known-image denied registration can
    // synchronously dispose its own function manager without code unloading.
    auto pin = host.lifecycle().enter_callback(resolved.value);
    if (pin.status != Status::ok) {
        // Manager-destructor reentry inherits the ORIGINAL existing code pin,
        // never an unrelated outer module and never new retired admission.
        if (destroying_owner && module(*destroying_owner) == resolved.value)
            token.Pin = destroying_owner->Pin;
        return token; // unresolved protection is retained by the SDK policy
    }
    token.Pin = std::make_shared<CallbackLease>(std::move(pin.value));
    const auto owner = host.api().code_owner();
    if (owner.status == Status::ok && owner.value == resolved.value) {
        token.Kind = Token::EKind::Module;
        ue4ss_lua_host_context_info_v1 context{1, sizeof(context), {}, {}, {}};
        if (HostApiRuntime::table().current_context(&context) == UE4SS_LUA_HOST_OK &&
            context.module.id == resolved.value.id && context.module.generation == resolved.value.generation) {
            token.VmId = context.vm.id; token.VmGeneration = context.vm.generation;
        }
        if (!token.VmId) token.DeferredVm = std::make_shared<DeferredVm>();
    }
    return token;
}
void revoke(ModuleHandle owner) { Bridge::RevokeModule(owner.id, owner.generation); }
void revoke_vm(VmHandle owner) { Bridge::RevokeVm(owner.id, owner.generation); }
void dispose_event(ModuleHandle module,const std::shared_ptr<void>& pin,void* payload,void (*body)(void*)) noexcept {
    Token token{};token.Kind=Token::EKind::Module;
    token.ModuleId=module.id;token.Generation=module.generation;token.Pin=pin;
    destroy(token,payload,body); // share the actual original lease for descendant disposal
}
}
bool install_engine_callback_bindings() noexcept {
    auto& host=HostBindings::instance();
    return Bridge::InstallCapture(capture) && host.install_module_revoker(revoke) && host.install_vm_revoker(revoke_vm) &&
        host.api().install_manager_disposer(dispose_event);
}
}
