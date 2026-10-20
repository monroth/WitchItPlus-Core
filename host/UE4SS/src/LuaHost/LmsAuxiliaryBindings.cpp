#include <LuaHost/LmsAuxiliaryBindings.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaHost/HostIntegration.hpp>
#include <LuaMadeSimple/HostAuxiliaryHooks.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <LuaMadeSimple/HostThreadAccess.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include "LuaNativeFrame.h"
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
namespace RC::LuaHost {
namespace {
using Kind = LuaMadeSimple::HostAuxiliaryKind;
struct Record { VmHandle vm; ModuleHandle module; Kind kind; const void* function; };
std::mutex Mutex;
std::unordered_map<std::uint64_t,Record> Records;
std::uint64_t Next{1};
std::uint32_t register_callback(const LuaMadeSimple::Lua* lua, Kind kind,
    const void* function, std::uint64_t* output) noexcept {
    if (!function || !output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = 0;
    if (kind != Kind::post && kind != Kind::error && kind != Kind::internal && kind != Kind::debug) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        const auto image = code_image(function);
        if (!image) return UE4SS_LUA_HOST_INVALID_HANDLE;
        if (image == host_code_image()) return UE4SS_LUA_HOST_OK;
        auto& host = HostBindings::instance();
        const auto module = host.module_for_image(image);
        if (module.status != Status::ok) return static_cast<std::uint32_t>(module.status);
        const auto owner = host.api().code_owner();
        if (owner.status != Status::ok) return static_cast<std::uint32_t>(owner.status);
        if (owner.value != module.value) return UE4SS_LUA_HOST_WRONG_OWNER;
        auto callback = host.lifecycle().enter_callback(module.value);
        if (callback.status != Status::ok) return static_cast<std::uint32_t>(callback.status);
        VmHandle vm{};
        std::optional<ScopeLease> scope;
        if (kind != Kind::error) {
            if (!lua) return UE4SS_LUA_HOST_INVALID_HANDLE;
            const auto actual_vm = host.find_vm(lua->get_root_lua_state());
            if (actual_vm.status != Status::ok) return static_cast<std::uint32_t>(actual_vm.status);
            vm = actual_vm.value;
            auto entered = host.lifecycle().enter_scope(vm, callback.value);
            if (entered.status != Status::ok) return static_cast<std::uint32_t>(entered.status);
            scope.emplace(std::move(entered.value));
        }
        std::uint64_t ticket;
        {
            std::lock_guard lock{Mutex};
            if (Next == std::numeric_limits<std::uint64_t>::max()) return UE4SS_LUA_HOST_INTERNAL_ERROR;
            ticket = Next++;
            Records.emplace(ticket, Record{vm,module.value,kind,function});
        }
        const auto status = scope ? host.lifecycle().validate_scope(*scope) :
            host.api().code_owner().status;
        if (status != Status::ok) {
            std::lock_guard lock{Mutex}; Records.erase(ticket);
            return static_cast<std::uint32_t>(status);
        }
        *output = ticket;
        return UE4SS_LUA_HOST_OK;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
std::uint32_t dispatch_host_auxiliary(Kind kind,const void* function,LuaMadeSimple::HostAuxiliaryInvocation* arguments) noexcept {
    if(!function || !arguments || !arguments->actual || code_image(function)!=host_code_image()) return UE4SS_LUA_HOST_WRONG_OWNER;
    try {
        const auto root=LuaMadeSimple::cached_root_lua_state(arguments->actual);
        if(!root || !LuaMadeSimple::has_host_execution_scope(root)) return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto& host=HostBindings::instance();const auto vm=host.find_vm(root);
        if(vm.status!=Status::ok) return static_cast<std::uint32_t>(vm.status);
        struct Request {Kind kind;const void* function;LuaMadeSimple::HostAuxiliaryInvocation* arguments;};
        Request request{kind,function,arguments};
        return static_cast<std::uint32_t>(host.dispatch_host_callback_lua_on_stack(vm.value,arguments->actual,&request,+[](void* opaque){
            auto* r=static_cast<Request*>(opaque);
            if(r->kind==Kind::post)
                reinterpret_cast<void (*)(const LuaMadeSimple::Lua&)>(const_cast<void*>(r->function))(*r->arguments->lua);
            else if(r->kind==Kind::error)
                reinterpret_cast<void (*)(lua_State*,const std::string&,const std::string&)>(const_cast<void*>(r->function))(r->arguments->actual,*r->arguments->message,*r->arguments->traceback);
            else throw std::runtime_error("Unsupported HOST auxiliary callback kind");
        }));
    } catch (...) {return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
std::uint32_t dispatch_callback(std::uint64_t ticket,
    LuaMadeSimple::HostAuxiliaryInvocation* invocation) noexcept {
    if (!invocation || !invocation->actual) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        VmHandle bound_vm{};ModuleHandle module{};Kind kind{};
        {
            std::lock_guard lock{Mutex};const auto found=Records.find(ticket);
            if(found==Records.end()) return UE4SS_LUA_HOST_INVALID_HANDLE;
            bound_vm=found->second.vm;module=found->second.module;kind=found->second.kind;
        }
        const auto root=LuaMadeSimple::cached_root_lua_state(invocation->actual);
        if(!root || !LuaMadeSimple::has_host_execution_scope(root)) return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto& host=HostBindings::instance();const auto vm=host.find_vm(root);
        if(vm.status!=Status::ok) return static_cast<std::uint32_t>(vm.status);
        if(bound_vm.id && bound_vm!=vm.value) return UE4SS_LUA_HOST_WRONG_OWNER;
        if(kind!=Kind::error && (!invocation->lua || invocation->lua->get_root_lua_state()!=root)) return UE4SS_LUA_HOST_WRONG_OWNER;
        if(kind==Kind::debug && !invocation->debug) return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(kind==Kind::error && (!invocation->message || !invocation->traceback)) return UE4SS_LUA_HOST_INVALID_HANDLE;
        const auto stack=host.stack_binding(root,invocation->actual);
        if(stack.status!=Status::ok) return static_cast<std::uint32_t>(stack.status);
        return host.api().dispatch_lua(vm.value,module,stack.value,[&](auto) {
            const void* function{};
            {
                std::lock_guard lock{Mutex};const auto found=Records.find(ticket);
                if(found==Records.end()) return ue4ss_lua_host_status{UE4SS_LUA_HOST_INVALID_HANDLE};
                function=found->second.function; // real owner/VM pins + gate acquired
            }
            struct Request {const void* function;Kind kind;LuaMadeSimple::HostAuxiliaryInvocation* arguments;bool failed;};
            Request request{function,kind,invocation,false};
            const auto bridge=+[](void* opaque) noexcept {
                auto* r=static_cast<Request*>(opaque);
                try {
                    if(r->kind==Kind::post)
                        reinterpret_cast<void (*)(const LuaMadeSimple::Lua&)>(const_cast<void*>(r->function))(*r->arguments->lua);
                    else if(r->kind==Kind::error)
                        reinterpret_cast<void (*)(lua_State*,const std::string&,const std::string&)>(const_cast<void*>(r->function))(r->arguments->actual,*r->arguments->message,*r->arguments->traceback);
                    else if(r->kind==Kind::debug)
                        reinterpret_cast<void (*)(lua_State*,lua_Debug*)>(const_cast<void*>(r->function))(r->arguments->actual,r->arguments->debug);
                    else r->arguments->results=reinterpret_cast<int (*)(lua_State*)>(const_cast<void*>(r->function))(r->arguments->actual);
                } catch (...) {r->failed=true;}
            };
            if(uikit_lua_native_run(invocation->actual,&request,bridge)!=LUA_OK || request.failed)
                return ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
            return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
        });
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
}
bool install_lms_auxiliary_bindings() noexcept {
    return LuaMadeSimple::install_host_auxiliary_hooks({register_callback,dispatch_callback,dispatch_host_auxiliary});
}
void revoke_lms_auxiliary_module(ModuleHandle module) noexcept {
    try {std::lock_guard lock{Mutex};std::erase_if(Records,[&](const auto& row){return row.second.module==module;});}
    catch (...) {std::terminate();}
}
void revoke_lms_auxiliary_vm(VmHandle vm) noexcept {
    try {std::lock_guard lock{Mutex};std::erase_if(Records,[&](const auto& row){return row.second.vm==vm;});}
    catch (...) {std::terminate();}
}
}
