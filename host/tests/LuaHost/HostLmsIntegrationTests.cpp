#include <LuaHost/HostBindings.hpp>
#include <LuaHost/HostIntegration.hpp>
#include <LuaHost/EngineCallbackBindings.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <Unreal/Hooks/Internal/CallbackOwnerBridge.hpp>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <Input/Handler.hpp>
// Observe real bundled gate calls through the linker's wrap facility. The
// delegated originals are the actual pthread implementation in luauser.c.
static thread_local int actual_gate_depth;
extern "C" void __real_LuaLock(lua_State*);
extern "C" void __real_LuaUnlock(lua_State*);
extern "C" void __wrap_LuaLock(lua_State* state) { __real_LuaLock(state); ++actual_gate_depth; }
extern "C" void __wrap_LuaUnlock(lua_State* state) { --actual_gate_depth; __real_LuaUnlock(state); }
using namespace RC;
using namespace RC::LuaHost;
using Slot = Unreal::Hook::Internal::TCallbackSlot<std::function<void()>>;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << '\n'; std::abort(); } } while (false)
static std::shared_ptr<Slot> slot;
static std::function<void()> queued_native;
extern "C" bool host_queue_native_cpp_event(std::function<void()> function) {
    auto result=HostBindings::instance().api().wrap_event(std::move(function));
    if(result.status!=Status::ok) return false;
    queued_native=std::move(result.value);return true;
}
extern "C" bool host_register_native_hook(void (*function)()) {
    // Actual public fixture boundary called by the separately loaded game-mod
    // library, using its real caller instruction/image. Generic broker is real.
    const auto origin = __builtin_extract_return_addr(__builtin_return_address(0));
    slot = Slot::Create([function] { function(); }, origin);
    return bool(slot);
}
static ModuleHandle watched_module;
static unsigned retired_manager_notes;
extern "C" bool host_native_manager_note() {
    auto& host = HostBindings::instance();
    const auto state = host.lifecycle().module_state(watched_module);
    if (state.status != Status::ok || state.value != ModuleState::retiring) return false;
    ++retired_manager_notes;
    ue4ss_lua_host_context_info_v1 context{1, sizeof(context), {}, {}, {}};
    CHECK(HostApiRuntime::table().current_context(&context) == UE4SS_LUA_HOST_INVALID_HANDLE);
    CHECK(host.lifecycle().claim_module_finalization(watched_module) == Status::pending);
    return true;
}
extern "C" bool host_register_native_cpp_hook(std::function<void()> callback) {
    const auto origin = __builtin_extract_return_addr(__builtin_return_address(0));
    slot = Slot::Create(std::move(callback), origin);
    return bool(slot);
}
extern "C" void host_input_manager_note(Input::Handler* handler, bool destroying) {
    // Reentry detects a Native manager copy/destruction under the Input mutex
    // as an actual deadlock (the test runner imposes a bounded timeout).
    (void)handler->is_keydown_event_registered(Input::Key::A);
    auto& host = HostBindings::instance();
    auto state = host.lifecycle().module_state(watched_module);
    if (destroying && state.status == Status::ok && state.value == ModuleState::retiring) {
        ue4ss_lua_host_context_info_v1 context{1, sizeof(context), {}, {}, {}};
        CHECK(HostApiRuntime::table().current_context(&context) == UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(host.lifecycle().claim_module_finalization(watched_module) == Status::pending);
    }
}
extern "C" void host_input_copy_context() {
    auto& host=HostBindings::instance();
    CHECK(host.api().code_owner().status==Status::ok);
    CHECK(host.api().code_owner().value==watched_module);
    ue4ss_lua_host_context_info_v1 context{1,sizeof(context),{},{},{}};
    CHECK(HostApiRuntime::table().current_context(&context)==UE4SS_LUA_HOST_INVALID_HANDLE);
}
static std::shared_ptr<Slot> cross_child_slot;
static int host_cross_child_sdk(const LuaMadeSimple::Lua& actual) {
    bool refused{};
    try {cross_child_slot->Invoke();}
    catch(const std::runtime_error& e){refused=std::strstr(e.what(),"protected actual VM")!=nullptr;}
    CHECK(refused);actual.set_integer(42);return 1;
}
static void test_host_vm_slots() {
    auto& host = HostBindings::instance();
    auto& lua = LuaMadeSimple::new_state();
    auto* root = lua.get_lua_state();
    auto vm = host.attach_vm(root); CHECK(vm.status == Status::ok);
    auto& child = lua.new_thread();
    auto origin = reinterpret_cast<const void*>(&test_host_vm_slots);
    std::shared_ptr<Slot> normal, error, child_error, throwing;
    unsigned calls{};
    {
        LuaMadeSimple::LuaExecutionScope execution{root, root};
        normal = Slot::Create([&] {
            CHECK(actual_gate_depth > 0); CHECK(LuaMadeSimple::LuaExecutionScope::Contains(root));
            CHECK(host.api().code_owner().status == Status::invalid_handle);
            ue4ss_lua_host_context_info_v1 info{1,sizeof(info),{},{},{}};
            CHECK(HostApiRuntime::table().current_context(&info)==UE4SS_LUA_HOST_INVALID_HANDLE);
            lua_pushinteger(child.get_lua_state(), 91); lua_pop(child.get_lua_state(), 1);
            ++calls;
        }, origin);
        error = Slot::Create([&] { luaL_error(root, "host root prep error"); }, origin);
        child_error = Slot::Create([&] { luaL_error(child.get_lua_state(), "host child prep error"); }, origin);
        throwing = Slot::Create([] { throw std::logic_error("exact host callback exception"); }, origin);
    }
    CHECK(normal && normal->Invoke() && calls == 1); CHECK(actual_gate_depth==0);
    CHECK(error && !error->Invoke()); CHECK(actual_gate_depth==0);
    CHECK(child_error && !child_error->Invoke()); CHECK(actual_gate_depth==0);
    CHECK(host.api().code_owner().status==Status::invalid_handle);
    bool caught{};
    try { throwing->Invoke(); } catch (const std::logic_error& e) { caught=std::strcmp(e.what(), "exact host callback exception")==0; }
    CHECK(caught && actual_gate_depth==0);
    {
        LuaMadeSimple::LuaExecutionScope execution{root,root};
        cross_child_slot=Slot::Create([&]{child.throw_error("must refuse a different active child handler before Lua access");},origin);
        child.register_function("HostCrossChild",host_cross_child_sdk);
    }
    child.execute_string("return HostCrossChild()");
    CHECK(lua_tointeger(child.get_lua_state(),-1)==42 && actual_gate_depth==0);
    cross_child_slot.reset();
    std::shared_ptr<Slot> closing;
    {
        LuaMadeSimple::LuaExecutionScope execution{root, root};
        closing = Slot::Create([&] {
            CHECK(host.claim_vm_close(vm.value)==Status::pending); // active real lease covers the full body
            CHECK(actual_gate_depth>0); ++calls;
        }, origin);
    }
    CHECK(closing && closing->Invoke() && calls==2);
    CHECK(!normal->Invoke() && !closing->Invoke() && calls==2);
    CHECK(host.claim_vm_close(vm.value)==Status::ready);
    CHECK(host.cleanup_vm(vm.value)==Status::ok);
    {
        LuaMadeSimple::LuaFinalizationScope finalizing{root,root};
        lua_close(root); finalizing.close_completed(); CHECK(host.finish_vm_close(vm.value)==Status::ok);
    }
    CHECK(!normal->Invoke() && !error->Invoke() && !child_error->Invoke()); CHECK(actual_gate_depth==0);
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    CHECK(install_lua_host_integration()); CHECK(install_engine_callback_bindings());
    auto& host = HostBindings::instance();
    test_host_vm_slots();
    auto& lua = LuaMadeSimple::new_state();
    auto* root = lua.get_lua_state();
    const auto vm = host.attach_vm(root); CHECK(vm.status == Status::ok);
    auto* dll = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(dll != nullptr);
    auto function = reinterpret_cast<LuaMadeSimple::Lua::LuaFunction>(dlsym(dll, "native_lms"));
    auto calls = reinterpret_cast<int (*)()>(dlsym(dll, "native_calls"));
    auto hook_calls = reinterpret_cast<int (*)()>(dlsym(dll, "native_hook_calls"));
    auto lua_hook_calls = reinterpret_cast<int (*)()>(dlsym(dll, "native_lua_hook_calls"));
    auto register_hook = reinterpret_cast<bool (*)()>(dlsym(dll, "native_register_hook"));
    auto register_cpp_hook = reinterpret_cast<bool (*)()>(dlsym(dll, "native_register_cpp_hook"));
    auto queue_cpp_event = reinterpret_cast<bool (*)()>(dlsym(dll,"native_queue_cpp_event"));
    auto abi_callback = reinterpret_cast<ue4ss_lua_host_callback_v1>(dlsym(dll, "native_abi_callback"));
    CHECK(function && calls && hook_calls && register_hook && register_cpp_hook);
    auto module = host.attach_module(code_image(reinterpret_cast<const void*>(function))); CHECK(module.status == Status::ok);
    watched_module = module.value;
    Input::Handler input;
    auto input_register = reinterpret_cast<void (*)(Input::Handler*, bool)>(dlsym(dll,"native_register_input"));
    auto input_edit = reinterpret_cast<void (*)(Input::Handler*)>(dlsym(dll,"native_edit_input"));
    auto input_calls = reinterpret_cast<int (*)()>(dlsym(dll,"native_input_calls"));
    auto input_copies = reinterpret_cast<int (*)()>(dlsym(dll,"native_input_copies"));
    auto input_destroys = reinterpret_cast<int (*)()>(dlsym(dll,"native_input_destroys"));
    CHECK(input_register && input_edit && input_calls && input_copies && input_destroys);
    CHECK(host.api().dispatch_code(module.value,[&] { input_register(&input, true); input_register(&input, false); })==Status::ok);
    CHECK(input.is_keydown_event_registered(Input::Key::A));
    CHECK(input.is_keydown_event_registered(Input::Key::A, Input::ModifierKeyArray{Input::ModifierKey::CONTROL}));
    const auto copies_before_snapshot = input_copies();
    input.get_events_safe([&](auto& keys) {
        for (auto& entry : keys.key_data.at(Input::Key::A)) { CHECK(entry.custom_data2==&input); entry.callback(); }
    });
    CHECK(input_calls()==2 && input_copies()==copies_before_snapshot);
    CHECK(host.api().dispatch_code(module.value,[&] { input_edit(&input); })==Status::ok);
    CHECK(!input.is_keydown_event_registered(Input::Key::A));
    CHECK(input.is_keydown_event_registered(Input::Key::B)); // concurrent/descendant registration survived snapshot publish
    CHECK(input.is_keydown_event_registered(Input::Key::C)); // public callback assignment captured Native origin
    input.get_events_safe([&](auto& keys) { for (auto& [key,entries] : keys.key_data) for (auto& entry : entries) entry.callback(); });
    CHECK(input_calls()==4);
    CHECK(input_destroys()>0);
    auto other = host.attach_module(); CHECK(other.status == Status::ok);
    Input::Handler refused_input;
    CHECK(host.api().dispatch_code(other.value,[&] { input_register(&refused_input,true); input_register(&refused_input,false); })==Status::ok);
    CHECK(!refused_input.is_keydown_event_registered(Input::Key::A));
    auto deferred_register=reinterpret_cast<bool (*)()>(dlsym(dll,"native_register_deferred_hook"));
    auto deferred_calls=reinterpret_cast<int (*)()>(dlsym(dll,"native_deferred_hook_calls"));
    CHECK(deferred_register && deferred_calls && abi_callback);
    auto& temporary_lua=LuaMadeSimple::new_state(); auto* temporary_root=temporary_lua.get_lua_state();
    auto temporary_vm=host.attach_vm(temporary_root); CHECK(temporary_vm.status==Status::ok);
    auto temporary_stack=host.stack_binding(temporary_root,temporary_root); CHECK(temporary_stack.status==Status::ok);
    CHECK(host.api().dispatch_lua(temporary_vm.value,module.value,temporary_stack.value,[&](auto) {
        ue4ss_lua_callback_handle callback{};
        CHECK(HostApiRuntime::table().register_callback(abi_callback,&callback)==UE4SS_LUA_HOST_OK);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    CHECK(host.api().dispatch_code(module.value,[&] { CHECK(deferred_register()); })==Status::ok);
    auto deferred_slot=slot; CHECK(deferred_slot && deferred_slot->Invoke() && deferred_calls()==1);
    CHECK(host.claim_vm_close(temporary_vm.value)==Status::ready);
    CHECK(host.cleanup_vm(temporary_vm.value)==Status::ok);
    {
        LuaMadeSimple::LuaFinalizationScope finalizing{temporary_root,temporary_root};
        lua_close(temporary_root); finalizing.close_completed(); CHECK(host.finish_vm_close(temporary_vm.value)==Status::ok);
    }
    CHECK(host.api().registered_callback_vm(module.value).status==Status::invalid_handle);
    CHECK(!deferred_slot->Invoke() && deferred_calls()==1); // never falls back to code-only after its actual VM closed
    input.clear_subscribed_key(Input::Key::B); CHECK(!input.has_event_on_key(Input::Key::B));
    input.clear_subscribed_keys(); CHECK(!input.has_event_on_key(Input::Key::C));
    bool refused{};
    try { lua.register_function("native", function); } catch (const std::exception&) { refused = true; }
    CHECK(refused); // a function address alone cannot supply owner permission
    CHECK(host.api().dispatch_code(other.value, [&] { CHECK(!register_hook()); }) == Status::ok);
    CHECK(!slot && hook_calls() == 0);
    const auto stack = host.stack_binding(root, root); CHECK(stack.status == Status::ok);
    auto try_input_copy_error=reinterpret_cast<bool (*)(Input::Handler*)>(dlsym(dll,"native_try_input_copy_error"));
    CHECK(try_input_copy_error);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        CHECK(try_input_copy_error(&input));
        CHECK(!input.is_keydown_event_registered(Input::Key::D));
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    auto host_code_slot = Slot::Create([&] {
        ue4ss_lua_host_context_info_v1 info{1,sizeof(info),{},{},{}};
        CHECK(HostApiRuntime::table().current_context(&info)==UE4SS_LUA_HOST_INVALID_HANDLE);
        CHECK(host.api().code_owner().status==Status::invalid_handle);
    }, reinterpret_cast<const void*>(&main));
    CHECK(host_code_slot);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        CHECK(host_code_slot->Invoke());
        ue4ss_lua_host_context_info_v1 info{1,sizeof(info),{},{},{}};
        CHECK(HostApiRuntime::table().current_context(&info)==UE4SS_LUA_HOST_OK);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    CHECK(abi_callback);
    CHECK(host.api().dispatch_lua(vm.value, other.value, stack.value, [&](auto) {
        ue4ss_lua_callback_handle callback{99,99};
        CHECK(HostApiRuntime::table().register_callback(abi_callback,&callback)==UE4SS_LUA_HOST_WRONG_OWNER);
        CHECK(callback.id==0 && callback.generation==0);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    }) == UE4SS_LUA_HOST_OK);
    CHECK(host.api().dispatch_lua(vm.value, module.value, stack.value, [&](auto) {
        ue4ss_lua_callback_handle callback{};
        CHECK(HostApiRuntime::table().register_callback(abi_callback,&callback)==UE4SS_LUA_HOST_OK);
        CHECK(callback.id && callback.generation);
        lua.register_function("native", function);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    }) == UE4SS_LUA_HOST_OK);
    lua_pushliteral(root, "untouched root marker");
    auto& thread = lua.new_thread();
    lua_getglobal(thread.get_lua_state(), "native"); lua_pushinteger(thread.get_lua_state(), 21);
    { LuaMadeSimple::LuaExecutionScope execution{thread.get_lua_state(),root};
      CHECK(lua_pcall(thread.get_lua_state(), 1, 1, 0) == LUA_OK); }
    CHECK(lua_tointeger(thread.get_lua_state(), -1) == 42 && calls() == 1);
    CHECK(lua_gettop(root) == 2 && std::strcmp(lua_tostring(root, 1), "untouched root marker") == 0);
    auto register_internal=reinterpret_cast<void (*)(const LuaMadeSimple::Lua&)>(dlsym(dll,"native_register_internal"));
    auto internal_calls=reinterpret_cast<int (*)()>(dlsym(dll,"native_internal_calls"));
    CHECK(register_internal && internal_calls);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        register_internal(lua);return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    lua.open_all_libs();
    lua.execute_string("assert(NativeInternal.call(39)==42); local ok=pcall(NativeInternal.call,-1); assert(not ok); local c=coroutine.create(function() return NativeInternal.call(39) end); local ok,v=coroutine.resume(c); assert(ok and v==42)");
    CHECK(internal_calls()==3);
    CHECK(lua_gettop(root)==2 && std::strcmp(lua_tostring(root,1),"untouched root marker")==0);
    auto register_debug=reinterpret_cast<void (*)(const LuaMadeSimple::Lua&)>(dlsym(dll,"native_register_debug"));
    auto debug_calls=reinterpret_cast<int (*)()>(dlsym(dll,"native_debug_calls"));
    auto debug_error=reinterpret_cast<void (*)(bool)>(dlsym(dll,"native_debug_error"));
    CHECK(register_debug && debug_calls && debug_error);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto){register_debug(lua);return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};})==UE4SS_LUA_HOST_OK);
    lua.execute_string("local c=coroutine.create(function() local a=40; return a+2 end); local ok,v=coroutine.resume(c); assert(ok and v==42)");
    auto& debug_child=lua.new_thread();lua_pop(root,1);
    CHECK(debug_calls()>0); // actual Raw thread birth inherited the original Native hook ticket
    debug_error(true);bool debug_failed{};
    try{lua.execute_string("local value=42");}catch(const std::exception&){debug_failed=true;}
    CHECK(debug_failed);debug_error(false);
    CHECK(actual_gate_depth==0 && host.api().code_owner().status==Status::invalid_handle);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto){lua.set_hook(nullptr,0,0);return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};})==UE4SS_LUA_HOST_OK);
    auto register_aux=reinterpret_cast<void (*)(const LuaMadeSimple::Lua&)>(dlsym(dll,"native_register_aux"));
    auto post_calls=reinterpret_cast<int (*)()>(dlsym(dll,"native_post_calls"));
    auto error_calls=reinterpret_cast<int (*)()>(dlsym(dll,"native_error_calls"));
    CHECK(register_aux && post_calls && error_calls);
    lua.open_all_libs();
    lua.execute_string("local c=coroutine.create(function() return native(21) end); local ok,v=coroutine.resume(c); assert(ok and v==42)");
    CHECK(calls()==2); // Lua-created coroutine is genuinely adopted and rooted
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        register_aux(lua);return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    lua_getglobal(root,"native");lua_pushinteger(root,5);
    { LuaMadeSimple::LuaExecutionScope execution{root,root};CHECK(lua_pcall(root,1,1,0)==LUA_OK); }
    CHECK(lua_tointeger(root,-1)==10 && post_calls()==1);lua_pop(root,1);
    try {thread.execute_string("error('auxiliary observer')");} catch(const std::exception&) {}
    CHECK(error_calls()==1); // reentrant unregister completed outside registry lock
    auto error_post=reinterpret_cast<void (*)(const LuaMadeSimple::Lua&)>(dlsym(dll,"native_register_error_post"));CHECK(error_post);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        error_post(thread);return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    lua_settop(thread.get_lua_state(),0);lua_getglobal(thread.get_lua_state(),"native");lua_pushinteger(thread.get_lua_state(),7);
    {LuaMadeSimple::LuaExecutionScope execution{thread.get_lua_state(),root};CHECK(lua_pcall(thread.get_lua_state(),1,1,0)!=LUA_OK);}
    CHECK(calls()==4 && post_calls()==2); // Native post Lua error returned through protected owner boundary
    const auto& api=HostApiRuntime::table();
    CHECK(host.dispatch_callback_lua(vm.value,module.value,[&](auto context) {
        int top=-1;CHECK(api.stack_top(context,&top)==UE4SS_LUA_HOST_OK && top==0);
        ue4ss_lua_value_v1 value{};value.abi_version=1;value.struct_size=sizeof(value);
        value.kind=UE4SS_LUA_VALUE_INTEGER;value.integer_value=111;
        CHECK(api.value_push(context,&value)==UE4SS_LUA_HOST_OK);
        CHECK(host.dispatch_callback_lua(vm.value,module.value,[&](auto nested) {
            CHECK(nested.id!=context.id);
            int nested_top=-1;CHECK(api.stack_top(nested,&nested_top)==UE4SS_LUA_HOST_OK && nested_top==0);
            value.integer_value=222;CHECK(api.value_push(nested,&value)==UE4SS_LUA_HOST_OK);
            return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
        })==UE4SS_LUA_HOST_OK);
        CHECK(api.stack_top(context,&top)==UE4SS_LUA_HOST_OK && top==1);
        CHECK(api.value_read(context,1,&value)==UE4SS_LUA_HOST_OK && value.integer_value==111);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    CHECK(lua_gettop(root)==2 && std::strcmp(lua_tostring(root,1),"untouched root marker")==0);
    bool callback_threw=false;
    try { host.dispatch_callback_lua(vm.value,module.value,[](auto)->ue4ss_lua_host_status {throw std::runtime_error("thread cleanup");}); }
    catch(const std::runtime_error&) {callback_threw=true;}
    CHECK(callback_threw);
    unsigned queued_calls{};std::function<void()> queued;
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        auto event=host.api().wrap_event([&] {
            ue4ss_lua_host_context_info_v1 info{1,sizeof(info),{},{},{}};
            CHECK(api.current_context(&info)==UE4SS_LUA_HOST_OK);
            int top=-1;CHECK(api.stack_top(info.context,&top)==UE4SS_LUA_HOST_OK && top==0);
            ++queued_calls;
        });
        CHECK(event.status==Status::ok);queued=std::move(event.value);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    queued();CHECK(queued_calls==1);queued={};
    CHECK(host.api().registered_callback_vm(module.value).value==vm.value);
    CHECK(host.api().dispatch_code(module.value, [&] { CHECK(register_hook()); }) == Status::ok);
    CHECK(slot && slot->Invoke() && hook_calls() == 1);
    CHECK(lua_hook_calls && lua_hook_calls()==1); // registration preceded its Lua dispatch
    CHECK(host.api().dispatch_code(module.value, [&] { CHECK(register_cpp_hook()); }) == Status::ok);
    CHECK(slot && slot->Invoke() && hook_calls() == 2);
    CHECK(lua_hook_calls()==1);
    auto& second_lua=LuaMadeSimple::new_state();auto* second_root=second_lua.get_lua_state();
    auto second_vm=host.attach_vm(second_root);CHECK(second_vm.status==Status::ok);
    auto second_stack=host.stack_binding(second_root,second_root);CHECK(second_stack.status==Status::ok);
    CHECK(host.api().dispatch_lua(vm.value,module.value,second_stack.value,[](auto){return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};})==UE4SS_LUA_HOST_WRONG_OWNER);
    CHECK(host.api().dispatch_lua(second_vm.value,module.value,second_stack.value,[&](auto) {
        ue4ss_lua_callback_handle callback{};CHECK(api.register_callback(abi_callback,&callback)==UE4SS_LUA_HOST_OK);
        return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    CHECK(host.api().registered_callback_vm(module.value).status==Status::wrong_owner);
    CHECK(host.api().dispatch_code(module.value,[&] {CHECK(register_hook());})==Status::ok);
    CHECK(slot && slot->Invoke() && hook_calls()==3 && lua_hook_calls()==1); // ambiguous VM stays masked
    CHECK(host.claim_vm_close(second_vm.value)==Status::ready);
    CHECK(host.cleanup_vm(second_vm.value)==Status::ok);
    { LuaMadeSimple::LuaFinalizationScope closing{second_root,second_root};
      lua_close(second_root);closing.close_completed();CHECK(host.finish_vm_close(second_vm.value)==Status::ok); }
    CHECK(host.api().registered_callback_vm(module.value).value==vm.value);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        CHECK(register_hook());return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    CHECK(slot && slot->Invoke() && hook_calls()==4 && lua_hook_calls()==2);
    CHECK(slot->Invoke() && hook_calls()==5 && lua_hook_calls()==3);
    CHECK(lua_gettop(root)==2 && std::strcmp(lua_tostring(root,1),"untouched root marker")==0);
    CHECK(host.api().dispatch_code(module.value,[&] {CHECK(register_cpp_hook());})==Status::ok);
    CHECK(queue_cpp_event);
    CHECK(host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto) {
        CHECK(queue_cpp_event());return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
    })==UE4SS_LUA_HOST_OK);
    CHECK(host.lifecycle().request_module_retire(module.value) == Status::pending);
    CHECK(!slot->Invoke() && hook_calls() == 5);
    lua_settop(thread.get_lua_state(), 0); lua_getglobal(thread.get_lua_state(), "native"); lua_pushinteger(thread.get_lua_state(), 9);
    { LuaMadeSimple::LuaExecutionScope execution{thread.get_lua_state(),root};
      CHECK(lua_pcall(thread.get_lua_state(), 1, 1, 0) != LUA_OK); }
    const auto before_retired_debug=debug_calls();bool retired_debug_refused{};
    try{debug_child.execute_string("local value=42");}catch(const std::exception&){retired_debug_refused=true;}
    CHECK(retired_debug_refused && debug_calls()==before_retired_debug);
    lua.execute_string("local ok=pcall(NativeInternal.call,39); assert(not ok)");
    CHECK(internal_calls()==3); // retired internal instructions are also never entered
    CHECK(calls() == 4); // retired native callback's instructions are never entered
    CHECK(host.claim_module_retire(module.value) == Status::ready);
    input.get_events_safe([&](auto& keys) { for(auto& [key,entries]:keys.key_data) for(auto& entry:entries) entry.callback(); });
    CHECK(input_calls()==4);
    queued_native();CHECK(hook_calls()==5); // canceled but still retained queue wrapper is inert
    CHECK(retired_manager_notes > 0);
    CHECK(Unreal::Hook::Internal::FCallbackOwnerBridge::QuarantinedCount() == 0);
    CHECK(!slot); // manager's refused descendant registration cleared fixture slot
    CHECK(host.cleanup_module(module.value) == Status::ok);
    CHECK(dlclose(dll) == 0); // all Native owners drained
    input.get_events_safe([](auto& keys){keys.key_data.clear();});
    queued_native();queued_native={}; // actual unloaded manager is never reached
    CHECK(host.finish_module_retire(module.value) == Status::ok);
    CHECK(!slot); // no descendant callback remains after actual library unload
    CHECK(host.claim_module_retire(other.value) == Status::ready);
    CHECK(host.cleanup_module(other.value) == Status::ok);
    CHECK(host.finish_module_retire(other.value) == Status::ok);
    CHECK(host.claim_vm_close(vm.value) == Status::ready);
    CHECK(host.cleanup_vm(vm.value) == Status::ok);
    { LuaMadeSimple::LuaFinalizationScope closing{root,root};
      lua_close(root);closing.close_completed();CHECK(host.finish_vm_close(vm.value) == Status::ok); }
    CHECK(host.vm_count() == 0);
    std::cout << checks << " real LuaMadeSimple/loaded module/engine broker assertions PASS\n";
}
