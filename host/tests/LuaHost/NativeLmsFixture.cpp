#include <LuaHost/UIKitLuaHost.h>
#include <functional>
#include <cstdlib>
namespace RC::LuaMadeSimple { class Lua; }
static int calls{}, hook_calls{}, lua_hook_calls{};
extern "C" int native_calls() { return calls; }
extern "C" int native_hook_calls() { return hook_calls; }
extern "C" int native_lua_hook_calls() { return lua_hook_calls; }
extern "C" ue4ss_lua_host_status native_abi_callback(ue4ss_lua_context_handle) {
    return UE4SS_LUA_HOST_OK;
}
extern "C" int native_lms(const RC::LuaMadeSimple::Lua&) {
    const ue4ss_uikit_lua_host_api_v1* api{};
    if (UE4SS_GetUIKitLuaHostApi_v1(1, sizeof(*api), &api) != UE4SS_LUA_HOST_OK) return 0;
    ue4ss_lua_host_context_info_v1 info{1, sizeof(info), {}, {}, {}};
    if (api->current_context(&info) != UE4SS_LUA_HOST_OK) return 0;
    ue4ss_lua_value_v1 value{}; value.abi_version = 1; value.struct_size = sizeof(value);
    if (api->value_read(info.context, 1, &value) != UE4SS_LUA_HOST_OK || value.kind != UE4SS_LUA_VALUE_INTEGER) return 0;
    value.integer_value *= 2;
    if (api->value_push(info.context, &value) != UE4SS_LUA_HOST_OK) return 0;
    ++calls; return 1;
}
static void native_hook() {
    ++hook_calls;
    const ue4ss_uikit_lua_host_api_v1* api{};
    if (UE4SS_GetUIKitLuaHostApi_v1(1,sizeof(*api),&api)!=UE4SS_LUA_HOST_OK) std::abort();
    ue4ss_lua_host_context_info_v1 info{1,sizeof(info),{},{},{}};
    if (api->current_context(&info)!=UE4SS_LUA_HOST_OK) return;
    int top=-1;
    if (api->stack_top(info.context,&top)!=UE4SS_LUA_HOST_OK || top!=0) std::abort();
    ue4ss_lua_value_v1 value{};value.abi_version=1;value.struct_size=sizeof(value);
    value.kind=UE4SS_LUA_VALUE_INTEGER;value.integer_value=314;
    if(api->value_push(info.context,&value)!=UE4SS_LUA_HOST_OK) std::abort();
    ++lua_hook_calls;
}
extern "C" bool host_register_native_hook(void (*)());
extern "C" bool native_register_hook() { return host_register_native_hook(native_hook); }

extern "C" bool host_register_native_cpp_hook(std::function<void()>);
extern "C" bool host_native_manager_note();
struct NativePayload {
    void operator()() const { ++hook_calls; }
    ~NativePayload() {
        if (host_native_manager_note()) {
            // Ordinary teardown reentry must refuse new work while retaining
            // the original manager's owner pin for the rejected child manager.
            const bool admitted = host_register_native_cpp_hook([] {});
            if (admitted) std::abort();
        }
    }
};
extern "C" bool native_register_cpp_hook() {
    std::function<void()> function = NativePayload{};
    return host_register_native_cpp_hook(std::move(function));
}
extern "C" bool host_queue_native_cpp_event(std::function<void()>);
extern "C" bool native_queue_cpp_event() {
    std::function<void()> function=NativePayload{};
    return host_queue_native_cpp_event(std::move(function));
}

#include <LuaMadeSimple/LuaMadeSimple.hpp>
static int post_calls, error_calls;
extern "C" int native_post_calls() {return post_calls;}
extern "C" int native_error_calls() {return error_calls;}
static void native_post(const RC::LuaMadeSimple::Lua&) {++post_calls;}
static void native_error_observer(lua_State*,const std::string&,const std::string&) {
    ++error_calls;
    RC::LuaMadeSimple::unregister_error_callback(native_error_observer);
}
extern "C" void native_register_aux(const RC::LuaMadeSimple::Lua& lua) {
    const_cast<RC::LuaMadeSimple::Lua&>(lua).register_post_function_process_callback(native_post);
    RC::LuaMadeSimple::register_error_callback(native_error_observer);
}

static void native_error_post(const RC::LuaMadeSimple::Lua& lua) {
    ++post_calls;lua.throw_error("Native post processing error");
}
extern "C" void native_register_error_post(const RC::LuaMadeSimple::Lua& lua) {
    const_cast<RC::LuaMadeSimple::Lua&>(lua).register_post_function_process_callback(native_error_post);
}

