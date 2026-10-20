#pragma once
#include <LuaHost/HostBindings.hpp>
#include <LuaHost/ModLuaEntry.hpp>
#include <mutex>
#include <utility>
namespace RC::LuaHost {
// Capture while the callback container is locked. This takes ONLY lifetime/cache
// bookkeeping locks, never the Lua gate. The pin keeps the wrapper, coroutine and
// registry alive until dispatch/cancellation completes on this same thread.
class DetachedLuaWork {
    VmExecutionLease pin_;
    lua_State* actual_{};
public:
    explicit DetachedLuaWork(const LuaMadeSimple::Lua* lua) {
        if (!lua) return;
        actual_=lua->get_lua_state();
        auto& host=HostBindings::instance();
        auto vm=host.find_vm(LuaMadeSimple::cached_root_lua_state(actual_));
        if(vm.status!=Status::ok) return;
        auto pin=host.lifecycle().enter_vm_execution(vm.value);
        if(pin.status==Status::ok) pin_=std::move(pin.value);
    }
    DetachedLuaWork(DetachedLuaWork&&)=default;
    DetachedLuaWork& operator=(DetachedLuaWork&&)=default;
    explicit operator bool() const noexcept {return bool(pin_);}
    bool cancelled() const {return !pin_ || HostBindings::instance().vm_is_closing(pin_.vm());}
    // Caller MUST release every container/control mutex before run(). A closing
    // owner cancels both execution and unref: its finalizer owns all references.
    // Keep the scope through callback error handling/stack restoration/cleanup.
    template<class F, class Error> bool run(F&& function, Error&& error) noexcept {
        try {
            if(cancelled()) return false;
            ModLuaEntry entry{actual_};
            try {std::forward<F>(function)();}
            catch(...) {std::forward<Error>(error)();}
            return true;
        } catch(...) {return false;}
    }
};
// Every shared flag transition uses its own short container critical section.
// Destruction never enters Lua, and restores the previous value on every exit.
class DispatchFlag {
    std::recursive_mutex& mutex_;
    bool& flag_;
    bool owned_{};
public:
    DispatchFlag(std::recursive_mutex& mutex, bool& flag):mutex_(mutex),flag_(flag) {
        std::lock_guard lock{mutex_};
        if(!flag_) {flag_=true;owned_=true;}
    }
    ~DispatchFlag() {if(owned_) {std::lock_guard lock{mutex_};flag_=false;}}
    explicit operator bool() const noexcept {return owned_;}
};
}
