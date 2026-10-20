#include <LuaHost/Lifecycle.hpp>
#include <cassert>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>

namespace RC::LuaHost::Detail {
struct Vm {
    std::uint64_t generation{1}, scopes{}, executions{};
    VmState phase{VmState::opening};
    bool claimed{};
    std::thread::id finalizer{};
};
struct Module {
    std::uint64_t generation{1}, callbacks{}, scopes{};
    ModuleState phase{ModuleState::starting};
    bool claimed{};
    std::thread::id finalizer{};
};
struct Entry {
    std::uint64_t generation{1}, callback_id{};
    ModuleHandle module{};
    VmHandle vm{};
    std::thread::id thread{};
    bool is_scope{};
    bool is_cleanup{};
};
struct Reference {
    std::uint64_t generation{1};
    ModuleHandle module{};
    VmHandle vm{};
};
struct State {
    mutable std::mutex mutex;
    HostState phase{HostState::running};
    std::uint64_t next_id{1};
    std::unordered_map<std::uint64_t, Vm> vms;
    std::unordered_map<std::uint64_t, Module> modules;
    std::unordered_map<std::uint64_t, Entry> entries;
    std::unordered_map<std::uint64_t, Reference> references;

    std::uint64_t allocate_id() noexcept {
        if (next_id == std::numeric_limits<std::uint64_t>::max()) return 0;
        return next_id++;
    }
};

template<class Map, class Handle, class Pointer>
Status lookup(Map& map, Handle handle, Pointer& result) {
    if (!handle.id || !handle.generation) return Status::invalid_handle;
    auto found = map.find(handle.id);
    if (found == map.end()) return Status::invalid_handle;
    if (found->second.generation != handle.generation) return Status::stale_generation;
    result = &found->second;
    return Status::ok;
}

Status scope_status(const State& state, ContextHandle handle, const Entry*& entry) {
    auto status = lookup(state.entries, handle, entry);
    if (status != Status::ok) return status;
    if (!entry->is_scope || entry->is_cleanup) return Status::wrong_owner;
    if (entry->thread != std::this_thread::get_id()) return Status::wrong_thread;
    auto callback = state.entries.find(entry->callback_id);
    if (callback == state.entries.end() || callback->second.is_scope)
        return Status::retired;
    if (state.phase != HostState::running) return Status::retired;
    const Vm* vm{};
    const Module* module{};
    status = lookup(state.vms, entry->vm, vm);
    if (status != Status::ok) return status;
    status = lookup(state.modules, entry->module, module);
    if (status != Status::ok) return status;
    if (vm->phase != VmState::open || module->phase != ModuleState::live)
        return Status::retired;
    return Status::ok;
}

Status cleanup_status(const State& state, ContextHandle handle, const Entry*& entry) {
    auto status=lookup(state.entries,handle,entry);
    if (status!=Status::ok) return status;
    if (!entry->is_scope || !entry->is_cleanup) return Status::wrong_owner;
    if (entry->thread!=std::this_thread::get_id()) return Status::wrong_thread;
    const Vm* vm{};
    const Module* module{};
    status=lookup(state.vms,entry->vm,vm);
    if (status!=Status::ok) return status;
    status=lookup(state.modules,entry->module,module);
    if (status!=Status::ok) return status;
    if (state.phase==HostState::stopped || vm->phase==VmState::closed || vm->claimed ||
        module->phase!=ModuleState::retiring || !module->claimed ||
        module->finalizer!=std::this_thread::get_id()) return Status::retired;
    return Status::ok;
}

void release(std::shared_ptr<State> state, ContextHandle handle) noexcept {
    if (!state) return;
    std::lock_guard lock{state->mutex};
    auto found = state->entries.find(handle.id);
    if (found == state->entries.end() || found->second.generation != handle.generation)
        return;
    const auto entry = found->second;
    auto& module = state->modules.at(entry.module.id);
    if (entry.is_scope) {
        auto& vm = state->vms.at(entry.vm.id);
        assert(vm.scopes && module.scopes);
        --vm.scopes;
        --module.scopes;
    } else {
        assert(module.callbacks);
        --module.callbacks;
    }
    state->entries.erase(found);
    // Deliberately no callback, queue submission, teardown, gate or wait here.
}

bool drained(const State& state) {
    if (!state.entries.empty()) return false;
    for (const auto& [id, vm] : state.vms) {
        (void)id;
        if (vm.phase != VmState::closed) return false;
    }
    for (const auto& [id, module] : state.modules) {
        (void)id;
        if (module.phase != ModuleState::unloaded) return false;
    }
    return true;
}
}

