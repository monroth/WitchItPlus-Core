#include <LuaMadeSimple/HostUserdataObserver.hpp>
#include <atomic>
namespace RC::LuaMadeSimple {
namespace {std::atomic<const HostUserdataObserver*> observer{};}
void set_host_userdata_observer(const HostUserdataObserver* value) noexcept {observer.store(value,std::memory_order_release);}
void notify_host_userdata_completed(const HostUserdataEmission& value) noexcept {
    const auto* active=observer.load(std::memory_order_acquire);
    if(active && active->completed)active->completed(value);
}
void notify_host_userdata_destroying(lua_State* stack,void* storage,const void* type) noexcept {
    const auto* active=observer.load(std::memory_order_acquire);
    if(active && active->destroying)active->destroying(stack,storage,type);
}
void notify_host_userdata_finalizer(lua_State* stack,const void* type,int (*function)(lua_State*)) noexcept {
    const auto* active=observer.load(std::memory_order_acquire);
    if(active && active->finalizer)active->finalizer(stack,type,function);
}
}