static int internal_calls;
extern "C" int native_internal_calls(){return internal_calls;}
static int native_internal(lua_State* actual) {
    ++internal_calls;
    if(lua_type(actual,lua_upvalueindex(1))!=LUA_TNONE)
        return luaL_error(actual,"Internal consumer received host metadata upvalue");
    ue4ss_lua_host_context_info_v1 info{1,sizeof(info),{},{},{}};
    const ue4ss_uikit_lua_host_api_v1* api{};
    if(UE4SS_GetUIKitLuaHostApi_v1(1,sizeof(*api),&api)!=UE4SS_LUA_HOST_OK ||
       api->current_context(&info)!=UE4SS_LUA_HOST_OK)
        return luaL_error(actual,"Missing original module/actual VM context");
    if(lua_tointeger(actual,1)<0) return luaL_error(actual,"Actual Native internal Lua error");
    lua_pushinteger(actual,lua_tointeger(actual,1)+3);return 1;
}
extern "C" void native_register_internal(const RC::LuaMadeSimple::Lua& lua) {
    auto table=lua.prepare_new_table();
    table.add_pair("call",native_internal);
    table.make_global("NativeInternal");
}


#include <Input/Handler.hpp>
static int input_calls{}, input_copies{}, input_destroys{};
extern "C" int native_input_calls() { return input_calls; }
extern "C" int native_input_copies() { return input_copies; }
extern "C" int native_input_destroys() { return input_destroys; }
extern "C" void host_input_manager_note(RC::Input::Handler*, bool);
struct NativeInputPayload {
    RC::Input::Handler* handler;
    explicit NativeInputPayload(RC::Input::Handler* value) : handler(value) {}
    NativeInputPayload(const NativeInputPayload& other) : handler(other.handler) {
        ++input_copies; host_input_manager_note(handler, false);
    }
    ~NativeInputPayload() { ++input_destroys; host_input_manager_note(handler, true); }
    void operator()() const { ++input_calls; }
};
extern "C" void native_register_input(RC::Input::Handler* handler, bool modifiers) {
    std::function<void()> callback = NativeInputPayload{handler};
    if (modifiers) handler->register_keydown_event(RC::Input::Key::A, RC::Input::ModifierKeyArray{RC::Input::ModifierKey::CONTROL}, callback, 7, handler);
    else handler->register_keydown_event(RC::Input::Key::A, std::move(callback), 7, handler);
}
extern "C" void native_edit_input(RC::Input::Handler* handler) {
    handler->get_events_safe([=](auto& keys) {
        // Each query takes the actual Input mutex. It must be available during
        // the visitor, manager copies, removals, and descendant registrations.
        (void)handler->is_keydown_event_registered(RC::Input::Key::A);
        keys.key_data.erase(RC::Input::Key::A);
        handler->register_keydown_event(RC::Input::Key::B, NativeInputPayload{handler});
        keys.key_data[RC::Input::Key::C].emplace_back().callback = std::function<void()>{NativeInputPayload{handler}};
    });
}

#include <stdexcept>
static bool input_copy_should_throw;
extern "C" void host_input_copy_context();
struct NativeThrowInputPayload {
    NativeThrowInputPayload() = default;
    NativeThrowInputPayload(const NativeThrowInputPayload&) {
        if (input_copy_should_throw) {
            host_input_copy_context();
            throw std::logic_error("exact Native manager copy error");
        }
    }
    void operator()() const {}
};
extern "C" bool native_try_input_copy_error(RC::Input::Handler* handler) {
    std::function<void()> callback=NativeThrowInputPayload{};
    input_copy_should_throw=true;
    try { handler->register_keydown_event(RC::Input::Key::D, RC::Input::ModifierKeyArray{RC::Input::ModifierKey::CONTROL}, callback); }
    catch (const std::logic_error& error) {
        input_copy_should_throw=false;
        return std::string{error.what()}=="exact Native manager copy error";
    }
    input_copy_should_throw=false; return false;
}

static int deferred_hook_calls;
extern "C" int native_deferred_hook_calls() { return deferred_hook_calls; }
static void native_deferred_hook() { ++deferred_hook_calls; }
extern "C" bool native_register_deferred_hook() { return host_register_native_hook(native_deferred_hook); }

static int debug_calls;static bool debug_error;
extern "C" int native_debug_calls(){return debug_calls;}
extern "C" void native_debug_error(bool enabled){debug_error=enabled;}
static void native_debug(lua_State* actual,lua_Debug*) {
    ++debug_calls;
    ue4ss_lua_host_context_info_v1 info{1,sizeof(info),{},{},{}};
    const ue4ss_uikit_lua_host_api_v1* api{};
    if(UE4SS_GetUIKitLuaHostApi_v1(1,sizeof(*api),&api)!=UE4SS_LUA_HOST_OK || api->current_context(&info)!=UE4SS_LUA_HOST_OK)
        luaL_error(actual,"Missing debug module/actual VM owner");
    if(debug_error) luaL_error(actual,"Actual Native debug Lua error");
}
extern "C" void native_register_debug(const RC::LuaMadeSimple::Lua& lua){lua.set_hook(native_debug,LUA_MASKCOUNT,1);}
