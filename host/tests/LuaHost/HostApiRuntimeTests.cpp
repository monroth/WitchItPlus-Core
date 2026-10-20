#include <LuaHost/HostApiRuntime.hpp>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <future>

using namespace RC::LuaHost;
static int checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { std::cerr << "FAIL " << __LINE__ << ": " << #condition << '\n'; std::abort(); } } while (false)
static const auto& api = HostApiRuntime::table();
static thread_local std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>* body;
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL entry(ue4ss_lua_context_handle c) { return (*body)(c); }
static ue4ss_lua_host_context_info_v1 info() {
    ue4ss_lua_host_context_info_v1 result{1,sizeof(result),{},{},{}};
    CHECK(api.current_context(&result) == UE4SS_LUA_HOST_OK);
    return result;
}
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL noop(ue4ss_lua_context_handle) { return UE4SS_LUA_HOST_OK; }
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL throws(ue4ss_lua_context_handle) { throw std::runtime_error("consumer error"); }
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL invalid_status(ue4ss_lua_context_handle) { return 999; }
static ue4ss_lua_context_handle parent_context;
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL nested(ue4ss_lua_context_handle context) {
    auto current = info();
    CHECK(current.context.id == context.id);
    CHECK(current.context.id != parent_context.id);
    for (int n=0;n<256;++n) {
        ue4ss_lua_callback_handle h{};
        CHECK(api.register_callback(noop, &h) == UE4SS_LUA_HOST_OK);
        CHECK(api.invoke(current.vm,h) == UE4SS_LUA_HOST_OK);
    }
    return UE4SS_LUA_HOST_OK;
}
struct Fixture {
    Lifecycle lifecycle;
    HostApiRuntime runtime{lifecycle};
    VmHandle vm{};
    ModuleHandle a{}, b{};
    Fixture() {
        auto v=lifecycle.create_vm();CHECK(v.status==Status::ok);vm=v.value;
        auto x=lifecycle.create_module();CHECK(x.status==Status::ok);a=x.value;
        auto y=lifecycle.create_module();CHECK(y.status==Status::ok);b=y.value;
        CHECK(lifecycle.publish_vm(vm)==Status::ok);
        CHECK(lifecycle.publish_module(a)==Status::ok);
        CHECK(lifecycle.publish_module(b)==Status::ok);
    }
    ue4ss_lua_host_status run(ModuleHandle module,
        std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)> function) {
        auto* previous=body;body=&function;
        auto result=runtime.dispatch(vm,module,entry);body=previous;return result;
    }
    void cleanup() {
        for(auto module:{a,b}) {
            auto status=lifecycle.request_module_retire(module);
            CHECK(status==Status::ready || status==Status::retired);
            CHECK(lifecycle.claim_module_finalization(module)==Status::ready);
            CHECK(runtime.cleanup_module(module)==Status::ok);
            CHECK(lifecycle.finish_module_finalization(module)==Status::ok);
        }
        CHECK(lifecycle.request_vm_close(vm)==Status::ready);
        CHECK(lifecycle.claim_vm_finalization(vm)==Status::ready);
        CHECK(runtime.cleanup_vm(vm)==Status::ok);
        CHECK(lifecycle.finish_vm_finalization(vm)==Status::ok);
    }
};
int main() {
    CHECK(&HostApiRuntime::table()==&api);
    CHECK(api.struct_size==sizeof(api));
    CHECK(api.current_context(nullptr)==UE4SS_LUA_HOST_INVALID_HANDLE);
    ue4ss_lua_host_context_info_v1 outside{1,sizeof(outside),{},{},{}};
    CHECK(api.current_context(&outside)==UE4SS_LUA_HOST_INVALID_HANDLE);
    ue4ss_lua_callback_handle callback{};
    CHECK(api.register_callback(noop,&callback)==UE4SS_LUA_HOST_INVALID_HANDLE);
    CHECK(api.invoke({}, {})==UE4SS_LUA_HOST_INVALID_HANDLE);
    CHECK(api.request_vm_close({})==UE4SS_LUA_HOST_INVALID_HANDLE);
    CHECK(api.request_module_retire()==UE4SS_LUA_HOST_INVALID_HANDLE);
    {
        Fixture f;
        CHECK(f.runtime.dispatch(f.vm,f.a,nullptr)==UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(f.run(f.a,[&](auto context) {
            auto current=info();CHECK(current.context.id==context.id);
            CHECK(current.vm.id==f.vm.id);CHECK(current.module.id==f.a.id);
            auto bad=current;bad.abi_version=2;
            CHECK(api.current_context(&bad)==UE4SS_LUA_HOST_UNSUPPORTED_ABI);
            CHECK(bad.context.id==current.context.id);
            bad=current;bad.struct_size=8;
            CHECK(api.current_context(&bad)==UE4SS_LUA_HOST_UNSUPPORTED_SIZE);
            struct Oversized { ue4ss_lua_host_context_info_v1 known; std::uint64_t sentinel; } large{current,0xabcdef};
            large.known.struct_size=sizeof(large);
            CHECK(api.current_context(&large.known)==UE4SS_LUA_HOST_OK);
            CHECK(large.sentinel==0xabcdef);
            CHECK(api.register_callback(nullptr,&callback)==UE4SS_LUA_HOST_INVALID_HANDLE);
            CHECK(api.register_callback(noop,nullptr)==UE4SS_LUA_HOST_INVALID_HANDLE);
            CHECK(api.register_callback(nested,&callback)==UE4SS_LUA_HOST_OK);
            CHECK(callback.id!=0);
            CHECK(api.invoke({current.vm.id+1,current.vm.generation},callback)==UE4SS_LUA_HOST_WRONG_OWNER);
            CHECK(api.invoke(current.vm,{callback.id,callback.generation+1})==UE4SS_LUA_HOST_STALE_GENERATION);
            CHECK(api.invoke(current.vm,{99999,1})==UE4SS_LUA_HOST_INVALID_HANDLE);
            parent_context=context;
            CHECK(api.invoke(current.vm,callback)==UE4SS_LUA_HOST_OK);
            CHECK(info().context.id==context.id);
            std::atomic<unsigned> other_status{};
            std::thread other([&] { ue4ss_lua_host_context_info_v1 data{1,sizeof(data),{},{},{}};
                other_status=api.current_context(&data); });other.join();
            CHECK(other_status==UE4SS_LUA_HOST_INVALID_HANDLE);
            ue4ss_lua_callback_handle exception{};
            CHECK(api.register_callback(throws,&exception)==UE4SS_LUA_HOST_OK);
            CHECK(api.invoke(current.vm,exception)==UE4SS_LUA_HOST_INTERNAL_ERROR);
            CHECK(info().context.id==context.id);
            CHECK(api.register_callback(invalid_status,&exception)==UE4SS_LUA_HOST_OK);
            CHECK(api.invoke(current.vm,exception)==UE4SS_LUA_HOST_INTERNAL_ERROR);
            CHECK(info().context.id==context.id);
            return UE4SS_LUA_HOST_OK;
        })==UE4SS_LUA_HOST_OK);
        CHECK(api.current_context(&outside)==UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(f.run(f.b,[&](auto) {
            auto current=info();
            CHECK(api.invoke(current.vm,callback)==UE4SS_LUA_HOST_WRONG_OWNER);
            return UE4SS_LUA_HOST_OK;
        })==UE4SS_LUA_HOST_OK);
        CHECK(f.runtime.dispatch(f.vm,f.a,throws)==UE4SS_LUA_HOST_INTERNAL_ERROR);
        CHECK(api.current_context(&outside)==UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(f.runtime.cleanup_module(f.a)==Status::retired);
        f.cleanup();CHECK(f.runtime.callback_count()==0);
    }
    {
        Fixture f;
        std::promise<void> entered, release;
        auto entered_future=entered.get_future();auto release_future=release.get_future();
        std::atomic<unsigned> post_retire{}, late_invoke{}, dispatch_result{};
        std::thread worker([&] {
            dispatch_result=f.run(f.a,[&](auto) {
                ue4ss_lua_callback_handle h{};
                CHECK(api.register_callback(noop,&h)==UE4SS_LUA_HOST_OK);
                auto current=info();entered.set_value();release_future.wait();
                ue4ss_lua_host_context_info_v1 data{1,sizeof(data),{},{},{}};
                post_retire=api.current_context(&data);
                late_invoke=api.invoke(current.vm,h);
                return UE4SS_LUA_HOST_OK;
            });
        });
        entered_future.wait();
        CHECK(f.lifecycle.request_module_retire(f.a)==Status::pending);
        CHECK(f.lifecycle.claim_module_finalization(f.a)==Status::pending);
        CHECK(f.runtime.cleanup_module(f.a)==Status::retired);
        release.set_value();worker.join();
        CHECK(dispatch_result==UE4SS_LUA_HOST_OK);
        CHECK(post_retire==UE4SS_LUA_HOST_RETIRED);
        CHECK(late_invoke==UE4SS_LUA_HOST_RETIRED);
        CHECK(f.runtime.dispatch(f.vm,f.a,noop)==UE4SS_LUA_HOST_RETIRED);
        f.cleanup();CHECK(f.runtime.callback_count()==0);
    }
    {
        Fixture f;
        CHECK(f.run(f.a,[&](auto) {
            auto current=info();
            CHECK(api.request_vm_close({f.vm.id+1,f.vm.generation})==UE4SS_LUA_HOST_WRONG_OWNER);
            CHECK(api.request_vm_close(current.vm)==UE4SS_LUA_HOST_PENDING);
            CHECK(f.lifecycle.claim_vm_finalization(f.vm)==Status::pending);
            CHECK(api.register_callback(noop,&callback)==UE4SS_LUA_HOST_RETIRED);
            return UE4SS_LUA_HOST_OK;
        })==UE4SS_LUA_HOST_OK);
        f.cleanup();
    }
    {
        Fixture f;
        CHECK(f.run(f.a,[&](auto) {
            CHECK(api.request_module_retire()==UE4SS_LUA_HOST_PENDING);
            CHECK(f.lifecycle.claim_module_finalization(f.a)==Status::pending);
            CHECK(api.register_callback(noop,&callback)==UE4SS_LUA_HOST_RETIRED);
            return UE4SS_LUA_HOST_OK;
        })==UE4SS_LUA_HOST_OK);
        f.cleanup();
    }
    std::cout<<checks<<" host API context/callback/retirement assertions PASS\n";
}
