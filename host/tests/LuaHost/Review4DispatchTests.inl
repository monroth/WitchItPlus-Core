// Permanent Review-4 repros, shared by Linux actual-stack and Windows linked gate.
// Review4Vm supplies real VM lifecycle; engine registration retains real owner
// slots, replacing engine patching only. Object/output marshalling is nil.
static void review4_check(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
static LuaMod::LuaCancellableCallbackData review4_nested_callback;
static int review4_replacements[2];
static int review4_reenter(const LuaMadeSimple::Lua& lua) {
    std::vector<PinnedNotify> inner;
    {std::lock_guard lock{LuaMod::m_thread_actions_mutex};inner.emplace_back(review4_nested_callback);}
    deliver_notify(inner.front(),[](const auto& value){value.set_nil();});
    for(auto& ref:review4_replacements) {
        lua_pushliteral(lua.get_lua_state(),"replacement survives outer cancellation");
        ref=luaL_ref(lua.get_lua_state(),LUA_REGISTRYINDEX);
    }
    review4_check(review4_replacements[0]==review4_nested_callback.lua_callback_thread_ref &&
        review4_replacements[1]==review4_nested_callback.lua_callback_function_ref,"notify test did not reuse both released registry slots");
    return 0;
}
static void review4_notify() {
    Review4Vm vm{L"Review4Notify"};auto& lua=vm.lua();
    {
        LuaHost::ModLuaEntry entry{lua};auto* root=lua.get_lua_state();
        lua.register_function("ReenterNotify",review4_reenter);
        lua.execute_string("depth=0; function nested(_) depth=depth+1; if depth==1 then ReenterNotify() end; return true end");
        lua_getglobal(root,"nested");auto function=luaL_ref(root,LUA_REGISTRYINDEX);
        lua_pushthread(root);auto thread=luaL_ref(root,LUA_REGISTRYINDEX);
        review4_nested_callback={900001,&lua,{},{},function,thread};
        {std::lock_guard lock{LuaMod::m_thread_actions_mutex};LuaMod::m_static_construct_object_lua_callbacks.push_back(review4_nested_callback);}
        {PinnedNotify outer{review4_nested_callback};deliver_notify(outer,[](const auto& value){value.set_nil();});}
        review4_check(LuaMod::m_static_construct_object_lua_callbacks.empty(),"nested notify registration survived cancellation");
        for(auto ref:review4_replacements) {
            lua_rawgeti(root,LUA_REGISTRYINDEX,ref);auto survived=lua_type(root,-1)==LUA_TSTRING;lua_pop(root,1);
            review4_check(survived,"nested notify cancellation released a reused registry slot");
            luaL_unref(root,LUA_REGISTRYINDEX,ref);
        }
        // Registry allocations must still receive distinct slots after re-entry.
        std::vector<int> refs;
        for(int i=0;i<12;++i) {lua_pushinteger(root,i);auto ref=luaL_ref(root,LUA_REGISTRYINDEX);
            review4_check(std::ranges::find(refs,ref)==refs.end(),"notify cleanup corrupted registry free list");refs.push_back(ref);}
        for(auto ref:refs) luaL_unref(root,LUA_REGISTRYINDEX,ref);
    }
    vm.close();std::cout<<"ROUND3 PASS nested notify: single cleanup claim, reused function/thread refs survive\n";
}
static void review4_console(bool global) {
    Review4Vm vm{global?L"Review4GlobalCommand":L"Review4ViewportCommand"};auto& lua=vm.lua();
    auto& container=global?LuaMod::m_global_command_lua_callbacks:LuaMod::m_custom_command_lua_pre_callbacks;
    {
        LuaHost::ModLuaEntry entry{lua};auto* root=lua.get_lua_state();
        lua.execute_string("first_calls=0; second_calls=0; function one(...) first_calls=first_calls+1; return false end; function two(...) second_calls=second_calls+1; return true end");
        LuaMod::LuaCallbackData data{&lua,nullptr,{}};
        for(auto name:{"one","two"}) {lua_getglobal(root,name);auto ref=luaL_ref(root,LUA_REGISTRYINDEX);data.registry_indexes.emplace_back(&lua,LuaMod::LuaCallbackData::RegistryIndex{ref,0});}
        {std::lock_guard lock{LuaMod::m_thread_actions_mutex};container.emplace(STR("shared"),data);}
    }
    bool handled=review4_dispatch_console(global);
    {LuaHost::ModLuaEntry entry{lua};
        review4_check(handled,"console dispatch dropped the second handler");
        lua.execute_string("assert(first_calls==1 and second_calls==1)");
    }
    {std::lock_guard lock{LuaMod::m_thread_actions_mutex};auto& refs=container.at(STR("shared")).registry_indexes;std::reverse(refs.begin(),refs.end());}
    handled=review4_dispatch_console(global);
    review4_check(!handled,"console dispatch changed last-handler result policy");
    {LuaHost::ModLuaEntry entry{lua};lua.execute_string("assert(first_calls==2 and second_calls==2)");}
    vm.close();review4_check(container.empty(),"console registry survived VM unload");
    std::cout<<"ROUND3 PASS "<<(global?"global":"viewport")<<" console: all handlers and last-handler result\n";
}
using Review4Slot=Unreal::Hook::Internal::TCallbackSlot<std::function<void()>>;
static std::vector<std::shared_ptr<Review4Slot>> review4_tick_slots,review4_event_slots;
static bool review4_register_dispatcher(bool tick) {
    auto callback=Review4Slot::Create([tick]{review4_deliver_dispatcher(tick);},reinterpret_cast<const void*>(&review4_register_dispatcher));
    review4_check(bool(callback),"shared dispatcher registration was refused");
    (tick?review4_tick_slots:review4_event_slots).push_back(std::move(callback));return true;
}
static void review4_shared(bool tick) {
    offline_shared_dispatcher_registration=review4_register_dispatcher;
    struct Reset {~Reset(){offline_shared_dispatcher_registration=nullptr;review4_tick_slots.clear();review4_event_slots.clear();if(offline_reset_shared_dispatchers) offline_reset_shared_dispatchers();}} reset;
    // Real public host setup; only engine-dependent remainder is bypassed.
    LuaMod::on_program_start();
    Review4Vm first{L"Review4FirstScheduler"},live{L"Review4LiveScheduler"};
    {LuaHost::ModLuaEntry entry{first.lua()};offline_ensure_shared_dispatcher(tick,first.mod());}
    {
        LuaHost::ModLuaEntry entry{live.lua()};
        offline_ensure_shared_dispatcher(tick,live.mod());
        auto& lua=live.lua();lua.execute_string("runs=0; function continuation() runs=runs+1 end");
        lua_getglobal(lua.get_lua_state(),"continuation");auto ref=luaL_ref(lua.get_lua_state(),LUA_REGISTRYINDEX);
        std::lock_guard lock{LuaMod::m_thread_actions_mutex};
        (tick?LuaMod::m_engine_tick_actions:LuaMod::m_game_thread_actions).push_back({&lua,ref,LUA_NOREF});
    }
    auto& slots=tick?review4_tick_slots:review4_event_slots;
    first.close();
    review4_check(!slots.empty() && slots.front()->Invoke(),"closing first scheduler revoked shared registered dispatcher");
    review4_check(slots.size()==1,"ProcessEvent dispatcher was registered once per mod");
    {LuaHost::ModLuaEntry entry{live.lua()};live.lua().execute_string("assert(runs==1)");
        // Repeated scheduling after another VM closes must reuse the live slot.
        lua_getglobal(live.lua().get_lua_state(),"continuation");auto ref=luaL_ref(live.lua().get_lua_state(),LUA_REGISTRYINDEX);
        offline_ensure_shared_dispatcher(tick,live.mod());
        std::lock_guard lock{LuaMod::m_thread_actions_mutex};(tick?LuaMod::m_engine_tick_actions:LuaMod::m_game_thread_actions).push_back({&live.lua(),ref,LUA_NOREF});}
    review4_check(slots.front()->Invoke(),"shared registered dispatcher failed second delivery");
    {LuaHost::ModLuaEntry entry{live.lua()};live.lua().execute_string("assert(runs==2)");}
    live.close();
    std::cout<<"ROUND3 PASS shared "<<(tick?"EngineTick":"ProcessEvent")<<": registered owner slot survives first scheduling VM close\n";
}
static void review4_callback_release() {
    Review4Vm vm{L"ScopedCallbackRelease"};
    {
        LuaHost::ModLuaEntry entry{vm.lua()};auto& lua=vm.lua();auto* state=lua.get_lua_state();
        lua.execute_string("weak=setmetatable({},{__mode='v'}); do local f=function() end; weak[1]=f; saved=f end");
        lua_getglobal(state,"saved");int ref=luaL_ref(state,LUA_REGISTRYINDEX);
        lua.execute_string("saved=nil; collectgarbage(); assert(weak[1])");
        LuaMod::FunctionHookData data{};
        data.callback_data.lua=&lua;
        data.callback_data.registry_indexes.emplace_back(&lua,LuaMod::LuaCallbackData::RegistryIndex{ref,1823456});
        LuaMod::m_script_hook_callbacks.push_back(std::move(data));
        // No UFunction exists in this fixture: cancellation must not look it up.
        review4_check(cancel_script_callback(lua,1823456,1823456),"scope cancellation depended on class residency");
        review4_check(LuaMod::m_script_hook_callbacks.back().callback_data.registry_indexes.front().second.lua_index==-1,
                      "cancelled script index remains live");
        ref=-1;
        release_lua_callback(lua,ref);
        review4_check(ref==-1,"cancelled callback index remains live");
        lua.execute_string("collectgarbage(); collectgarbage(); assert(weak[1]==nil)");
        lua.execute_string("function replacement() return 42 end");
        lua_getglobal(state,"replacement");auto reused=luaL_ref(state,LUA_REGISTRYINDEX);
        release_lua_callback(lua,ref); // no double-unref of a reused native slot
        lua_rawgeti(state,LUA_REGISTRYINDEX,reused);
        review4_check(lua_isfunction(state,-1),"repeated cancellation released replacement callback");lua_pop(state,1);
        luaL_unref(state,LUA_REGISTRYINDEX,reused);
    }
    vm.close();
    std::cout<<"SCOPED CALLBACK PASS registry root released once; replacement survives repeated cancel\n";
}
static void review4_gates(int mode=0) {
    review4_callback_release();
    if(!mode || mode==4) review4_notify();
    if(!mode || mode==5) review4_shared(true);
    if(!mode || mode==6) review4_shared(false);
    if(!mode || mode==7) review4_console(false);
    if(!mode || mode==8) review4_console(true);
    review4_check(LuaHost::HostBindings::instance().vm_count()==0,"review4 VM pin leaked");
}
