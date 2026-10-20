#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaHost/LmsAuxiliaryBindings.hpp>
#include <LuaHost/OwnedUserdataBindings.hpp>
#include <vector>
#include <optional>
#include <LuaMadeSimple/HostThreadAccess.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include "LuaThreadFrame.h"
#include "LuaEntryFrame.h"
extern "C" void LuaLock(lua_State*);
extern "C" void LuaUnlock(lua_State*);

namespace RC::LuaHost {
HostBindings& HostBindings::instance() {
    static HostBindings host;
    return host;
}
std::shared_ptr<HostBindings::VmBinding> HostBindings::binding(VmHandle vm) const {
    std::lock_guard lock{bindings_mutex_};
    for (const auto& [root, record] : roots_) {
        (void)root;
        if (record->vm == vm) return record;
    }
    return {};
}
bool HostBindings::install_vm_observer(const VmObserver& observer) noexcept {
    try {
        std::lock_guard lock{bindings_mutex_};
        if (!roots_.empty() || vm_observer_.attach || !observer.attach || !observer.detach || !observer.forget) return false;
        vm_observer_=observer;return true;
    } catch (...) { return false; }
}
Result<VmHandle> HostBindings::attach_vm(lua_State* root) {
    if (!root) return {Status::invalid_handle, {}};
    std::lock_guard lock{bindings_mutex_};
    if (roots_.contains(root)) return {Status::busy, {}};
    auto vm = lifecycle_.create_vm();
    if (vm.status != Status::ok) return vm;
    try { roots_.emplace(root, std::make_shared<VmBinding>(lifecycle_, vm.value, root)); }
    catch (...) {
        lifecycle_.request_vm_close(vm.value);
        lifecycle_.claim_vm_finalization(vm.value);
        lifecycle_.finish_vm_finalization(vm.value);
        return {Status::internal_error, {}};
    }
    auto status = lifecycle_.publish_vm(vm.value);
    if (status==Status::ok && vm_observer_.attach) {
        status=static_cast<Status>(vm_observer_.attach(vm.value,root)); // pure host bookkeeping, no Lua
        roots_.at(root)->observer_bound=status==Status::ok;
    }
    return {status, vm.value};
}
Result<ModuleHandle> HostBindings::attach_module(void* image) {
    auto module = lifecycle_.create_module();
    if (module.status != Status::ok) return module;
    try {
        std::lock_guard lock{bindings_mutex_};
        module_images_.emplace(module.value.id, ModuleImage{module.value, image});
    } catch (...) {
        lifecycle_.request_module_retire(module.value);
        lifecycle_.claim_module_finalization(module.value);
        lifecycle_.finish_module_finalization(module.value);
        return {Status::internal_error, {}};
    }
    module.status = lifecycle_.publish_module(module.value);
    return module;
}
Result<ModuleHandle> HostBindings::module_for_image(void* image) const {
    if (!image) return {Status::invalid_handle, {}};
    std::lock_guard lock{bindings_mutex_};
    for (const auto& [id, record] : module_images_) {
        (void)id;
        if (record.image == image) return {Status::ok, record.module};
    }
    return {Status::invalid_handle, {}};
}
Result<LuaStackBinding> HostBindings::stack_binding(lua_State* root, lua_State* actual,
    int (*trampoline)(lua_State*), std::uint32_t first, std::uint32_t count) const {
    if (!root || !actual) return {Status::invalid_handle, {}};
    std::lock_guard lock{bindings_mutex_};
    const auto found = roots_.find(root);
    if (found == roots_.end()) return {Status::invalid_handle, {}};
    return {Status::ok, {root, actual, trampoline, first, count, &found->second->references}};
}
ue4ss_lua_host_status HostBindings::dispatch_callback_lua(VmHandle vm, ModuleHandle module,
    const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>& function) {
    if (!function) return UE4SS_LUA_HOST_INVALID_HANDLE;
    auto record = binding(vm); // cached metadata, no state inspection
    if (!record) return UE4SS_LUA_HOST_INVALID_HANDLE;
    auto callback = lifecycle_.enter_callback(module);
    if (callback.status != Status::ok) return static_cast<ue4ss_lua_host_status>(callback.status);
    auto scope = lifecycle_.enter_scope(vm, callback.value);
    if (scope.status != Status::ok) return static_cast<ue4ss_lua_host_status>(scope.status);
    LuaValueSerialization serialization{lifecycle_, scope.value, record->root};
    if (serialization.status() != UE4SS_LUA_HOST_OK) return serialization.status();
    if (uikit_lua_entry_reserve(record->root) != LUA_OK) return UE4SS_LUA_HOST_INTERNAL_ERROR;
    auto admission = lifecycle_.validate_scope(scope.value);
    if (admission != Status::ok) return static_cast<ue4ss_lua_host_status>(admission);
    std::shared_ptr<ThreadBinding> thread;
    {
        std::lock_guard lock{bindings_mutex_};
        for (const auto& candidate : record->threads) {
            if (!candidate->busy && !candidate->failed) { thread = candidate; break; }
        }
        if (!thread) {
            thread = std::make_shared<ThreadBinding>();
            record->threads.push_back(thread); // allocate metadata before Lua
        }
        thread->busy = true;
    }
    const auto release = [&](bool failed) {
        std::lock_guard lock{bindings_mutex_};
        thread->failed = thread->failed || failed;
        thread->busy = false;
    };
    if (!thread->actual) {
        uikit_lua_thread_request create{UIKIT_THREAD_CREATE, nullptr, -2, LUA_OK};
        if (uikit_lua_thread_run(record->root, &create) != LUA_OK) {
            release(false); return UE4SS_LUA_HOST_INTERNAL_ERROR;
        }
        thread->actual = create.thread;
        thread->reference = create.reference;
        if (!uikit_lua_entry_matches_root(thread->actual, record->root) ||
            !LuaMadeSimple::adopt_host_thread(thread->actual, record->root, &thread->adoption_epoch)) {
            release(true); return UE4SS_LUA_HOST_INTERNAL_ERROR;
        }
    }
    // Ordinary LMS processing inherits this outer VM admission through the
    // complete Native body and protected callback-stack reset.
    std::optional<LuaMadeSimple::LuaExecutionScope> execution;
    try { execution.emplace(thread->actual, record->root); }
    catch (...) { release(false); throw; }
    const auto reset = [&] {
        uikit_lua_thread_request request{UIKIT_THREAD_RESET, thread->actual, thread->reference, LUA_OK};
        const bool failed = uikit_lua_thread_run(record->root, &request) != LUA_OK || request.reset_status != LUA_OK;
        release(failed);
        return !failed;
    };
    const LuaStackBinding stack{record->root, thread->actual, nullptr, 1, 0, &record->references};
    ue4ss_lua_host_status status;
    try { status = api_.dispatch_lua(vm, module, stack, function); }
    catch (...) { reset(); throw; } // explicit Lua cleanup before C++ unwinding
    return reset() ? status : ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
}
Status HostBindings::dispatch_host_callback_lua(VmHandle vm, void* payload, void (*body)(void*)) {
    if (!body) return Status::invalid_handle;
    auto record = binding(vm); // cached host metadata before touching actual Lua
    if (!record) return Status::invalid_handle;
    auto lease = lifecycle_.enter_vm_execution(vm);
    if (lease.status != Status::ok) return lease.status;
    LuaMadeSimple::LuaExecutionScope execution{record->root, record->root};
    auto status = lifecycle_.validate_vm_execution(lease.value);
    if (status != Status::ok) return status;
    // Runtime keeps its empty owner/mask outside the protected C return.
    return api_.run_host_callback(record->root, payload, body);
}
Status HostBindings::dispatch_host_callback_lua_on_stack(VmHandle vm,lua_State* actual,void* payload,void (*body)(void*)) {
    if(!body || !actual) return Status::invalid_handle;
    auto record=binding(vm);
    if(!record || LuaMadeSimple::cached_root_lua_state(actual)!=record->root) return Status::wrong_owner;
    auto lease=lifecycle_.enter_vm_execution(vm);
    if(lease.status!=Status::ok) return lease.status;
    LuaMadeSimple::LuaExecutionScope execution{actual,record->root};
    auto status=lifecycle_.validate_vm_execution(lease.value);
    if(status!=Status::ok) return status;
    if(!uikit_lua_entry_matches_root(actual,record->root)) return Status::wrong_owner;
    return api_.run_host_callback(actual,payload,body);
}
Result<VmHandle> HostBindings::find_vm(lua_State* root) const {
    std::lock_guard lock{bindings_mutex_};
    auto it = roots_.find(root);
    if (it == roots_.end()) return {Status::invalid_handle, {}};
    return {Status::ok, it->second->vm};
}
bool HostBindings::vm_is_closing(VmHandle vm) const {
    auto result = lifecycle_.vm_state(vm);
    return result.status == Status::ok && result.value == VmState::closing;
}
bool HostBindings::module_is_retiring(ModuleHandle module) const {
    auto result = lifecycle_.module_state(module);
    return result.status == Status::ok && result.value == ModuleState::retiring;
}
Status HostBindings::claim_vm_close(VmHandle vm) {
    auto owned = lifecycle_.validate_vm_finalizer(vm);
    if (owned == Status::ok) return Status::ready;
    if (owned == Status::wrong_thread) return Status::busy;
    auto status = lifecycle_.request_vm_close(vm);
    if (status != Status::ready && status != Status::pending) return status;
    // Admission closes first. VM-associated idle closures are detached outside
    // lifecycle/control mutexes; active snapshots retain their own VM lease.
    try { if (auto revoke = vm_revoker_.load()) revoke(vm); }
    catch (...) { return Status::internal_error; }
    return lifecycle_.claim_vm_finalization(vm);
}
bool HostBindings::install_module_revoker(void (*revoker)(ModuleHandle)) noexcept {
    if (!revoker) return false;
    void (*expected)(ModuleHandle){};
    return module_revoker_.compare_exchange_strong(expected, revoker) || expected == revoker;
}
bool HostBindings::install_vm_revoker(void (*revoker)(VmHandle)) noexcept {
    if (!revoker) return false;
    void (*expected)(VmHandle){};
    return vm_revoker_.compare_exchange_strong(expected, revoker) || expected == revoker;
}
Status HostBindings::claim_module_retire(ModuleHandle module) {
    auto owned = lifecycle_.validate_module_finalizer(module);
    if (owned == Status::ok) return Status::ready;
    if (owned == Status::wrong_thread) return Status::busy;
    auto status = lifecycle_.request_module_retire(module);
    if (status != Status::ready && status != Status::pending) return status;
    // Close admission before detaching permanent registration pins. Managers
    // may reenter; revocation and disposal hold no shared host control mutex.
    try {
        status=api_.revoke_queued_events(module);
        if(status!=Status::ok) return status;
        if (auto revoke = module_revoker_.load()) revoke(module);
    }
    catch (...) { return Status::internal_error; }
    return lifecycle_.claim_module_finalization(module);
}
Status HostBindings::cleanup_vm(VmHandle vm) {
    auto record = binding(vm);
    if (!record) return Status::invalid_handle;
    auto status = lifecycle_.validate_vm_finalizer(vm);
    if (status != Status::ok) return status;
    status=api_.revoke_queued_vm_events(vm);
    if(status!=Status::ok) return status;
    {
        struct Gate { lua_State* root; explicit Gate(lua_State* r):root(r){LuaLock(root);} ~Gate(){LuaUnlock(root);} } gate{record->root};
        for (const auto& thread : record->threads) {
            if (thread->busy) return Status::pending;
            if (thread->reference != -2) {
                uikit_lua_thread_request release{UIKIT_THREAD_RELEASE, nullptr, thread->reference, LUA_OK};
                if (uikit_lua_thread_run(record->root, &release) != LUA_OK) return Status::internal_error;
                thread->reference = -2;
            }
        }
    }
    if (record->observer_bound && vm_observer_.detach) {
        status=static_cast<Status>(vm_observer_.detach(vm,record->root));
        if (status!=Status::ok) return status;
    }
    status = cleanup_owned_userdata_vm(vm,record->root);
    if(status != Status::ok) return status;
    status = record->references.cleanup_vm();
    if (status != Status::ok) return status;
    revoke_lms_auxiliary_vm(vm);
    return api_.cleanup_vm(vm);
}
Status HostBindings::cleanup_module(ModuleHandle module) {
    auto status = lifecycle_.validate_module_finalizer(module);
    if (status != Status::ok) return status;
    std::vector<std::shared_ptr<VmBinding>> records;
    try {
        std::lock_guard lock{bindings_mutex_};
        records.reserve(roots_.size());
        for (const auto& [root, record] : roots_) { (void)root; records.push_back(record); }
    } catch (...) { return Status::internal_error; }
    // No registry/control lock is held while LuaReferences acquires LuaRaw's
    // existing gate. The cleanup lease pins the real VM until unref completes.
    for (const auto& record : records) {
        auto scope = lifecycle_.enter_module_cleanup(record->vm, module);
        if (scope.status == Status::retired || scope.status == Status::stale_generation) continue;
        if (scope.status != Status::ok) return scope.status;
        status = record->references.cleanup_module(scope.value);
        if (status != Status::ok) return status;
    }
    revoke_lms_auxiliary_module(module);
    return api_.cleanup_module(module);
}
Status HostBindings::finish_vm_close(VmHandle vm) {
    auto record = binding(vm);
    if (!record) return Status::invalid_handle;
    if (record->references.size()) return Status::pending;
    auto owned = forget_owned_userdata_vm(vm);
    if(owned != Status::ok) return owned;
    auto status = lifecycle_.finish_vm_finalization(vm);
    if (status != Status::ok) return status;
    if (record->observer_bound && vm_observer_.forget) {
        status=static_cast<Status>(vm_observer_.forget(vm,record->root));
        if (status!=Status::ok) return status;
    }
    for (const auto& thread : record->threads)
        if (thread->actual) LuaMadeSimple::forget_closed_host_thread(thread->actual, thread->adoption_epoch);
    LuaMadeSimple::forget_closed_host_vm(record->root);
    std::lock_guard lock{bindings_mutex_};
    roots_.erase(record->root);
    return Status::ok;
}
Status HostBindings::finish_module_retire(ModuleHandle module) {
    auto status = lifecycle_.finish_module_finalization(module);
    if (status != Status::ok) return status;
    std::lock_guard lock{bindings_mutex_};
    module_images_.erase(module.id);
    return Status::ok;
}
std::size_t HostBindings::vm_count() const {
    std::lock_guard lock{bindings_mutex_};
    return roots_.size();
}
}
