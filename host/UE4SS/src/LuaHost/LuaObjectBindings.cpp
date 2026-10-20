#include <LuaHost/LuaObjectBindings.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaHost/LuaObjectAuthorityRuntime.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
namespace RC::LuaHost {
namespace {
lua_State* root_for_stack(lua_State* actual,void*) noexcept {
    if (auto* root=HostApiRuntime::active_root_for_stack(actual)) return root;
    return LuaMadeSimple::cached_root_lua_state(actual);
}
struct ObjectRuntime {
    LuaObjectAuthorityRuntime authority;
    LuaObjectBackend values;
    ObjectRuntime():authority(HostBindings::instance().lifecycle(),root_for_stack,nullptr),
        values(HostBindings::instance().api().value_backend(),authority) {}
};
ObjectRuntime& objects() { static ObjectRuntime runtime;return runtime; }
ue4ss_lua_host_status attach(VmHandle vm,lua_State* root) noexcept { return objects().authority.attach_vm(vm,root); }
ue4ss_lua_host_status detach(VmHandle vm,lua_State* root) noexcept { return objects().authority.detach_vm(vm,root); }
ue4ss_lua_host_status forget(VmHandle vm,lua_State* root) noexcept { return objects().authority.forget_closed_vm(vm,root); }
ue4ss_lua_host_status read(void* backend,const LuaValueContext& view,ue4ss_lua_context_handle c,std::int32_t i,void** out) noexcept {
    return static_cast<LuaObjectBackend*>(backend)->object_read(view,c,i,out);
}
ue4ss_lua_host_status push(void* backend,const LuaValueContext& view,ue4ss_lua_context_handle c,void* object) noexcept {
    return static_cast<LuaObjectBackend*>(backend)->object_push(view,c,object);
}
}
bool install_lua_object_bindings() noexcept {
    try {
        auto& runtime=objects();auto& host=HostBindings::instance();
        return runtime.authority.install_observer()==UE4SS_LUA_HOST_OK &&
               host.install_vm_observer({attach,detach,forget}) &&
               host.api().install_object_operations({&runtime.values,read,push});
    } catch (...) { return false; }
}
}