namespace RC::LuaHost {
VmExecutionLease::VmExecutionLease(std::shared_ptr<Detail::State> state, VmHandle vm)
    : state_(std::move(state)), vm_(vm), thread_(std::this_thread::get_id()) {}
VmExecutionLease::VmExecutionLease(VmExecutionLease&& other) noexcept
    : state_(std::move(other.state_)), vm_(std::exchange(other.vm_, {})), thread_(other.thread_) {}
VmExecutionLease& VmExecutionLease::operator=(VmExecutionLease&& other) noexcept {
    if (this != &other) {
        reset(); state_ = std::move(other.state_);
        vm_ = std::exchange(other.vm_, {}); thread_ = other.thread_;
    }
    return *this;
}
VmExecutionLease::~VmExecutionLease() { reset(); }
void VmExecutionLease::reset() noexcept {
    auto state = std::move(state_);
    const auto vm_handle = std::exchange(vm_, {});
    if (!state) return;
    std::lock_guard lock{state->mutex};
    Detail::Vm* vm{};
    const auto status = Detail::lookup(state->vms, vm_handle, vm);
    assert(status == Status::ok && vm->executions);
    if (status == Status::ok && vm->executions) --vm->executions;
    // No Lua, gate acquisition, queue, wait or finalization from release.
}

CallbackLease::CallbackLease(std::shared_ptr<Detail::State> state,
                             ContextHandle handle, ModuleHandle module)
    : state_(std::move(state)), handle_(handle), module_(module) {}
CallbackLease::CallbackLease(CallbackLease&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, {})),
      module_(std::exchange(other.module_, {})) {}
CallbackLease& CallbackLease::operator=(CallbackLease&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, {});
        module_ = std::exchange(other.module_, {});
    }
    return *this;
}
CallbackLease::~CallbackLease() { reset(); }
void CallbackLease::reset() noexcept {
    auto state = std::move(state_);
    auto handle = std::exchange(handle_, {});
    module_ = {};
    Detail::release(std::move(state), handle);
}

ScopeLease::ScopeLease(std::shared_ptr<Detail::State> state, ContextHandle handle,
                      VmHandle vm, ModuleHandle module)
    : state_(std::move(state)), handle_(handle), vm_(vm), module_(module) {}
ScopeLease::ScopeLease(ScopeLease&& other) noexcept
    : state_(std::move(other.state_)), handle_(std::exchange(other.handle_, {})),
      vm_(std::exchange(other.vm_, {})), module_(std::exchange(other.module_, {})) {}
ScopeLease& ScopeLease::operator=(ScopeLease&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, {});
        vm_ = std::exchange(other.vm_, {});
        module_ = std::exchange(other.module_, {});
    }
    return *this;
}
ScopeLease::~ScopeLease() { reset(); }
void ScopeLease::reset() noexcept {
    auto state = std::move(state_);
    auto handle = std::exchange(handle_, {});
    vm_ = {};
    module_ = {};
    Detail::release(std::move(state), handle);
}

