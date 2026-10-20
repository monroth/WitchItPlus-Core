#include <LuaHost/HostBindings.hpp>
#include <LuaHost/EventBatch.hpp>
#include <lua.hpp>
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

using namespace RC::LuaHost;
static unsigned checks{};
#define CHECK(c) do { ++checks; if (!(c)) { std::cerr<<"FAIL "<<__LINE__<<": "<<#c<<'\n';std::abort(); } } while(false)
// Linux isolated test gate. Windows bundle substitutes the actual production
// LuaRaw gate; no second Lua runtime is embedded in either executable.
#if !defined(_WIN32)
static std::recursive_mutex gate;
extern "C" void LuaLock(lua_State*) { gate.lock(); }
extern "C" void LuaUnlock(lua_State*) { gate.unlock(); }
#endif
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL noop(ue4ss_lua_context_handle) { return UE4SS_LUA_HOST_OK; }
static ue4ss_lua_host_context_info_v1 info(const ue4ss_uikit_lua_host_api_v1& api) {
    ue4ss_lua_host_context_info_v1 value{1,sizeof(value),{},{},{}};
    CHECK(api.current_context(&value)==UE4SS_LUA_HOST_OK);return value;
}
static void finish_module(HostBindings& host,ModuleHandle module) {
    CHECK(host.claim_module_retire(module)==Status::ready);
    CHECK(host.cleanup_module(module)==Status::ok);
    CHECK(host.finish_module_retire(module)==Status::ok);
}
static void finish_vm(HostBindings& host,VmHandle vm,lua_State* state) {
    CHECK(host.claim_vm_close(vm)==Status::ready);
    CHECK(host.cleanup_vm(vm)==Status::ok);
    lua_close(state);
    CHECK(host.finish_vm_close(vm)==Status::ok);
}
int main() {
    const ue4ss_uikit_lua_host_api_v1* api=nullptr;
    CHECK(UE4SS_GetUIKitLuaHostApi_v1(1,sizeof(*api),nullptr)==UE4SS_LUA_HOST_INVALID_HANDLE);
    CHECK(UE4SS_GetUIKitLuaHostApi_v1(2,sizeof(*api),&api)==UE4SS_LUA_HOST_UNSUPPORTED_ABI);CHECK(!api);
    CHECK(UE4SS_GetUIKitLuaHostApi_v1(1,7,&api)==UE4SS_LUA_HOST_UNSUPPORTED_SIZE);CHECK(!api);
    CHECK(UE4SS_GetUIKitLuaHostApi_v1(1,sizeof(*api)+1,&api)==UE4SS_LUA_HOST_UNSUPPORTED_SIZE);CHECK(!api);
    CHECK(UE4SS_GetUIKitLuaHostApi_v1(1,8,&api)==UE4SS_LUA_HOST_OK);CHECK(api);
    CHECK(api==&HostApiRuntime::table());
    ue4ss_lua_host_context_info_v1 none{1,sizeof(none),{},{},{}};
    CHECK(api->current_context(&none)==UE4SS_LUA_HOST_INVALID_HANDLE);
    {
        HostBindings host;
        auto* first=luaL_newstate();auto* second=luaL_newstate();CHECK(first&&second);
        CHECK(host.attach_vm(nullptr).status==Status::invalid_handle);
        auto av=host.attach_vm(first),bv=host.attach_vm(second);CHECK(av.status==Status::ok);CHECK(bv.status==Status::ok);
        CHECK(host.attach_vm(first).status==Status::busy);CHECK(host.vm_count()==2);
        CHECK(host.find_vm(first).value==av.value);CHECK(host.find_vm(second).value==bv.value);
        auto a=host.attach_module(),b=host.attach_module();CHECK(a.status==Status::ok);CHECK(b.status==Status::ok);
        ue4ss_lua_callback_handle h{};
        CHECK(host.api().dispatch_host(av.value,a.value,[&](auto context) {
            auto active=info(*api);CHECK(active.context.id==context.id);CHECK(active.vm.id==av.value.id);CHECK(active.module.id==a.value.id);
            CHECK(api->register_callback(noop,&h)==UE4SS_LUA_HOST_OK);
            CHECK(host.api().dispatch_code(b.value,[&] {
                CHECK(api->current_context(&none)==UE4SS_LUA_HOST_INVALID_HANDLE);
                CHECK(api->invoke(active.vm,h)==UE4SS_LUA_HOST_INVALID_HANDLE);
            })==Status::ok);
            CHECK(info(*api).context.id==context.id);
            try { host.api().dispatch_code(b.value,[] { throw 3; }); } catch(int) {}
            CHECK(info(*api).context.id==context.id);
            CHECK(host.api().dispatch_host(bv.value,b.value,[&](auto) {
                CHECK(info(*api).module.id==b.value.id);
                CHECK(api->invoke({av.value.id,av.value.generation},h)==UE4SS_LUA_HOST_WRONG_OWNER);
                return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
            })==UE4SS_LUA_HOST_OK);
            CHECK(info(*api).context.id==context.id);
            return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
        })==UE4SS_LUA_HOST_OK);
        CHECK(api->current_context(&none)==UE4SS_LUA_HOST_INVALID_HANDLE);
        finish_module(host,a.value);CHECK(host.api().callback_count()==0);
        // Retiring a C++ module does not close either actual Lua root.
        CHECK(host.vm_count()==2);lua_pushinteger(second,37);CHECK(lua_tointeger(second,-1)==37);lua_pop(second,1);
        finish_vm(host,av.value,first);CHECK(host.find_vm(first).status==Status::invalid_handle);CHECK(host.vm_count()==1);
        finish_module(host,b.value);finish_vm(host,bv.value,second);CHECK(host.vm_count()==0);
    }
    {
        HostBindings host;auto* state=luaL_newstate();CHECK(state);
        auto vm=host.attach_vm(state);auto module=host.attach_module();CHECK(vm.status==Status::ok);CHECK(module.status==Status::ok);
        std::promise<void> entered, release;
        auto entered_future=entered.get_future(),release_future=release.get_future();
        std::atomic<unsigned> retired_status{};
        std::thread worker([&] {
            host.api().dispatch_host(vm.value,module.value,[&](auto) {
                entered.set_value();release_future.wait();
                ue4ss_lua_host_context_info_v1 value{1,sizeof(value),{},{},{}};
                retired_status=api->current_context(&value);
                return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
            });
        });
        entered_future.wait();CHECK(host.claim_vm_close(vm.value)==Status::pending);
        CHECK(host.vm_is_closing(vm.value));CHECK(lua_gettop(state)==0);CHECK(host.vm_count()==1);
        CHECK(host.claim_module_retire(module.value)==Status::pending);CHECK(host.module_is_retiring(module.value));
        release.set_value();worker.join();CHECK(retired_status==UE4SS_LUA_HOST_RETIRED);
        finish_vm(host,vm.value,state);finish_module(host,module.value);
    }
    {
        HostBindings host;auto module=host.attach_module();CHECK(module.status==Status::ok);
        std::vector<HostEvent> events;std::mutex mutex;unsigned executed{};
        struct Probe {
            Lifecycle& lifecycle;HostApiRuntime& runtime;ModuleHandle module;std::mutex& mutex;unsigned& destroyed;
            ~Probe() {
                // The consumer function manager is destroyed before its code pin.
                CHECK(lifecycle.claim_module_finalization(module)==Status::pending);
                CHECK(mutex.try_lock());mutex.unlock();
                auto descendant=runtime.wrap_event([]{});
                CHECK(descendant.status==Status::retired);
                ++destroyed;
            }
        };
        unsigned destroyed{};
        CHECK(host.api().dispatch_code(module.value,[&] {
            auto probe=std::shared_ptr<Probe>(new Probe{host.lifecycle(),host.api(),module.value,mutex,destroyed});
            auto wrapped=host.api().wrap_event([&,probe] { ++executed; });CHECK(wrapped.status==Status::ok);
            std::lock_guard lock{mutex};events.push_back(std::move(wrapped.value));
        })==Status::ok);
        CHECK(host.claim_module_retire(module.value)==Status::pending);
        {
            auto batch=take_event_batch(events,mutex,5);CHECK(events.empty());CHECK(batch.size()==1);
            batch[0]();CHECK(executed==0);CHECK(destroyed==0);
            CHECK(host.claim_module_retire(module.value)==Status::pending);
        }
        CHECK(destroyed==1);finish_module(host,module.value);
    }
    {
        HostBindings host;auto module=host.attach_module();CHECK(module.status==Status::ok);
        CHECK(host.claim_module_retire(module.value)==Status::ready);
        unsigned payloads{};
        CHECK(host.api().dispatch_finalizer(module.value,[&] {
            CHECK(api->current_context(&none)==UE4SS_LUA_HOST_INVALID_HANDLE);
            auto result=host.api().wrap_event([&] { ++payloads; });
            CHECK(result.status==Status::retired);CHECK(!result.value);
            CHECK(host.lifecycle().claim_module_finalization(module.value)==Status::busy);
        })==Status::ok);
        CHECK(payloads==0);
        CHECK(host.cleanup_module(module.value)==Status::ok);
        CHECK(host.finish_module_retire(module.value)==Status::ok);
    }
    {
        std::vector<HostEvent> events;std::mutex mutex;unsigned executed{};
        events.emplace_back([&] { CHECK(mutex.try_lock());mutex.unlock();++executed;
            std::lock_guard lock{mutex};events.emplace_back([&] {++executed;}); });
        events.emplace_back([&] {++executed;});
        auto batch=take_event_batch(events,mutex,5);CHECK(batch.size()==2);
        for(auto& event:batch) { event(); }CHECK(executed==2);CHECK(events.size()==1);
        batch=take_event_batch(events,mutex,5);for(auto& event:batch) { event(); }CHECK(executed==3);CHECK(events.empty());
        events.emplace_back([&] { ++executed; });auto detached=detach_events(events,mutex);CHECK(events.empty());CHECK(detached.size()==1);
        CHECK(mutex.try_lock());mutex.unlock();detached.clear();CHECK(executed==3);
    }
    std::cout<<checks<<" real Lua root / host accessor / queued-code-lifetime assertions PASS\n";
}
