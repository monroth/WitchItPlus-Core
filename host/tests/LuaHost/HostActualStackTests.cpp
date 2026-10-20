#include <LuaHost/HostApiRuntime.hpp>
#include <LuaHost/LuaReferences.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
extern "C" int uikit_lua_entry_matches_root(lua_State*,lua_State*);
#include <unordered_map>
#include <memory>
#include <lua.hpp>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace RC::LuaHost;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << '\n'; std::abort(); } } while (false)
static const auto& api = HostApiRuntime::table();
struct Fixture;
static std::unordered_map<lua_State*,Fixture*> raw_vms;
struct Fixture {
    Lifecycle life;
    HostApiRuntime runtime{life};
    VmHandle vm = life.create_vm().value;
    ModuleHandle module = life.create_module().value;
    lua_State* root = luaL_newstate();
    lua_State* thread{};
    lua_State* foreign = luaL_newstate();
    std::unique_ptr<LuaReferences> references;
    ue4ss_lua_context_handle outer{};
    std::function<void(ue4ss_lua_context_handle)> body;
    Fixture() {
        CHECK(root && foreign);
        CHECK(life.publish_vm(vm) == Status::ok);
        raw_vms.emplace(root,this);
        CHECK(life.publish_module(module) == Status::ok);
        lua_pushliteral(root, "root must stay unchanged");
        references = std::make_unique<LuaReferences>(life, vm, root);
        thread = lua_newthread(root); // rooted by actual shared VM's stack
    }
    ~Fixture() {
        CHECK(life.request_module_retire(module) == Status::ready);
        CHECK(life.claim_module_finalization(module) == Status::ready);
        CHECK(runtime.cleanup_module(module) == Status::ok);
        CHECK(life.finish_module_finalization(module) == Status::ok);
        CHECK(life.request_vm_close(vm) == Status::ready);
        CHECK(life.claim_vm_finalization(vm) == Status::ready);
        CHECK(runtime.cleanup_vm(vm) == Status::ok);
        raw_vms.erase(root);
        lua_close(root);
        CHECK(life.finish_vm_finalization(vm) == Status::ok);
        CHECK(life.vm_state(vm).status==Status::stale_generation);
        CHECK(life.validate_closed_vm(vm)==Status::ok);
        CHECK(life.validate_closed_vm({vm.id,vm.generation+1})==Status::stale_generation);
        lua_close(foreign);
    }
};
// This suite intentionally uses raw Lua fixtures, not LMS wrappers. Its trusted
// execution policy still acquires the SAME lifecycle VM lease + bundled gate.
// The production LMS/Windows gate uses install_lua_host_integration instead.
struct RawExecution { VmExecutionLease lease; explicit RawExecution(VmExecutionLease value):lease(std::move(value)){} ~RawExecution(){LuaUnlock(nullptr);} };
static std::uint32_t raw_enter(lua_State* actual,lua_State* root,void** out) noexcept {
    *out=nullptr;
    try {
        auto it=raw_vms.find(root);if(it==raw_vms.end())return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto admitted=it->second->life.enter_vm_execution(it->second->vm);
        if(admitted.status!=Status::ok)return static_cast<std::uint32_t>(admitted.status);
        auto ticket=std::make_unique<RawExecution>(std::move(admitted.value));
        LuaLock(actual);
        if(!uikit_lua_entry_matches_root(actual,root))return UE4SS_LUA_HOST_WRONG_OWNER;
        *out=ticket.release();return UE4SS_LUA_HOST_OK;
    } catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
static void raw_leave(void* ticket) noexcept {delete static_cast<RawExecution*>(ticket);}
static Fixture* active;
static int entry(lua_State* L) {
    LuaStackBinding stack{active->root, L, entry, 3, 1, active->references.get()};
    CHECK(active->runtime.dispatch_lua(active->vm, active->module, stack, [&](auto context) {
        active->outer = context;
        active->body(context);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    }) == UE4SS_LUA_HOST_OK);
    return lua_gettop(L);
}
static ue4ss_lua_value_v1 value(unsigned kind = UE4SS_LUA_VALUE_NIL) {
    ue4ss_lua_value_v1 v{};
    v.abi_version = 1; v.struct_size = sizeof(v); v.kind = kind;
    return v;
}
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL nested(ue4ss_lua_context_handle context) {
    CHECK(context.id != active->outer.id);
    int32_t top{};
    CHECK(api.stack_top(context, &top) == UE4SS_LUA_HOST_OK);
    CHECK(top == 1);
    auto v = value();
    CHECK(api.value_read(context, 1, &v) == UE4SS_LUA_HOST_OK);
    CHECK(v.kind == UE4SS_LUA_VALUE_INTEGER && v.integer_value == 42);
    CHECK(api.upvalue_read(context, 1, &v) == UE4SS_LUA_HOST_OK);
    CHECK(v.kind == UE4SS_LUA_VALUE_STRING && v.string_size == 8);
    CHECK(std::memcmp(v.string_data, "consumer", 8) == 0);
    return UE4SS_LUA_HOST_OK;
}
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL closure_body(ue4ss_lua_context_handle context) {
    auto v=value();
    CHECK(api.upvalue_read(context,1,&v)==UE4SS_LUA_HOST_OK);
    CHECK(v.kind==UE4SS_LUA_VALUE_INTEGER && v.integer_value==80);
    const auto add=v.integer_value;
    CHECK(api.upvalue_read(context,2,&v)==UE4SS_LUA_HOST_OK);
    CHECK(v.kind==UE4SS_LUA_VALUE_STRING && v.string_size==3 && std::memcmp(v.string_data,"u\0x",3)==0);
    CHECK(api.upvalue_read(context,3,&v)==UE4SS_LUA_HOST_INVALID_HANDLE);
    CHECK(api.value_read(context,1,&v)==UE4SS_LUA_HOST_OK && v.kind==UE4SS_LUA_VALUE_INTEGER);
    v.integer_value+=add;
    CHECK(api.value_push(context,&v)==UE4SS_LUA_HOST_OK);
    CHECK(api.callback_return(context,999)==UE4SS_LUA_HOST_INVALID_HANDLE);
    return api.callback_return(context,1);
}
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL closure_throws(ue4ss_lua_context_handle) {
    throw std::runtime_error("ordinary callback error");
}
static int real_nested_frame(lua_State* L) {
    int32_t untouched = 991;
    CHECK(api.stack_top(active->outer, &untouched) == UE4SS_LUA_HOST_WRONG_OWNER);
    CHECK(untouched == 991);
    LuaStackBinding stack{active->root, L, nullptr, 1, 0, active->references.get()};
    CHECK(active->runtime.dispatch_lua(active->vm, active->module, stack, [&](auto context) {
        int32_t top{};
        CHECK(api.stack_top(context, &top) == UE4SS_LUA_HOST_OK && top == 2);
        auto v = value(UE4SS_LUA_VALUE_INTEGER); v.integer_value = 7;
        CHECK(api.value_push(context, &v) == UE4SS_LUA_HOST_OK);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    }) == UE4SS_LUA_HOST_OK);
    return 1;
}
int main() {
    CHECK(RC::LuaMadeSimple::install_host_execution_hooks({raw_enter,raw_enter,raw_leave,nullptr}));
    CHECK(api.capabilities == 0); // incomplete tail never advertised
    CHECK(sizeof(api) == 168 && sizeof(ue4ss_lua_value_v1) == 80);
    CHECK(api.stack_top({}, nullptr) == UE4SS_LUA_HOST_INVALID_HANDLE);
    Fixture fixture; active = &fixture;
    CHECK(fixture.runtime.dispatch_host(fixture.vm, fixture.module, [&](auto context) {
        int32_t untouched = 123;
        CHECK(api.stack_top(context, &untouched) == UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(untouched == 123);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    }) == UE4SS_LUA_HOST_OK);
    bool called{};
    LuaStackBinding wrong{fixture.root, fixture.foreign};
    CHECK(fixture.runtime.dispatch_lua(fixture.vm, fixture.module, wrong, [&](auto) {
        called = true; return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    }) == UE4SS_LUA_HOST_WRONG_OWNER);
    CHECK(!called && lua_gettop(fixture.foreign) == 0);
    fixture.body = [&](auto context) {
        int32_t top{};
        CHECK(api.stack_top(context, &top) == UE4SS_LUA_HOST_OK && top == 1);
        int32_t untouched = 123;
        CHECK(api.stack_top({context.id, context.generation + 1}, &untouched) == UE4SS_LUA_HOST_STALE_GENERATION);
        CHECK(untouched == 123);
        CHECK(api.stack_top({context.id + 1, context.generation}, &untouched) == UE4SS_LUA_HOST_INVALID_HANDLE);
        auto v = value();
        CHECK(api.value_read(context, -1, &v) == UE4SS_LUA_HOST_OK);
        CHECK(v.kind == UE4SS_LUA_VALUE_INTEGER && v.integer_value == 42);
        CHECK(api.value_read(context, 0, &v) == UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(api.upvalue_read(context, 1, &v) == UE4SS_LUA_HOST_OK);
        CHECK(v.kind == UE4SS_LUA_VALUE_STRING && v.string_size == 8);
        CHECK(std::memcmp(v.string_data, "consumer", 8) == 0);
        CHECK(api.upvalue_read(context, 2, &v) == UE4SS_LUA_HOST_INVALID_HANDLE);
        ue4ss_lua_callback_handle callback{};
        CHECK(api.register_callback(nested, &callback) == UE4SS_LUA_HOST_OK);
        CHECK(api.invoke({fixture.vm.id, fixture.vm.generation}, callback) == UE4SS_LUA_HOST_OK);
        CHECK(api.stack_top(context, &top) == UE4SS_LUA_HOST_OK && top == 1);
        v = value(UE4SS_LUA_VALUE_TABLE);
        CHECK(api.value_push(context, &v) == UE4SS_LUA_HOST_OK);
        v = value(UE4SS_LUA_VALUE_STRING); v.string_data = "b\0y"; v.string_size = 3;
        CHECK(api.value_push(context, &v) == UE4SS_LUA_HOST_OK);
        CHECK(api.table_set_field(context, -2, "k\0x", 3) == UE4SS_LUA_HOST_OK);
        CHECK(api.stack_top(context, &top) == UE4SS_LUA_HOST_OK && top == 2);
        // Actual result was written to the coroutine table, never the VM root.
        lua_pushlstring(fixture.thread, "k\0x", 3); lua_rawget(fixture.thread, 2);
        size_t size{}; const char* bytes = lua_tolstring(fixture.thread, -1, &size);
        CHECK(size == 3 && std::memcmp(bytes, "b\0y", 3) == 0); lua_pop(fixture.thread, 1);
        CHECK(api.stack_set_top(context, 1) == UE4SS_LUA_HOST_OK);
        lua_pushcfunction(fixture.thread, real_nested_frame);
        v = value(UE4SS_LUA_VALUE_INTEGER); v.integer_value = 1;
        CHECK(api.value_push(context, &v) == UE4SS_LUA_HOST_OK);
        v.integer_value = 2;
        CHECK(api.value_push(context, &v) == UE4SS_LUA_HOST_OK);
        CHECK(api.protected_call(context, 2, 1) == UE4SS_LUA_HOST_OK);
        CHECK(api.value_read(context, -1, &v) == UE4SS_LUA_HOST_OK);
        CHECK(v.kind == UE4SS_LUA_VALUE_INTEGER && v.integer_value == 7);
        CHECK(api.stack_set_top(context, 1) == UE4SS_LUA_HOST_OK);
        ue4ss_lua_reference_handle reference{};
        CHECK(api.reference_create(context, 1, &reference) == UE4SS_LUA_HOST_OK);
        CHECK(reference.id != 0 && fixture.references->size() == 1);
        CHECK(api.stack_top(context, &top) == UE4SS_LUA_HOST_OK && top == 1);
        CHECK(api.stack_set_top(context, 0) == UE4SS_LUA_HOST_OK);
        CHECK(api.reference_push(context, reference) == UE4SS_LUA_HOST_OK);
        CHECK(api.value_read(context, 1, &v) == UE4SS_LUA_HOST_OK);
        CHECK(v.kind == UE4SS_LUA_VALUE_INTEGER && v.integer_value == 42);
        CHECK(api.reference_push(context, {reference.id, reference.generation + 1}) == UE4SS_LUA_HOST_STALE_GENERATION);
        CHECK(api.reference_release(context, reference) == UE4SS_LUA_HOST_OK);
        CHECK(fixture.references->size() == 0);
        CHECK(api.reference_push(context, reference) == UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(api.callback_return(context,0)==UE4SS_LUA_HOST_INVALID_HANDLE);
        ue4ss_lua_callback_handle closure{};
        CHECK(api.register_callback(closure_body,&closure)==UE4SS_LUA_HOST_OK);
        v=value(UE4SS_LUA_VALUE_INTEGER);v.integer_value=80;
        CHECK(api.value_push(context,&v)==UE4SS_LUA_HOST_OK);
        v=value(UE4SS_LUA_VALUE_STRING);v.string_data="u\0x";v.string_size=3;
        CHECK(api.value_push(context,&v)==UE4SS_LUA_HOST_OK);
        CHECK(api.callback_push(context,closure,2)==UE4SS_LUA_HOST_OK);
        CHECK(api.stack_top(context,&top)==UE4SS_LUA_HOST_OK && top==2);
        v=value(UE4SS_LUA_VALUE_INTEGER);v.integer_value=21;
        CHECK(api.value_push(context,&v)==UE4SS_LUA_HOST_OK);
        CHECK(api.protected_call(context,1,1)==UE4SS_LUA_HOST_OK);
        CHECK(api.value_read(context,-1,&v)==UE4SS_LUA_HOST_OK && v.integer_value==101);
        CHECK(api.stack_set_top(context,1)==UE4SS_LUA_HOST_OK);
        CHECK(api.callback_push(context,{closure.id,closure.generation+1},0)==UE4SS_LUA_HOST_STALE_GENERATION);
        CHECK(api.register_callback(closure_throws,&closure)==UE4SS_LUA_HOST_OK);
        CHECK(api.callback_push(context,closure,0)==UE4SS_LUA_HOST_OK);
        CHECK(api.protected_call(context,0,1)==UE4SS_LUA_HOST_INTERNAL_ERROR);
        CHECK(api.stack_top(context,&top)==UE4SS_LUA_HOST_OK && top==1);
        CHECK(api.request_module_retire() == UE4SS_LUA_HOST_PENDING);
        CHECK(api.stack_top(context, &top) == UE4SS_LUA_HOST_RETIRED);
    };
    lua_pushinteger(fixture.thread, 111); lua_pushinteger(fixture.thread, 222);
    lua_pushliteral(fixture.thread, "consumer"); lua_pushcclosure(fixture.thread, entry, 3);
    lua_pushinteger(fixture.thread, 42);
    CHECK(lua_pcall(fixture.thread, 1, 1, 0) == LUA_OK);
    CHECK(lua_gettop(fixture.thread) == 1 && lua_tointeger(fixture.thread, 1) == 42);
    CHECK(lua_gettop(fixture.root) == 2);
    CHECK(std::strcmp(lua_tostring(fixture.root, 1), "root must stay unchanged") == 0);
    int32_t untouched = 321;
    CHECK(api.stack_top(fixture.outer, &untouched) == UE4SS_LUA_HOST_INVALID_HANDLE && untouched == 321);
    std::cout << checks << " host actual-stack/API/nested-frame assertions PASS\n";
}