Lifecycle::Lifecycle() : state_(std::make_shared<Detail::State>()) {}
Result<VmHandle> Lifecycle::create_vm() {
    std::lock_guard lock{state_->mutex};
    if (state_->phase != HostState::running) return {Status::retired, {}};
    const auto id = state_->allocate_id();
    if (!id) return {Status::internal_error, {}};
    try { state_->vms.emplace(id, Detail::Vm{}); }
    catch (const std::bad_alloc&) { return {Status::internal_error, {}}; }
    return {Status::ok, {id, 1}};
}
Result<ModuleHandle> Lifecycle::create_module() {
    std::lock_guard lock{state_->mutex};
    if (state_->phase != HostState::running) return {Status::retired, {}};
    const auto id = state_->allocate_id();
    if (!id) return {Status::internal_error, {}};
    try { state_->modules.emplace(id, Detail::Module{}); }
    catch (const std::bad_alloc&) { return {Status::internal_error, {}}; }
    return {Status::ok, {id, 1}};
}
Status Lifecycle::publish_vm(VmHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Vm* vm{};
    auto status = Detail::lookup(state_->vms, handle, vm);
    if (status != Status::ok) return status;
    if (state_->phase != HostState::running || vm->phase != VmState::opening)
        return Status::retired;
    vm->phase = VmState::open;
    return Status::ok;
}
Status Lifecycle::publish_module(ModuleHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Module* module{};
    auto status = Detail::lookup(state_->modules, handle, module);
    if (status != Status::ok) return status;
    if (state_->phase != HostState::running || module->phase != ModuleState::starting)
        return Status::retired;
    module->phase = ModuleState::live;
    return Status::ok;
}
Result<VmExecutionLease> Lifecycle::enter_vm_execution(VmHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Vm* vm{};
    auto status = Detail::lookup(state_->vms, handle, vm);
    if (status != Status::ok) return {status, {}};
    if (state_->phase != HostState::running || vm->phase != VmState::open || vm->claimed)
        return {Status::retired, {}};
    if (vm->executions == std::numeric_limits<std::uint64_t>::max())
        return {Status::internal_error, {}};
    ++vm->executions;
    return {Status::ok, VmExecutionLease{state_, handle}};
}
Status Lifecycle::validate_vm_execution(const VmExecutionLease& lease) const {
    if (lease.state_.get() != state_.get()) return Status::wrong_owner;
    if (lease.thread_ != std::this_thread::get_id()) return Status::wrong_thread;
    std::lock_guard lock{state_->mutex};
    const Detail::Vm* vm{};
    auto status = Detail::lookup(state_->vms, lease.vm_, vm);
    if (status != Status::ok) return status;
    return state_->phase == HostState::running && vm->phase == VmState::open &&
           !vm->claimed && vm->executions ? Status::ok : Status::retired;
}

