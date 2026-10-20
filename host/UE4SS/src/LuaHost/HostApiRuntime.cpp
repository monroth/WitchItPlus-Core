#include <optional>
#include <LuaHost/HostBindings.hpp>
#include "LuaNativeFrame.h"
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <LuaHost/HostApiRuntime.hpp>
#include <LuaHost/LuaReferences.hpp>
#include "LuaEntryFrame.h"
#include "LuaCallbackFrame.h"
#include <cassert>
#include <limits>
#include <atomic>
#include <vector>
#include <exception>

namespace RC::LuaHost {
namespace {
ue4ss_lua_host_status code(Status status) { return static_cast<ue4ss_lua_host_status>(status); }
VmHandle native(ue4ss_lua_vm_handle handle) { return {handle.id, handle.generation}; }
std::mutex runtime_registry_mutex;
std::unordered_map<std::uint64_t, HostApiRuntime*> runtime_registry;
std::uint64_t next_runtime{1};
ue4ss_lua_host_status protected_native_callback(lua_State* actual,
    ue4ss_lua_host_callback_v1 function,ue4ss_lua_context_handle context) noexcept {
    if (!actual) {
        try {return function(context);} catch (...) {return UE4SS_LUA_HOST_INTERNAL_ERROR;}
    }
    struct Request {ue4ss_lua_host_callback_v1 function;ue4ss_lua_context_handle context;ue4ss_lua_host_status status;bool failed;};
    Request request{function,context,UE4SS_LUA_HOST_INTERNAL_ERROR,false};
    const auto bridge=+[](void* opaque) noexcept {
        auto* r=static_cast<Request*>(opaque);
        try {r->status=r->function(r->context);} catch (...) {r->failed=true;}
    };
    if(uikit_lua_native_run(actual,&request,bridge)!=LUA_OK || request.failed)
        return UE4SS_LUA_HOST_INTERNAL_ERROR;
    return request.status;
}
}

struct HostApiRuntime::CodeOwner {
    HostApiRuntime& runtime;
    ModuleHandle module;
    CodeOwner* previous;
    CodeOwner(HostApiRuntime& runtime_, ModuleHandle module_)
        : runtime(runtime_), module(module_), previous(owner_) { owner_ = this; }
    ~CodeOwner() { assert(owner_ == this); owner_ = previous; }
    CodeOwner(const CodeOwner&) = delete;
    CodeOwner& operator=(const CodeOwner&) = delete;
};
thread_local HostApiRuntime::CodeOwner* HostApiRuntime::owner_{};
struct HostApiRuntime::Mask {
    Entry* previous;
    Mask() : previous(current_) { current_ = nullptr; }
    ~Mask() { current_ = previous; }
};

struct HostApiRuntime::Entry {
    HostApiRuntime& runtime;
    const ScopeLease& scope;
    Entry* previous;
    CodeOwner owner;
    const LuaValueContext* values{};
    const LuaStackBinding* stack{};
    bool lua_callback{};
    std::uint32_t results{};
    Entry(HostApiRuntime& runtime_, const ScopeLease& scope_,
          const LuaValueContext* values_ = nullptr, const LuaStackBinding* stack_ = nullptr)
        : runtime(runtime_), scope(scope_), previous(current_), owner(runtime_, scope_.module()),
          values(values_), stack(stack_) { current_ = this; }
    ~Entry() { assert(current_ == this); current_ = previous; }
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
};
thread_local HostApiRuntime::Entry* HostApiRuntime::current_{};

struct HostApiRuntime::OwnedEvent {
    struct Payload {
        std::shared_ptr<CallbackLease> pin;
        HostApiRuntime* runtime;
        ModuleHandle module;
        std::function<void()> function;
        Payload(CallbackLease&& p,HostApiRuntime* r,ModuleHandle m,std::function<void()>&& f)
            :pin(std::make_shared<CallbackLease>(std::move(p))),runtime(r),module(m),function(std::move(f)) {}
        ~Payload() {
            const auto reset=[](void* opaque) {static_cast<Payload*>(opaque)->function={};};
            if(runtime->manager_disposer_) runtime->manager_disposer_(module,pin,this,reset);
            else runtime->run_code_manager_destruction(module,this,reset);
        }
    };
    ModuleHandle module;
    VmHandle vm;
    std::atomic<std::shared_ptr<Payload>> payload;
    OwnedEvent(ModuleHandle m,VmHandle v,std::shared_ptr<Payload> p)
        :module(m),vm(v),payload(std::move(p)) {}
};

HostApiRuntime::HostApiRuntime(Lifecycle& life) : lifecycle_(life) {
    std::lock_guard lock{runtime_registry_mutex};
    if (next_runtime == std::numeric_limits<std::uint64_t>::max()) throw std::bad_alloc{};
    runtime_id_ = next_runtime++;
    runtime_registry.emplace(runtime_id_, this);
}
HostApiRuntime* HostApiRuntime::callback_runtime(std::uint64_t id) noexcept {
    try {
        std::lock_guard lock{runtime_registry_mutex};
        const auto it = runtime_registry.find(id);
        return it == runtime_registry.end() ? nullptr : it->second;
    } catch (...) { return nullptr; }
}
HostApiRuntime::~HostApiRuntime() {
    // No consumer execution, Lua access or unload from a destructor.
    assert(callbacks_.empty());
    assert(functions_.empty());
    assert(closures_.empty());
    std::lock_guard lock{runtime_registry_mutex}; runtime_registry.erase(runtime_id_);
}
bool HostApiRuntime::finish_scalar_bootstrap() noexcept {
    if(!callback_origin_ || !stack_origin_ || !callback_dispatch_) return false;
    scalar_ready_=true;return true;
}
std::uint64_t HostApiRuntime::ready_capabilities() const noexcept {
    if(!scalar_ready_ || !callback_origin_ || !stack_origin_ || !callback_dispatch_)return 0;
    const auto scalar=UE4SS_LUA_HOST_CAP_SCALARS_V1;
#if defined(UE4SS_DIAGNOSTIC_OBJECT_VALUES_V1) && UE4SS_DIAGNOSTIC_OBJECT_VALUES_V1
    // Explicit diagnostic-only opt-in. Installing an object backend is not
    // engine acceptance; shipping source keeps CAP_VALUES_V1 disabled.
    return object_operations_.backend && object_operations_.read && object_operations_.push
        ? scalar|UE4SS_LUA_HOST_CAP_VALUES_V1:scalar;
#else
    return scalar;
#endif
}
const ue4ss_uikit_lua_host_api_v1& HostApiRuntime::table() noexcept {
    // Immutable tables: acquiring a new table samples actual host bootstrap.
    // An earlier missing-backend table never becomes enabled in place.
    static const ue4ss_uikit_lua_host_api_v1 tables[] = {
        {UE4SS_UIKIT_LUA_HOST_ABI_VERSION,sizeof(ue4ss_uikit_lua_host_api_v1),0,
         current_context,register_callback,invoke,request_vm_close,request_module_retire,
         stack_top,stack_set_top,value_read,upvalue_read,value_push,table_set_field,
         reference_create,reference_push,reference_release,protected_call,callback_push,callback_return,object_read,object_push},
        {UE4SS_UIKIT_LUA_HOST_ABI_VERSION,sizeof(ue4ss_uikit_lua_host_api_v1),UE4SS_LUA_HOST_CAP_SCALARS_V1,
         current_context,register_callback,invoke,request_vm_close,request_module_retire,
         stack_top,stack_set_top,value_read,upvalue_read,value_push,table_set_field,
         reference_create,reference_push,reference_release,protected_call,callback_push,callback_return,object_read,object_push},
        {UE4SS_UIKIT_LUA_HOST_ABI_VERSION,sizeof(ue4ss_uikit_lua_host_api_v1),UE4SS_LUA_HOST_CAP_SCALARS_V1|UE4SS_LUA_HOST_CAP_VALUES_V1,
         current_context,register_callback,invoke,request_vm_close,request_module_retire,
         stack_top,stack_set_top,value_read,upvalue_read,value_push,table_set_field,
         reference_create,reference_push,reference_release,protected_call,callback_push,callback_return,object_read,object_push},
    };
    const auto capabilities=HostBindings::instance().api().ready_capabilities();
    return tables[(capabilities&UE4SS_LUA_HOST_CAP_VALUES_V1)?2:(capabilities&UE4SS_LUA_HOST_CAP_SCALARS_V1)?1:0];
}

ue4ss_lua_host_status HostApiRuntime::dispatch(
    VmHandle vm, ModuleHandle module, ue4ss_lua_host_callback_v1 function) noexcept {
    if (!function) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        return dispatch_host(vm, module, [function](auto context) { return function(context); });
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status HostApiRuntime::dispatch_host(
    VmHandle vm, ModuleHandle module,
    const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>& function) {
    if (!function) return UE4SS_LUA_HOST_INVALID_HANDLE;
    // Exceptions may propagate to the host's existing logging boundary. The C
    // ABI dispatch()/invoke() boundaries still contain all consumer exceptions.
    auto callback = lifecycle_.enter_callback(module);
    if (callback.status != Status::ok) return code(callback.status);
    auto scope = lifecycle_.enter_scope(vm, callback.value);
    if (scope.status != Status::ok) return code(scope.status);
    Entry entry{*this, scope.value};
    auto status = function({scope.value.handle().id, scope.value.handle().generation});
    return status <= UE4SS_LUA_HOST_INTERNAL_ERROR ? status : ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
}
ue4ss_lua_host_status HostApiRuntime::dispatch_lua(
    VmHandle vm, ModuleHandle module, const LuaStackBinding& stack,
    const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>& function) {
    if (!function || !stack.root || !stack.actual) return UE4SS_LUA_HOST_INVALID_HANDLE;
    if (stack_origin_) {
        const auto status=stack_origin_(vm,stack);
        if(status!=Status::ok) return code(status);
    }
    auto callback = lifecycle_.enter_callback(module);
    if (callback.status != Status::ok) return code(callback.status);
    auto scope = lifecycle_.enter_scope(vm, callback.value);
    if (scope.status != Status::ok) return code(scope.status);
    LuaValueSerialization serialization{lifecycle_, scope.value, stack.actual};
    if (serialization.status() != UE4SS_LUA_HOST_OK) return serialization.status();
    // Production stack-origin policy identifies a genuine attached LMS root.
    // Keep its SDK execution marker outside the private C error boundary, so
    // nested SDK calls borrow it without creating longjmp-skippable owners.
    std::optional<LuaMadeSimple::LuaExecutionScope> sdk_execution;
    if (stack_origin_) sdk_execution.emplace(stack.actual, stack.root);
    if (!uikit_lua_entry_matches_root(stack.actual, stack.root)) return UE4SS_LUA_HOST_WRONG_OWNER;
    // Bootstrap/reservation is protected C POD code, never a potentially
    // allocating lua_checkstack across the C++ lease/serialization frames.
    if (uikit_lua_entry_reserve(stack.actual) != LUA_OK) return UE4SS_LUA_HOST_INTERNAL_ERROR;
    auto admission = lifecycle_.validate_scope(scope.value);
    if (admission != Status::ok) return code(admission);
    LuaValueContext values{scope.value, serialization, stack.trampoline,
                           stack.first_consumer_upvalue, stack.consumer_upvalue_count};
    Entry entry{*this, scope.value, &values, &stack};
    std::exception_ptr exception;
    struct Request {
        const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>* function;
        ue4ss_lua_context_handle context;
        ue4ss_lua_host_status status;
        std::exception_ptr* exception;
    } request{&function,{scope.value.handle().id,scope.value.handle().generation},UE4SS_LUA_HOST_INTERNAL_ERROR,&exception};
    const auto bridge=+[](void* opaque) noexcept {
        auto* r=static_cast<Request*>(opaque);
        try {r->status=(*r->function)(r->context);} catch (...) {*r->exception=std::current_exception();}
    };
    const auto lua_status=uikit_lua_native_run(stack.actual,&request,bridge);
    if(exception) std::rethrow_exception(exception); // ordinary host logging boundary retains exact C++ exception
    if(lua_status!=LUA_OK) return UE4SS_LUA_HOST_INTERNAL_ERROR;
    return request.status <= UE4SS_LUA_HOST_INTERNAL_ERROR ? request.status : ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
}

HostApiRuntime::Entry* HostApiRuntime::resolve_value_context(
    ue4ss_lua_context_handle context, ue4ss_lua_host_status& status) noexcept {
    status = UE4SS_LUA_HOST_INVALID_HANDLE;
    if (!current_) return nullptr;
    try {
        status = code(current_->runtime.lifecycle_.validate_scope(current_->scope));
        if (status != UE4SS_LUA_HOST_OK) return nullptr;
        const auto actual = current_->scope.handle();
        if (!context.id || !context.generation || context.id != actual.id) {
            status = UE4SS_LUA_HOST_INVALID_HANDLE; return nullptr;
        }
        if (context.generation != actual.generation) {
            status = UE4SS_LUA_HOST_STALE_GENERATION; return nullptr;
        }
        if (!current_->values || !current_->stack) {
            status = UE4SS_LUA_HOST_INVALID_HANDLE; return nullptr;
        }
        return current_;
    } catch (...) { status = UE4SS_LUA_HOST_INTERNAL_ERROR; return nullptr; }
}
Status HostApiRuntime::dispatch_code(ModuleHandle module, const std::function<void()>& function) {
    if (!function) return Status::invalid_handle;
    auto callback = lifecycle_.enter_callback(module);
    if (callback.status != Status::ok) return callback.status;
    CodeOwner owner{*this, module};
    Mask mask;
    function();
    return Status::ok;
}

Status HostApiRuntime::dispatch_finalizer(ModuleHandle module, const std::function<void()>& function) {
    if (!function) return Status::invalid_handle;
    auto status = lifecycle_.validate_module_finalizer(module);
    if (status != Status::ok) return status;
    CodeOwner owner{*this, module};
    Mask mask;
    function();
    return Status::ok;
}
Result<ModuleHandle> HostApiRuntime::code_owner() const {
    if (!owner_ || &owner_->runtime != this) return {Status::invalid_handle, {}};
    auto state = lifecycle_.module_state(owner_->module);
    if (state.status != Status::ok) return {state.status, {}};
    if (state.value != ModuleState::live) return {Status::retired, {}};
    return {Status::ok, owner_->module};
}
Status HostApiRuntime::run_host_callback(lua_State* actual, void* payload, void (*body)(void*)) {
    if (!body) return Status::invalid_handle;
    CodeOwner owner{*this, {}};
    Mask mask;
    if (!actual) { body(payload); return Status::ok; }
    std::exception_ptr exception;
    struct Request { void* payload; void (*body)(void*); std::exception_ptr* exception; }
        request{payload, body, &exception};
    const auto invoke = +[](void* opaque) noexcept {
        auto* request = static_cast<Request*>(opaque);
        try { request->body(request->payload); }
        catch (...) { *request->exception = std::current_exception(); }
    };
    const auto status = uikit_lua_native_run(actual, &request, invoke);
    if (exception) std::rethrow_exception(exception);
    return status == LUA_OK ? Status::ok : Status::internal_error;
}
void HostApiRuntime::run_code_manager_destruction(
    ModuleHandle module, void* payload, void (*destroy)(void*)) noexcept {
    // Caller is the trusted broker, with its existing token code pin held.
    // No admission or shared control lock: destruction must also run on retire.
    if (!destroy) return;
    CodeOwner owner{*this, module};
    Mask mask;
    try { destroy(payload); } catch (...) { /* C/manager boundary contains exceptions. */ }
}
Result<std::uint64_t> HostApiRuntime::register_lms_function(
    VmHandle vm, int (*function)(const RC::LuaMadeSimple::Lua&)) {
    if (!function) return {Status::invalid_handle, {}};
    auto owner = code_owner();
    if (owner.status != Status::ok) return {owner.status, {}};
    if (current_ && (&current_->runtime != this || current_->scope.vm() != vm))
        return {Status::wrong_owner, {}};
    auto callback = lifecycle_.enter_callback(owner.value);
    if (callback.status != Status::ok) return {callback.status, {}};
    auto scope = lifecycle_.enter_scope(vm, callback.value);
    if (scope.status != Status::ok) return {scope.status, {}};
    std::uint64_t ticket{};
    try {
        std::lock_guard lock{functions_mutex_};
        if (next_function_ == std::numeric_limits<std::uint64_t>::max()) return {Status::internal_error, {}};
        ticket = next_function_++;
        functions_.emplace(ticket, NativeFunction{ticket, vm, owner.value, function});
    } catch (...) { return {Status::internal_error, {}}; }
    const auto status = lifecycle_.validate_scope(scope.value);
    if (status != Status::ok) {
        std::lock_guard lock{functions_mutex_}; functions_.erase(ticket);
        return {status, {}};
    }
    return {Status::ok, ticket};
}
Result<int> HostApiRuntime::dispatch_lms_function(
    std::uint64_t ticket, const RC::LuaMadeSimple::Lua& lua,
    const LuaStackBinding& stack) noexcept {
    try {
        VmHandle vm{}; ModuleHandle module{};
        {
            std::lock_guard lock{functions_mutex_};
            const auto found = functions_.find(ticket);
            if (found == functions_.end()) return {Status::invalid_handle, {}};
            // Only metadata is read here. Code pointer remains inaccessible
            // until dispatch_lua has acquired both owner/VM leases + the gate.
            vm = found->second.vm; module = found->second.module;
        }
        int result{};
        const auto status = dispatch_lua(vm, module, stack, [&](auto) {
            int (*function)(const RC::LuaMadeSimple::Lua&){};
            {
                std::lock_guard lock{functions_mutex_};
                const auto found = functions_.find(ticket);
                if (found == functions_.end()) return ue4ss_lua_host_status{UE4SS_LUA_HOST_INVALID_HANDLE};
                function = found->second.function;
            }
            struct NativeRequest {
                int (*function)(const RC::LuaMadeSimple::Lua&);
                const RC::LuaMadeSimple::Lua* lua;
                int result;
                bool failed;
            } request{function, &lua, 0, false};
            const auto bridge = +[](void* opaque) noexcept {
                auto* request = static_cast<NativeRequest*>(opaque);
                try { request->result = request->function(*request->lua); }
                catch (...) { request->failed = true; }
            };
            // Keep actual arguments/upvalues in their original CallInfo. The
            // private C protected boundary returns every Lua error before any
            // host module/VM lease or TLS scope needs to be destroyed.
            if (uikit_lua_native_run(stack.actual, &request, bridge) != LUA_OK || request.failed)
                return ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
            result = request.result;
            return result < 0 ? ue4ss_lua_host_status{UE4SS_LUA_HOST_INVALID_HANDLE} :
                                ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
        });
        return {static_cast<Status>(status), result};
    } catch (...) { return {Status::internal_error, {}}; }
}

ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::current_context(
    ue4ss_lua_host_context_info_v1* output) noexcept {
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    auto prefix = ue4ss_lua_host_validate_prefix(output->abi_version, output->struct_size,
                                               sizeof(*output));
    if (prefix != UE4SS_LUA_HOST_OK) return prefix;
    if (!current_) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        auto status = current_->runtime.lifecycle_.validate_scope(current_->scope);
        if (status != Status::ok) return code(status);
        const auto& scope = current_->scope;
        *output = {UE4SS_UIKIT_LUA_HOST_ABI_VERSION, sizeof(*output),
                   {scope.handle().id, scope.handle().generation},
                   {scope.vm().id, scope.vm().generation},
                   {scope.module().id, scope.module().generation}};
        return UE4SS_LUA_HOST_OK;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::register_callback(
    ue4ss_lua_host_callback_v1 function, ue4ss_lua_callback_handle* output) noexcept {
    if (!function || !output || !current_) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = {};
    try {
        auto& runtime = current_->runtime;
        auto status = runtime.lifecycle_.validate_scope(current_->scope);
        if (status != Status::ok) return code(status);
        if (runtime.callback_origin_) {
            status = runtime.callback_origin_(current_->scope.module(),
                reinterpret_cast<const void*>(function));
            if (status != Status::ok) return code(status);
        }
        ue4ss_lua_callback_handle handle{};
        {
            std::lock_guard lock{runtime.callbacks_mutex_};
            if (runtime.next_callback_ == std::numeric_limits<std::uint64_t>::max())
                return UE4SS_LUA_HOST_INTERNAL_ERROR;
            handle = {runtime.next_callback_++, 1};
            runtime.callbacks_.emplace(handle.id, Callback{handle, current_->scope.vm(),
                                                         current_->scope.module(), function});
        }
        status = runtime.lifecycle_.validate_scope(current_->scope);
        if (status != Status::ok) {
            std::lock_guard lock{runtime.callbacks_mutex_};
            runtime.callbacks_.erase(handle.id);
            return code(status);
        }
        *output = handle;
        return UE4SS_LUA_HOST_OK;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
bool HostApiRuntime::install_callback_origin(
    Status (*policy)(ModuleHandle, const void*) noexcept) noexcept {
    if (!policy || callback_origin_ || callback_count()) return false;
    callback_origin_ = policy;
    return true;
}
bool HostApiRuntime::install_stack_origin(Status (*policy)(VmHandle,const LuaStackBinding&) noexcept) noexcept {
    if(!policy || stack_origin_) return false;
    stack_origin_=policy;return true;
}
bool HostApiRuntime::install_callback_dispatch(ue4ss_lua_host_status (*dispatcher)(
    VmHandle, ModuleHandle, const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>&)) noexcept {
    if (!dispatcher || callback_dispatch_) return false;
    callback_dispatch_ = dispatcher;
    return true;
}
bool HostApiRuntime::install_manager_disposer(void (*disposer)(ModuleHandle,
    const std::shared_ptr<void>&,void*,void (*)(void*)) noexcept) noexcept {
    if(!disposer || manager_disposer_) return false;
    manager_disposer_=disposer;return true;
}
Result<VmHandle> HostApiRuntime::registered_callback_vm(ModuleHandle module) const {
    VmHandle vm{};
    {
        std::scoped_lock lock{callbacks_mutex_,functions_mutex_};
        for (const auto& [id, record] : callbacks_) {
            (void)id;
            if (record.module != module) continue;
            if (vm.id && vm != record.vm) return {Status::wrong_owner, {}};
            vm = record.vm; // no Native function pointer is read
        }
        for (const auto& [id, record] : functions_) {
            (void)id;
            if (record.module != module) continue;
            if (vm.id && vm != record.vm) return {Status::wrong_owner, {}};
            vm = record.vm; // actual host-managed LuaMadeSimple registration
        }
    }
    if (!vm.id) return {Status::invalid_handle, {}};
    const auto state = lifecycle_.vm_state(vm);
    if (state.status != Status::ok) return {state.status, {}};
    if (state.value != VmState::open) return {Status::retired, {}};
    return {Status::ok, vm};
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::invoke(
    ue4ss_lua_vm_handle vm, ue4ss_lua_callback_handle handle) noexcept {
    if (!current_ || !handle.id || !handle.generation) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        auto& runtime = current_->runtime;
        auto status = runtime.lifecycle_.validate_scope(current_->scope);
        if (status != Status::ok) return code(status);
        if (native(vm) != current_->scope.vm()) return UE4SS_LUA_HOST_WRONG_OWNER;
        {
            std::lock_guard lock{runtime.callbacks_mutex_};
            auto it = runtime.callbacks_.find(handle.id);
            if (it == runtime.callbacks_.end()) return UE4SS_LUA_HOST_INVALID_HANDLE;
            if (it->second.handle.generation != handle.generation) return UE4SS_LUA_HOST_STALE_GENERATION;
            if (it->second.module != current_->scope.module() || it->second.vm != native(vm))
                return UE4SS_LUA_HOST_WRONG_OWNER;
        }
        // Acquire the owner lease before reading a consumer function pointer.
        auto callback = runtime.lifecycle_.enter_callback(current_->scope.module());
        if (callback.status != Status::ok) return code(callback.status);
        auto scope = runtime.lifecycle_.enter_scope(native(vm), callback.value);
        if (scope.status != Status::ok) return code(scope.status);
        ue4ss_lua_host_callback_v1 function{};
        {
            std::lock_guard lock{runtime.callbacks_mutex_};
            auto it = runtime.callbacks_.find(handle.id);
            if (it == runtime.callbacks_.end()) return UE4SS_LUA_HOST_INVALID_HANDLE;
            if (it->second.handle.generation != handle.generation) return UE4SS_LUA_HOST_STALE_GENERATION;
            function = it->second.function;
        }
        // Neither the callback registry nor lifecycle mutex is held here.
        const auto call = [&](const LuaValueContext* values, const LuaStackBinding* stack) {
            Entry entry{runtime, scope.value, values, stack};
            auto result = protected_native_callback(stack?stack->actual:nullptr,function,
                {scope.value.handle().id, scope.value.handle().generation});
            return result <= UE4SS_LUA_HOST_INTERNAL_ERROR ? result : ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
        };
        if (current_->values && current_->stack) {
            std::int32_t top{};
            const auto value_status = runtime.values_.stack_top(*current_->values,
                {current_->scope.handle().id, current_->scope.handle().generation}, &top);
            if (value_status != UE4SS_LUA_HOST_OK) return value_status;
            const auto stack = *current_->stack;
            LuaValueSerialization serialization{runtime.lifecycle_, scope.value, stack.actual};
            if (serialization.status() != UE4SS_LUA_HOST_OK) return serialization.status();
            LuaValueContext values{scope.value, serialization, stack.trampoline,
                                   stack.first_consumer_upvalue, stack.consumer_upvalue_count};
            return call(&values, &stack);
        }
        return call(nullptr, nullptr);
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::request_vm_close(
    ue4ss_lua_vm_handle vm) noexcept {
    if (!current_) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        auto status = current_->runtime.lifecycle_.validate_scope(current_->scope);
        if (status != Status::ok) return code(status);
        if (native(vm) != current_->scope.vm()) return UE4SS_LUA_HOST_WRONG_OWNER;
        return code(current_->runtime.lifecycle_.request_vm_close(native(vm)));
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::request_module_retire() noexcept {
    if (!current_) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        auto status = current_->runtime.lifecycle_.validate_scope(current_->scope);
        if (status != Status::ok) return code(status);
        return code(current_->runtime.lifecycle_.request_module_retire(current_->scope.module()));
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::stack_top(
    ue4ss_lua_context_handle context, std::int32_t* output) noexcept {
    ue4ss_lua_host_status status{};
    auto* entry = resolve_value_context(context, status);
    return entry ? entry->runtime.values_.stack_top(*entry->values, context, output) : status;
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::stack_set_top(
    ue4ss_lua_context_handle context, std::int32_t index) noexcept {
    ue4ss_lua_host_status status{};
    auto* entry = resolve_value_context(context, status);
    return entry ? entry->runtime.values_.stack_set_top(*entry->values, context, index) : status;
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::value_read(
    ue4ss_lua_context_handle context, std::int32_t index, ue4ss_lua_value_v1* output) noexcept {
    ue4ss_lua_host_status status{};
    auto* entry = resolve_value_context(context, status);
    if (!entry) return status;
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    ue4ss_lua_value_v1 value{};value.abi_version=output->abi_version;value.struct_size=output->struct_size;
    status=entry->runtime.values_.value_read(*entry->values,context,index,&value);
    if (status==UE4SS_LUA_HOST_OK && value.kind==UE4SS_LUA_VALUE_OTHER)
        status=entry->runtime.tokens_.read(*entry->values,context,entry->stack->actual,index,&value);
    if (status==UE4SS_LUA_HOST_OK) *output=value;
    return status;
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::upvalue_read(
    ue4ss_lua_context_handle context, std::uint32_t ordinal, ue4ss_lua_value_v1* output) noexcept {
    ue4ss_lua_host_status status{};
    auto* entry = resolve_value_context(context, status);
    if (!entry) return status;
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    ue4ss_lua_value_v1 value{};value.abi_version=output->abi_version;value.struct_size=output->struct_size;
    status=entry->runtime.values_.upvalue_read(*entry->values,context,ordinal,&value);
    if (status==UE4SS_LUA_HOST_OK && value.kind==UE4SS_LUA_VALUE_OTHER)
        status=entry->runtime.tokens_.read_upvalue(*entry->values,context,entry->stack->actual,ordinal,&value);
    if (status==UE4SS_LUA_HOST_OK) *output=value;
    return status;
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::value_push(
    ue4ss_lua_context_handle context, const ue4ss_lua_value_v1* input) noexcept {
    ue4ss_lua_host_status status{};
    auto* entry = resolve_value_context(context, status);
    if (!entry) return status;
    if (!input) return UE4SS_LUA_HOST_INVALID_HANDLE;
    const auto prefix=ue4ss_lua_host_validate_prefix(input->abi_version,input->struct_size,sizeof(*input));
    if (prefix!=UE4SS_LUA_HOST_OK) return prefix;
    if (input->kind==UE4SS_LUA_VALUE_TOKEN)
        return entry->runtime.tokens_.push(*entry->values,context,entry->stack->actual,input->token);
    return entry->runtime.values_.value_push(*entry->values,context,input);
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::table_set_field(
    ue4ss_lua_context_handle context, std::int32_t index, const char* key, std::uint64_t size) noexcept {
    ue4ss_lua_host_status status{};
    auto* entry = resolve_value_context(context, status);
    return entry ? entry->runtime.values_.table_set_field(*entry->values, context, index, key, size) : status;
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::protected_call(
    ue4ss_lua_context_handle context, std::int32_t nargs, std::int32_t nresults) noexcept {
    try {
        ue4ss_lua_host_status status{};
        auto* entry = resolve_value_context(context, status);
        if (!entry) return status;
        LuaMadeSimple::LuaExecutionScope execution{entry->stack->actual, entry->stack->root};
        return entry->runtime.values_.protected_call(*entry->values, context, nargs, nresults);
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::reference_create(
    ue4ss_lua_context_handle context, std::int32_t index, ue4ss_lua_reference_handle* output) noexcept {
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = {};
    try {
        ue4ss_lua_host_status status{};
        auto* entry = resolve_value_context(context, status);
        if (!entry) return status;
        if (!entry->stack->references) return UE4SS_LUA_HOST_INVALID_HANDLE;
        std::int32_t top{};
        status = entry->runtime.values_.stack_top(*entry->values, context, &top);
        if (status != UE4SS_LUA_HOST_OK) return status;
        auto result = entry->stack->references->retain(entry->scope, index, entry->stack->actual);
        if (result.status == Status::ok) *output = {result.value.id, result.value.generation};
        return code(result.status);
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::reference_push(
    ue4ss_lua_context_handle context, ue4ss_lua_reference_handle reference) noexcept {
    try {
        ue4ss_lua_host_status status{};
        auto* entry = resolve_value_context(context, status);
        if (!entry) return status;
        if (!entry->stack->references) return UE4SS_LUA_HOST_INVALID_HANDLE;
        std::int32_t top{};
        status = entry->runtime.values_.stack_top(*entry->values, context, &top);
        if (status != UE4SS_LUA_HOST_OK) return status;
        return code(entry->stack->references->push(entry->scope, {reference.id, reference.generation}, entry->stack->actual));
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::reference_release(
    ue4ss_lua_context_handle context, ue4ss_lua_reference_handle reference) noexcept {
    try {
        ue4ss_lua_host_status status{};
        auto* entry = resolve_value_context(context, status);
        if (!entry) return status;
        if (!entry->stack->references) return UE4SS_LUA_HOST_INVALID_HANDLE;
        std::int32_t top{};
        status = entry->runtime.values_.stack_top(*entry->values, context, &top);
        if (status != UE4SS_LUA_HOST_OK) return status;
        return code(entry->stack->references->release(entry->scope, {reference.id, reference.generation}, entry->stack->actual));
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::callback_push(
    ue4ss_lua_context_handle context, ue4ss_lua_callback_handle callback, std::uint32_t upvalues) noexcept {
    std::uint64_t emission{}; HostApiRuntime* runtime{}; lua_State* state{}; int old_top=-1;
    try {
        ue4ss_lua_host_status status{}; auto* entry=resolve_value_context(context,status);
        if (!entry) return status;
        runtime=&entry->runtime;
        std::int32_t top{};status=runtime->values_.stack_top(*entry->values,context,&top);
        if (status!=UE4SS_LUA_HOST_OK) return status;
        if (upvalues>253 || upvalues>static_cast<std::uint32_t>(top)) return UE4SS_LUA_HOST_INVALID_HANDLE;
        {
            std::lock_guard lock{runtime->callbacks_mutex_};
            const auto it=runtime->callbacks_.find(callback.id);
            if (it==runtime->callbacks_.end()) return UE4SS_LUA_HOST_INVALID_HANDLE;
            if (it->second.handle.generation!=callback.generation) return UE4SS_LUA_HOST_STALE_GENERATION;
            if (it->second.vm!=entry->scope.vm() || it->second.module!=entry->scope.module()) return UE4SS_LUA_HOST_WRONG_OWNER;
        }
        state=entry->stack->actual;old_top=top;
        {
            std::lock_guard lock{runtime->closures_mutex_};
            if (runtime->next_closure_==std::numeric_limits<std::uint64_t>::max()) return UE4SS_LUA_HOST_INTERNAL_ERROR;
            emission=runtime->next_closure_++;
            runtime->closures_.emplace(emission,Closure{emission,callback,entry->scope.vm(),entry->scope.module(),*entry->stack,nullptr,nullptr,upvalues});
        }
        uikit_lua_callback_request request{runtime->runtime_id_,callback.id,callback.generation,emission,upvalues,0,nullptr,nullptr};
        const auto result=uikit_lua_callback_push(state,&request);
        status=result==LUA_OK?code(runtime->lifecycle_.validate_scope(entry->scope)):ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
        if (status!=UE4SS_LUA_HOST_OK) {
            uikit_lua_value_pop_to(state,old_top);
            std::lock_guard lock{runtime->closures_mutex_};runtime->closures_.erase(emission);
            return status;
        }
        {
            std::lock_guard lock{runtime->closures_mutex_};
            auto& record=runtime->closures_.at(emission);record.userdata=request.userdata;record.identity=request.closure;
        }
        uikit_lua_callback_commit(state,upvalues);
        return UE4SS_LUA_HOST_OK;
    } catch (...) {
        if (state && old_top>=0) uikit_lua_value_pop_to(state,old_top);
        if (runtime && emission) { std::lock_guard lock{runtime->closures_mutex_};runtime->closures_.erase(emission); }
        return UE4SS_LUA_HOST_INTERNAL_ERROR;
    }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::callback_return(
    ue4ss_lua_context_handle context, std::uint32_t count) noexcept {
    ue4ss_lua_host_status status{};auto* entry=resolve_value_context(context,status);
    if (!entry) return status;
    if (!entry->lua_callback) return UE4SS_LUA_HOST_INVALID_HANDLE;
    std::int32_t top{};status=entry->runtime.values_.stack_top(*entry->values,context,&top);
    if (status!=UE4SS_LUA_HOST_OK) return status;
    if (count>static_cast<std::uint32_t>(top)) return UE4SS_LUA_HOST_INVALID_HANDLE;
    entry->results=count;return UE4SS_LUA_HOST_OK;
}
Result<int> HostApiRuntime::dispatch_lua_closure(lua_State* actual, void* userdata, std::uint64_t emission) noexcept {
    try {
        Closure record{};
        {
            std::lock_guard lock{closures_mutex_};const auto it=closures_.find(emission);
            if (it==closures_.end() || it->second.userdata!=userdata) return {Status::invalid_handle,{}};
            record=it->second; // host metadata only, no consumer pointer
        }
        auto callback=lifecycle_.enter_callback(record.module);
        if (callback.status!=Status::ok) return {callback.status,{}};
        auto scope=lifecycle_.enter_scope(record.vm,callback.value);
        if (scope.status!=Status::ok) return {scope.status,{}};
        LuaValueSerialization serialization{lifecycle_,scope.value,actual};
        if (serialization.status()!=UE4SS_LUA_HOST_OK) return {static_cast<Status>(serialization.status()),{}};
        if (!uikit_lua_entry_matches_root(actual,record.stack.root) ||
            uikit_lua_callback_current_identity(actual)!=record.identity) return {Status::wrong_owner,{}};
        if (uikit_lua_entry_reserve(actual)!=LUA_OK) return {Status::internal_error,{}};
        const auto admission=lifecycle_.validate_scope(scope.value);
        if (admission!=Status::ok) return {admission,{}};
        LuaStackBinding stack=record.stack;stack.actual=actual;stack.trampoline=uikit_lua_callback_entry;
        stack.first_consumer_upvalue=3;stack.consumer_upvalue_count=record.upvalues;
        LuaValueContext values{scope.value,serialization,stack.trampoline,3,record.upvalues};
        ue4ss_lua_host_callback_v1 function{};
        {
            std::lock_guard lock{callbacks_mutex_};const auto it=callbacks_.find(record.callback.id);
            if (it==callbacks_.end()) return {Status::invalid_handle,{}};
            if (it->second.handle.generation!=record.callback.generation) return {Status::stale_generation,{}};
            if (it->second.vm!=record.vm || it->second.module!=record.module) return {Status::wrong_owner,{}};
            function=it->second.function;
        }
        Entry entry{*this,scope.value,&values,&stack};entry.lua_callback=true;
        const auto result=protected_native_callback(actual,function,
            {scope.value.handle().id,scope.value.handle().generation});
        if (result>UE4SS_LUA_HOST_INTERNAL_ERROR) return {Status::internal_error,{}};
        if (result!=UE4SS_LUA_HOST_OK) return {static_cast<Status>(result),{}};
        std::int32_t top{};const auto status=values_.stack_top(values,
            {scope.value.handle().id,scope.value.handle().generation},&top);
        if (status!=UE4SS_LUA_HOST_OK) return {static_cast<Status>(status),{}};
        if (entry.results>static_cast<std::uint32_t>(top)) return {Status::invalid_handle,{}};
        return {Status::ok,static_cast<int>(entry.results)};
    } catch (...) { return {Status::internal_error,{}}; }
}
void HostApiRuntime::collect_lua_closure(void* userdata,std::uint64_t emission) noexcept {
    try {
        std::lock_guard lock{closures_mutex_};const auto it=closures_.find(emission);
        if (it!=closures_.end() && it->second.userdata==userdata) closures_.erase(it);
    } catch (...) {}
}
bool HostApiRuntime::install_object_operations(const ObjectOperations& operations) noexcept {
    // Bootstrap-only before consumer dispatch; immutable handler lifetime is
    // stable host code and persists through every associated actual lua_close.
    if (object_operations_.backend || !operations.backend || !operations.read || !operations.push) return false;
    object_operations_=operations;return true;
}
lua_State* HostApiRuntime::active_root_for_stack(lua_State* actual) noexcept {
    try {
        if (!current_ || !current_->stack || current_->stack->actual!=actual ||
            current_->runtime.lifecycle_.validate_scope(current_->scope)!=Status::ok) return nullptr;
        return current_->stack->root;
    } catch (...) { return nullptr; }
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::object_read(
    ue4ss_lua_context_handle context,std::int32_t index,void** output) noexcept {
    ue4ss_lua_host_status status{};auto* entry=resolve_value_context(context,status);
    if (!entry) return status;
    const auto& operations=entry->runtime.object_operations_;
    if (!operations.backend) return UE4SS_LUA_HOST_PENDING;
    return operations.read(operations.backend,*entry->values,context,index,output);
}
ue4ss_lua_host_status UE4SS_LUA_HOST_CALL HostApiRuntime::object_push(
    ue4ss_lua_context_handle context,void* object) noexcept {
    ue4ss_lua_host_status status{};auto* entry=resolve_value_context(context,status);
    if (!entry) return status;
    const auto& operations=entry->runtime.object_operations_;
    if (!operations.backend) return UE4SS_LUA_HOST_PENDING;
    return operations.push(operations.backend,*entry->values,context,object);
}
Result<std::function<void()>> HostApiRuntime::wrap_event(std::function<void()> function) {
    if (!function || !owner_ || &owner_->runtime != this) return {Status::invalid_handle, {}};
    const auto module=owner_->module;
    auto pin=lifecycle_.enter_callback(module);
    if(pin.status!=Status::ok) return {pin.status,{}};
    const auto vm=current_ ? current_->scope.vm() : VmHandle{};
    auto payload=std::make_shared<OwnedEvent::Payload>(std::move(pin.value),this,module,std::move(function));
    auto event=std::make_shared<OwnedEvent>(module,vm,payload);
    {
        std::lock_guard lock{events_mutex_};
        if(next_event_==std::numeric_limits<std::uint64_t>::max()) return {Status::internal_error,{}};
        std::erase_if(events_,[](const auto& entry){return entry.second.slot.expired();});
        events_.emplace(next_event_++,EventRecord{module,vm,event});
    }
    const auto state=lifecycle_.module_state(module);
    if(state.status!=Status::ok || state.value!=ModuleState::live) {
        auto detached=event->payload.exchange({}); // dispose outside every registry lock
        return {state.status==Status::ok ? Status::retired : state.status,{}};
    }
    return {Status::ok,[event] {
        const auto current=event->payload.load(); // inert metadata only until leased entry
        if(!current) return;
        auto* runtime=current->runtime;
        auto vm=event->vm;
        if(!vm.id) {
            const auto associated=runtime->registered_callback_vm(event->module);
            if(associated.status==Status::ok) vm=associated.value;
            else if(associated.status==Status::retired || associated.status==Status::stale_generation) return;
        }
        if(vm.id) {
            const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)> dispatch=[&](auto) {
                current->function();return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
            };
            if(runtime->callback_dispatch_) runtime->callback_dispatch_(vm,event->module,dispatch);
            else runtime->dispatch_host(vm,event->module,dispatch);
        } else runtime->dispatch_code(event->module,current->function);
    }};
}
Status HostApiRuntime::revoke_queued_events(ModuleHandle module) {
    for(;;) {
        std::shared_ptr<OwnedEvent> event; // destruction always after registry unlock
        {
            std::lock_guard lock{events_mutex_};
            for(auto it=events_.begin();it!=events_.end();) {
                if(it->second.module==module) {
                    event=it->second.slot.lock();
                    it=events_.erase(it);
                    if(event) break;
                } else if(it->second.slot.expired()) it=events_.erase(it);
                else ++it;
            }
        }
        if(!event) break;
        auto detached=event->payload.exchange({}); // Native manager disposal outside locks
    }
    return Status::ok;
}
Status HostApiRuntime::revoke_queued_vm_events(VmHandle vm) {
    for(;;) {
        std::shared_ptr<OwnedEvent> event;
        {
            std::lock_guard lock{events_mutex_};
            for(auto it=events_.begin();it!=events_.end();) {
                if(it->second.vm==vm) {
                    event=it->second.slot.lock();
                    it=events_.erase(it);
                    if(event) break;
                } else if(it->second.slot.expired()) it=events_.erase(it);
                else ++it;
            }
        }
        if(!event) break;
        auto detached=event->payload.exchange({});
    }
    return Status::ok;
}
Status HostApiRuntime::cleanup_module(ModuleHandle module) {
    auto status = lifecycle_.validate_module_finalizer(module);
    if (status != Status::ok) return status;
    status=tokens_.cleanup_module(module);
    if (status!=Status::ok) return status;
    {
        std::lock_guard lock{closures_mutex_};
        std::erase_if(closures_, [&](const auto& item) { return item.second.module == module; });
    }
    {
        std::lock_guard lock{functions_mutex_};
        std::erase_if(functions_, [&](const auto& item) { return item.second.module == module; });
    }
    std::lock_guard lock{callbacks_mutex_};
    std::erase_if(callbacks_, [&](const auto& item) { return item.second.module == module; });
    return Status::ok;
}
Status HostApiRuntime::cleanup_vm(VmHandle vm) {
    auto status = lifecycle_.validate_vm_finalizer(vm);
    if (status != Status::ok) return status;
    status=tokens_.cleanup_vm(vm);
    if (status!=Status::ok) return status;
    {
        std::lock_guard lock{closures_mutex_};
        std::erase_if(closures_, [&](const auto& item) { return item.second.vm == vm; });
    }
    {
        std::lock_guard lock{functions_mutex_};
        std::erase_if(functions_, [&](const auto& item) { return item.second.vm == vm; });
    }
    std::lock_guard lock{callbacks_mutex_};
    std::erase_if(callbacks_, [&](const auto& item) { return item.second.vm == vm; });
    return Status::ok;
}
std::size_t HostApiRuntime::callback_count() const {
    std::lock_guard lock{callbacks_mutex_};
    return callbacks_.size();
}
}

extern "C" ue4ss_lua_host_status uikit_host_callback_dispatch(
    lua_State* state,std::uint64_t id,void* userdata,std::uint64_t emission,int* output) {
    auto* runtime=RC::LuaHost::HostApiRuntime::callback_runtime(id);
    if (!runtime || !output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    const auto result=runtime->dispatch_lua_closure(state,userdata,emission);
    if (result.status==RC::LuaHost::Status::ok) *output=result.value;
    return static_cast<ue4ss_lua_host_status>(result.status);
}
extern "C" void uikit_host_callback_collected(std::uint64_t id,void* userdata,std::uint64_t emission) {
    if (auto* runtime=RC::LuaHost::HostApiRuntime::callback_runtime(id)) runtime->collect_lua_closure(userdata,emission);
}
