#include <LuaMadeSimple/HostOwnedUserdataHooks.hpp>
#include <lua.hpp>
#include <atomic>
#include <stdexcept>
namespace RC::LuaMadeSimple {
namespace { HostOwnedUserdataHooks Hooks{}; std::atomic<bool> Installed{}; }
bool install_host_owned_userdata_hooks(const HostOwnedUserdataHooks& hooks) noexcept {
    if(Installed.load(std::memory_order_acquire)||!hooks.Execute||!hooks.Allocate||!hooks.Arm||!hooks.Phase||!hooks.Finalizer||!hooks.Readable)return false;
    Hooks=hooks;Installed.store(true,std::memory_order_release);return true;
}
std::uint32_t execute_host_owned_userdata(const HostOwnedUserdataConstruction& request,bool* handled) noexcept {
    if(!handled)return 1;*handled=false;
    return Installed.load(std::memory_order_acquire)?Hooks.Execute(&request,handled):0;
}
void* allocate_host_owned_userdata(lua_State* actual,const void* type,std::size_t size,int values,bool* owned) {
    *owned=false;
    if(!Installed.load(std::memory_order_acquire))return lua_newuserdatauv(actual,size,values);
    void* storage{};const auto status=Hooks.Allocate(actual,type,size,values,&storage,owned);
    if(status)throw std::runtime_error("Host refused owned userdata allocation");
    return storage?storage:lua_newuserdatauv(actual,size,values);
}
int (*publish_host_owned_userdata_finalizer(lua_State* actual,const void* type,int (*original)(lua_State*)))(lua_State*) {
    if(!Installed.load(std::memory_order_acquire))return original;
    int (*published)(lua_State*){};
    if(Hooks.Finalizer(actual,type,original,&published)||!published)
        throw std::runtime_error("Host refused Native automatic GC publication");
    return published;
}
bool host_owned_userdata_readable(lua_State* actual,void* storage,const void* type) noexcept {
    return !Installed.load(std::memory_order_acquire)||Hooks.Readable(actual,storage,type);
}
void arm_host_owned_userdata(lua_State* actual,void* storage) {
    if(Installed.load(std::memory_order_acquire)&&Hooks.Arm(actual,storage))
        throw std::runtime_error("Host refused owned userdata finalizer publication");
}
void phase_host_owned_userdata(lua_State* actual,void* storage,HostOwnedUserdataPhase phase) noexcept {
    if(Installed.load(std::memory_order_acquire))Hooks.Phase(actual,storage,phase);
}
}