Result<CallbackLease> Lifecycle::enter_callback(ModuleHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Module* module{};
    auto status = Detail::lookup(state_->modules, handle, module);
    if (status != Status::ok) return {status, {}};
    if (state_->phase != HostState::running || module->phase != ModuleState::live)
        return {Status::retired, {}};
    const auto id = state_->allocate_id();
    if (!id) return {Status::internal_error, {}};
    try { state_->entries.emplace(id, Detail::Entry{1, 0, handle, {}, std::this_thread::get_id(), false}); }
    catch (const std::bad_alloc&) { return {Status::internal_error, {}}; }
    ++module->callbacks;
    return {Status::ok, CallbackLease{state_, {id, 1}, handle}};
}
Result<ScopeLease> Lifecycle::enter_scope(VmHandle handle, const CallbackLease& callback) {
    if (callback.state_.get() != state_.get()) return {Status::wrong_owner, {}};
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    auto status = Detail::lookup(state_->entries, callback.handle_, entry);
    if (status != Status::ok) return {status, {}};
    if (entry->is_scope) return {Status::wrong_owner, {}};
    if (entry->thread != std::this_thread::get_id()) return {Status::wrong_thread, {}};
    Detail::Module* module{};
    Detail::Vm* vm{};
    status = Detail::lookup(state_->modules, entry->module, module);
    if (status != Status::ok) return {status, {}};
    status = Detail::lookup(state_->vms, handle, vm);
    if (status != Status::ok) return {status, {}};
    if (state_->phase != HostState::running || vm->phase != VmState::open ||
        module->phase != ModuleState::live) return {Status::retired, {}};
    const auto id = state_->allocate_id();
    if (!id) return {Status::internal_error, {}};
    try { state_->entries.emplace(id, Detail::Entry{1, callback.handle_.id,
              entry->module, handle, std::this_thread::get_id(), true}); }
    catch (const std::bad_alloc&) { return {Status::internal_error, {}}; }
    ++vm->scopes;
    ++module->scopes;
    return {Status::ok, ScopeLease{state_, {id, 1}, handle, callback.module_}};
}
Status Lifecycle::validate_context(ContextHandle handle, const CallbackLease& callback) const {
    if (callback.state_.get() != state_.get()) return Status::wrong_owner;
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    auto status = Detail::scope_status(*state_, handle, entry);
    if (status != Status::ok) return status;
    if (entry->callback_id != callback.handle_.id || entry->module != callback.module_)
        return Status::wrong_owner;
    return Status::ok;
}
Result<ReferenceHandle> Lifecycle::create_reference(const ScopeLease& scope) {
    if (scope.state_.get() != state_.get()) return {Status::wrong_owner, {}};
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    auto status = Detail::scope_status(*state_, scope.handle_, entry);
    if (status != Status::ok) return {status, {}};
    const auto id = state_->allocate_id();
    if (!id) return {Status::internal_error, {}};
    try { state_->references.emplace(id, Detail::Reference{1, entry->module, entry->vm}); }
    catch (const std::bad_alloc&) { return {Status::internal_error, {}}; }
    return {Status::ok, {id, 1}};
}
Status Lifecycle::validate_scope(const ScopeLease& scope) const {
    if (scope.state_.get() != state_.get()) return Status::wrong_owner;
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    return Detail::scope_status(*state_, scope.handle_, entry);
}
Status Lifecycle::validate_reference(ReferenceHandle handle, const ScopeLease& scope) const {
    if (scope.state_.get() != state_.get()) return Status::wrong_owner;
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    auto status = Detail::scope_status(*state_, scope.handle_, entry);
    if (status != Status::ok) return status;
    const Detail::Reference* reference{};
    status = Detail::lookup(state_->references, handle, reference);
    if (status != Status::ok) return status;
    return reference->vm == entry->vm && reference->module == entry->module
        ? Status::ok : Status::wrong_owner;
}
Status Lifecycle::release_reference(ReferenceHandle handle, const ScopeLease& scope) {
    if (scope.state_.get() != state_.get()) return Status::wrong_owner;
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    auto status = Detail::scope_status(*state_, scope.handle_, entry);
    if (status != Status::ok) return status;
    const Detail::Reference* reference{};
    status = Detail::lookup(state_->references, handle, reference);
    if (status != Status::ok) return status;
    if (reference->vm != entry->vm || reference->module != entry->module)
        return Status::wrong_owner;
    state_->references.erase(handle.id);
    return Status::ok;
}
Status Lifecycle::release_reference_for_cleanup(ReferenceHandle handle, const ScopeLease& scope) {
    if (scope.state_.get() != state_.get()) return Status::wrong_owner;
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    auto status=Detail::lookup(state_->entries,scope.handle_,entry);
    if (status!=Status::ok) return status;
    if (!entry->is_scope || entry->vm!=scope.vm_ || entry->module!=scope.module_)
        return Status::wrong_owner;
    if (entry->thread!=std::this_thread::get_id()) return Status::wrong_thread;
    const Detail::Reference* reference{};
    status=Detail::lookup(state_->references,handle,reference);
    if (status!=Status::ok) return status;
    if (reference->vm!=entry->vm || reference->module!=entry->module) return Status::wrong_owner;
    // The existing scope still pins VM/module memory. This only drops a record;
    // it never calls consumer code or grants a new operation after retirement.
    state_->references.erase(handle.id);
    return Status::ok;
}
Status Lifecycle::validate_vm_finalizer(VmHandle handle) const {
    std::lock_guard lock{state_->mutex};
    const Detail::Vm* vm{};
    auto status=Detail::lookup(state_->vms,handle,vm);
    if (status!=Status::ok) return status;
    if (vm->phase!=VmState::closing || !vm->claimed) return Status::retired;
    if (vm->finalizer!=std::this_thread::get_id()) return Status::wrong_thread;
    return vm->scopes || vm->executions ? Status::pending : Status::ok;
}
Status Lifecycle::release_reference_at_vm_finalizer(ReferenceHandle handle, VmHandle owner) {
    std::lock_guard lock{state_->mutex};
    const Detail::Vm* vm{};
    auto status=Detail::lookup(state_->vms,owner,vm);
    if (status!=Status::ok) return status;
    if (vm->phase!=VmState::closing || !vm->claimed) return Status::retired;
    if (vm->finalizer!=std::this_thread::get_id()) return Status::wrong_thread;
    if (vm->scopes || vm->executions) return Status::pending;
    const Detail::Reference* reference{};
    status=Detail::lookup(state_->references,handle,reference);
    if (status!=Status::ok) return status;
    if (reference->vm!=owner) return Status::wrong_owner;
    state_->references.erase(handle.id);
    return Status::ok;
}
Result<ScopeLease> Lifecycle::enter_module_cleanup(VmHandle vm_handle, ModuleHandle module_handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Vm* vm{};
    Detail::Module* module{};
    auto status=Detail::lookup(state_->vms,vm_handle,vm);
    if (status!=Status::ok) return {status,{}};
    status=Detail::lookup(state_->modules,module_handle,module);
    if (status!=Status::ok) return {status,{}};
    if (state_->phase==HostState::stopped ||
        (vm->phase!=VmState::open && vm->phase!=VmState::closing) ||
        module->phase!=ModuleState::retiring || !module->claimed)
        return {Status::retired,{}};
    if (module->finalizer!=std::this_thread::get_id()) return {Status::wrong_thread,{}};
    if (vm->claimed || module->callbacks || module->scopes) return {Status::busy,{}};
    const auto id=state_->allocate_id();
    if (!id) return {Status::internal_error,{}};
    try { state_->entries.emplace(id,Detail::Entry{1,0,module_handle,vm_handle,
                       std::this_thread::get_id(),true,true}); }
    catch (const std::bad_alloc&) { return {Status::internal_error,{}}; }
    ++vm->scopes; ++module->scopes;
    return {Status::ok,ScopeLease{state_,{id,1},vm_handle,module_handle}};
}
Status Lifecycle::validate_module_cleanup(const ScopeLease& scope) const {
    if (scope.state_.get()!=state_.get()) return Status::wrong_owner;
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    return Detail::cleanup_status(*state_,scope.handle_,entry);
}
Status Lifecycle::release_reference_at_module_cleanup(ReferenceHandle handle, const ScopeLease& scope) {
    if (scope.state_.get()!=state_.get()) return Status::wrong_owner;
    std::lock_guard lock{state_->mutex};
    const Detail::Entry* entry{};
    auto status=Detail::cleanup_status(*state_,scope.handle_,entry);
    if (status!=Status::ok) return status;
    const Detail::Reference* reference{};
    status=Detail::lookup(state_->references,handle,reference);
    if (status!=Status::ok) return status;
    if (reference->vm!=entry->vm || reference->module!=entry->module) return Status::wrong_owner;
    state_->references.erase(handle.id);
    return Status::ok;
}
Status Lifecycle::request_vm_close(VmHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Vm* vm{};
    auto status = Detail::lookup(state_->vms, handle, vm);
    if (status != Status::ok) return status;
    if (vm->phase == VmState::closed) return Status::retired;
    vm->phase = VmState::closing;
    return vm->scopes || vm->executions || vm->claimed ? Status::pending : Status::ready;
}
Status Lifecycle::request_module_retire(ModuleHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Module* module{};
    auto status = Detail::lookup(state_->modules, handle, module);
    if (status != Status::ok) return status;
    if (module->phase == ModuleState::unloaded) return Status::retired;
    module->phase = ModuleState::retiring;
    return module->callbacks || module->scopes || module->claimed ? Status::pending : Status::ready;
}
Status Lifecycle::claim_vm_finalization(VmHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Vm* vm{};
    auto status = Detail::lookup(state_->vms, handle, vm);
    if (status != Status::ok) return status;
    if (vm->phase != VmState::closing) return Status::retired;
    if (vm->scopes || vm->executions) return Status::pending;
    if (vm->claimed) return Status::busy;
    vm->claimed = true;
    vm->finalizer = std::this_thread::get_id();
    return Status::ready;
}
Status Lifecycle::claim_module_finalization(ModuleHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Module* module{};
    auto status = Detail::lookup(state_->modules, handle, module);
    if (status != Status::ok) return status;
    if (module->phase != ModuleState::retiring) return Status::retired;
    if (module->callbacks || module->scopes) return Status::pending;
    if (module->claimed) return Status::busy;
    module->claimed = true;
    module->finalizer = std::this_thread::get_id();
    return Status::ready;
}
Status Lifecycle::finish_vm_finalization(VmHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Vm* vm{};
    auto status = Detail::lookup(state_->vms, handle, vm);
    if (status != Status::ok) return status;
    if (vm->phase != VmState::closing || !vm->claimed) return Status::retired;
    if (vm->finalizer != std::this_thread::get_id()) return Status::wrong_thread;
    assert(!vm->scopes && !vm->executions);
    vm->phase = VmState::closed;
    ++vm->generation;
    for (auto it = state_->references.begin(); it != state_->references.end();) {
        if (it->second.vm == handle) it = state_->references.erase(it); else ++it;
    }
    return Status::ok;
}
Status Lifecycle::validate_module_finalizer(ModuleHandle handle) const {
    std::lock_guard lock{state_->mutex};
    const Detail::Module* module{};
    auto status = Detail::lookup(state_->modules, handle, module);
    if (status != Status::ok) return status;
    if (module->phase != ModuleState::retiring || !module->claimed) return Status::retired;
    if (module->finalizer != std::this_thread::get_id()) return Status::wrong_thread;
    if (module->callbacks || module->scopes) return Status::pending;
    return Status::ok;
}
Status Lifecycle::finish_module_finalization(ModuleHandle handle) {
    std::lock_guard lock{state_->mutex};
    Detail::Module* module{};
    auto status = Detail::lookup(state_->modules, handle, module);
    if (status != Status::ok) return status;
    if (module->phase != ModuleState::retiring || !module->claimed) return Status::retired;
    if (module->finalizer != std::this_thread::get_id()) return Status::wrong_thread;
    if (module->callbacks || module->scopes) return Status::pending;
    module->phase = ModuleState::unloaded;
    ++module->generation;
    for (auto it = state_->references.begin(); it != state_->references.end();) {
        if (it->second.module == handle) it = state_->references.erase(it); else ++it;
    }
    return Status::ok;
}
Status Lifecycle::begin_shutdown() {
    std::lock_guard lock{state_->mutex};
    if (state_->phase == HostState::stopped) return Status::retired;
    state_->phase = HostState::stopping;
    for (auto& [id, vm] : state_->vms) {
        (void)id;
        if (vm.phase != VmState::closed) vm.phase = VmState::closing;
    }
    for (auto& [id, module] : state_->modules) {
        (void)id;
        if (module.phase != ModuleState::unloaded) module.phase = ModuleState::retiring;
    }
    return Detail::drained(*state_) ? Status::ready : Status::pending;
}
Status Lifecycle::finish_shutdown() {
    std::lock_guard lock{state_->mutex};
    if (state_->phase != HostState::stopping) return Status::retired;
    if (!Detail::drained(*state_)) return Status::pending;
    state_->phase = HostState::stopped;
    return Status::ok;
}
HostState Lifecycle::host_state() const {
    std::lock_guard lock{state_->mutex};
    return state_->phase;
}
Status Lifecycle::validate_closed_vm(VmHandle former) const {
    std::lock_guard lock{state_->mutex};
    const auto it=state_->vms.find(former.id);
    if (!former.id || !former.generation || it==state_->vms.end()) return Status::invalid_handle;
    // finish_vm_finalization invalidates ordinary handles by advancing exactly
    // one generation. This trusted close receipt accepts only that former VM.
    const auto& vm=it->second;
    if (former.generation==std::numeric_limits<std::uint64_t>::max() ||
        vm.generation!=former.generation+1) return Status::stale_generation;
    return vm.phase==VmState::closed?Status::ok:Status::busy;
}
Result<VmState> Lifecycle::vm_state(VmHandle handle) const {
    std::lock_guard lock{state_->mutex};
    const Detail::Vm* vm{};
    auto status = Detail::lookup(state_->vms, handle, vm);
    return {status, status == Status::ok ? vm->phase : VmState::closed};
}
Result<ModuleState> Lifecycle::module_state(ModuleHandle handle) const {
    std::lock_guard lock{state_->mutex};
    const Detail::Module* module{};
    auto status = Detail::lookup(state_->modules, handle, module);
    return {status, status == Status::ok ? module->phase : ModuleState::unloaded};
}

}
