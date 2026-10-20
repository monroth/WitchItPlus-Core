#include <LuaHost/HostIntegration.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaHost/LuaExecutionBindings.hpp>
#include <LuaHost/LmsAuxiliaryBindings.hpp>
#include <LuaHost/OwnedUserdataBindings.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaMadeSimple/HostFunctionHooks.hpp>
#if defined(_WIN32)
# include <Windows.h>
#else
# include <dlfcn.h>
#endif
namespace RC::LuaHost {
void* code_image(const void* address) noexcept {
    if (!address) return nullptr;
#if defined(_WIN32)
    HMODULE image{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &image)) return nullptr;
    return image;
#else
    Dl_info image{};
    return dladdr(address, &image) ? image.dli_fbase : nullptr;
#endif
}
void* host_code_image() noexcept {
    return code_image(reinterpret_cast<const void*>(&install_lua_host_integration));
}
namespace {
ue4ss_lua_host_status callback_dispatch(VmHandle vm, ModuleHandle module,
    const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>& function) {
    return HostBindings::instance().dispatch_callback_lua(vm, module, function);
}
Status stack_origin(VmHandle expected,const LuaStackBinding& stack) noexcept {
    try {
        const auto vm=HostBindings::instance().find_vm(stack.root);
        if(vm.status!=Status::ok) return vm.status;
        return vm.value==expected?Status::ok:Status::wrong_owner;
    } catch (...) {return Status::internal_error;}
}
Status callback_origin(ModuleHandle owner, const void* function) noexcept {
    try {
        const auto image = code_image(function);
        if (!image) return Status::invalid_handle;
        const auto actual = HostBindings::instance().module_for_image(image);
        if (actual.status != Status::ok) return actual.status;
        return actual.value == owner ? Status::ok : Status::wrong_owner;
    } catch (...) { return Status::internal_error; }
}
std::uint32_t register_function(const LuaMadeSimple::Lua& lua,
    int (*function)(const LuaMadeSimple::Lua&), std::uint64_t* output) {
    if (!function || !output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = 0;
    try {
        const auto image = code_image(reinterpret_cast<const void*>(function));
        if (!image) return UE4SS_LUA_HOST_INVALID_HANDLE;
        if (image == host_code_image()) return UE4SS_LUA_HOST_OK;
        auto& host = HostBindings::instance();
        const auto module = host.module_for_image(image);
        if (module.status != Status::ok) return static_cast<std::uint32_t>(module.status);
        const auto owner = host.api().code_owner();
        if (owner.status != Status::ok) return static_cast<std::uint32_t>(owner.status);
        if (owner.value != module.value) return UE4SS_LUA_HOST_WRONG_OWNER;
        const auto vm = host.find_vm(lua.get_root_lua_state());
        if (vm.status != Status::ok) return static_cast<std::uint32_t>(vm.status);
        const auto ticket = host.api().register_lms_function(vm.value, function);
        if (ticket.status == Status::ok) *output = ticket.value;
        return static_cast<std::uint32_t>(ticket.status);
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
std::uint32_t dispatch_host_function(const LuaMadeSimple::Lua& lua,int (*function)(const LuaMadeSimple::Lua&),int* output) {
    if(!function || !output || code_image(reinterpret_cast<const void*>(function))!=host_code_image()) return UE4SS_LUA_HOST_WRONG_OWNER;
    try {
        auto& host=HostBindings::instance();
        const auto vm=host.find_vm(lua.get_root_lua_state());
        if(vm.status!=Status::ok) return static_cast<std::uint32_t>(vm.status);
        struct Request {const LuaMadeSimple::Lua* lua;int (*function)(const LuaMadeSimple::Lua&);int* output;};
        Request request{&lua,function,output};
        return static_cast<std::uint32_t>(host.dispatch_host_callback_lua_on_stack(vm.value,lua.get_lua_state(),&request,
            +[](void* opaque){auto* r=static_cast<Request*>(opaque);*r->output=r->function(*r->lua);}));
    } catch (...) {return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
std::uint32_t dispatch_function(std::uint64_t ticket, const LuaMadeSimple::Lua& lua, int* output) {
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    *output = 0;
    try {
        auto& host = HostBindings::instance();
        const auto stack = host.stack_binding(lua.get_root_lua_state(), lua.get_lua_state(),
            LuaMadeSimple::process_lua_function, 3, 0); // two SDK-private upvalues
        if (stack.status != Status::ok) return static_cast<std::uint32_t>(stack.status);
        const auto result = host.api().dispatch_lms_function(ticket, lua, stack.value);
        if (result.status == Status::ok) *output = result.value;
        return static_cast<std::uint32_t>(result.status);
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
}
bool install_lua_host_integration() noexcept {
    const bool ready=install_lua_execution_lifetime() && install_lms_auxiliary_bindings() && install_owned_userdata_bindings() &&
        HostBindings::instance().api().install_callback_origin(callback_origin) &&
        HostBindings::instance().api().install_stack_origin(stack_origin) &&
        HostBindings::instance().api().install_callback_dispatch(callback_dispatch) &&
        LuaMadeSimple::install_host_function_hooks({register_function, dispatch_function,dispatch_host_function});
    return ready && HostBindings::instance().api().finish_scalar_bootstrap();
}
}
