// Included by ModLifecycleGate.inl. Real LuaMod async thread, LoopAsync binding,
// scheduling core, script/notify admission and action dispatch; no engine init.
static std::atomic<bool> round2_gate_held{},round2_snapshot_ready{};
static std::thread::id round2_async_thread;
static int round2_reference{};
static int round2_wait(const LuaMadeSimple::Lua&) {
    round2_async_thread=std::this_thread::get_id();
    round2_gate_held.store(true,std::memory_order_release);
    while(!round2_snapshot_ready.load(std::memory_order_acquire)) std::this_thread::yield();
    return 0;
}
static void round2_prepare(const LuaMadeSimple::Lua& lua) {
    register_loop_async(lua);
    lua.register_function("GateWait",round2_wait);
    lua.register_function("ExecuteInGameThread",[](const LuaMadeSimple::Lua& value)->int {
        if(!value.is_function(1)) value.throw_error("callback required");
        queue_game_thread_action(value,get_mod_ref(value),GameThreadExecutionMethod::EngineTick,1);
        return 0;
    });
    lua.execute_string("deliveries=0; game_ran=0; function delivery(_) deliveries=deliveries+1; return false end");
    lua_getglobal(lua.get_lua_state(),"delivery");round2_reference=luaL_ref(lua.get_lua_state(),LUA_REGISTRYINDEX);
}
struct Round2Preparation {
    const LuaMod* mod;
    explicit Round2Preparation(LuaMod& value):mod(&value) {offline_preparations.emplace(mod,round2_prepare);}
    ~Round2Preparation() {offline_preparations.erase(mod);}
};
static void round2_assert(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
static void round2_concurrent(UE4SSProgram& program,const std::filesystem::path& scratch,bool notify) {
    round2_gate_held=false;round2_snapshot_ready=false;
    std::jthread watchdog{[](std::stop_token stop) {
        for(unsigned i=0;i<160 && !stop.stop_requested();++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if(!stop.stop_requested()) {
            const bool armed=round2_gate_held.load() && round2_snapshot_ready.load();
            std::cerr<<(armed ? "ROUND2_DEADLOCK_TIMEOUT: actual async gate vs delivery action mutex\n" : "ROUND2_SETUP_TIMEOUT\n")<<std::flush;
            std::_Exit(armed ? 86 : 87);
        }
    }};
    const auto path=scratch/(notify ? L"ConcurrentNotify" : L"ConcurrentScript");
    std::filesystem::create_directories(path/L"Scripts");
    std::ofstream(path/L"Scripts/main.lua") << "LoopAsync(0,function() GateWait(); ExecuteInGameThread(function() game_ran=game_ran+1 end); return true end)";
    LuaMod mod{program,notify ? StringType(STR("ConcurrentNotify")) : StringType(STR("ConcurrentScript")),path.wstring()};
    Round2Preparation prepare{mod};
    struct Cleanup {LuaMod& mod;~Cleanup(){before_detached_lua_entry=nullptr;mod.uninstall();}} cleanup{mod};
    mod.start_mod();round2_assert(mod.is_started(),"concurrency mod did not start");
    while(!round2_gate_held.load(std::memory_order_acquire)) std::this_thread::yield();
    before_detached_lua_entry=[] {round2_snapshot_ready.store(true,std::memory_order_release);};
    if(notify) {
        std::vector<PinnedNotify> ready;
        {
            std::lock_guard guard{LuaMod::m_thread_actions_mutex};
            const auto id=LuaMod::m_next_static_construct_callback_id++;
            auto& callback=LuaMod::m_static_construct_object_lua_callbacks.emplace_back(LuaMod::LuaCancellableCallbackData{id,&mod.lua(),{},{},round2_reference,LUA_NOREF});
            ready.emplace_back(callback);
        }
        deliver_notify(ready.front(),[](const auto& lua){lua.set_nil();});
    } else {
        std::vector<PinnedRegistryCallback> ready;
        {
            std::lock_guard guard{LuaMod::m_thread_actions_mutex};
            ready.emplace_back(&mod.lua(),LuaMod::LuaCallbackData::RegistryIndex{round2_reference,1});
        }
        deliver_script_callback(ready.front(),[]{return true;},[](const auto& lua,const auto& ref){
            lua.registry().get_function_ref(ref.lua_index);lua.set_nil();lua.call_function(1,0);
        });
    }
    before_detached_lua_entry=nullptr;
    round2_assert(round2_async_thread==mod.get_async_thread_id(),"LoopAsync did not execute on real async thread");
    process_simple_actions(LuaMod::m_engine_tick_actions);
    {LuaHost::ModLuaEntry entry{mod.lua()};mod.lua().execute_string("assert(game_ran==1 and deliveries==1)");}
    round2_assert(!LuaMod::m_is_currently_executing_game_action,"concurrency left game flag set");
    std::cout << "ROUND2 PASS actual LoopAsync/ExecuteInGameThread vs " << (notify ? "notify" : "script") << " delivery\n";
}
static void round2_close(UE4SSProgram& program,const std::filesystem::path& scratch) {
    const auto path=scratch/L"DetachedClose";
    std::filesystem::create_directories(path/L"Scripts");std::ofstream(path/L"Scripts/main.lua")<<"started=true";
    LuaMod closing{program,StringType(STR("DetachedClose")),path.wstring()};Round2Preparation binding1{closing};
    LuaMod live{program,StringType(STR("DetachedLive")),path.wstring()};Round2Preparation binding2{live};
    struct Finish {LuaMod& closing;LuaMod& live;~Finish(){after_simple_actions_detached=nullptr;closing.uninstall();live.uninstall();}} finish{closing,live};
    closing.start_mod();const auto closing_ref=round2_reference;
    live.start_mod();const auto live_ref=round2_reference;
    round2_assert(closing.is_started() && live.is_started(),"close fixtures did not start");
    // Models another active host callback. Closing admission can be claimed,
    // but this pure lifetime pin prevents memory close until dispatch returns.
    std::optional<LuaHost::DetachedLuaWork> external{std::in_place,&closing.lua()};
    const auto closing_id=LuaMod::m_next_static_construct_callback_id++;
    const auto live_id=LuaMod::m_next_static_construct_callback_id++;
    {
        std::lock_guard guard{LuaMod::m_thread_actions_mutex};
        LuaMod::m_static_construct_object_lua_callbacks.push_back({closing_id,&closing.lua(),{},{},closing_ref,LUA_NOREF});
        LuaMod::m_static_construct_object_lua_callbacks.push_back({live_id,&live.lua(),{},{},live_ref,LUA_NOREF});
        LuaMod::m_pending_notify_on_new_object_callbacks.push_back({closing_id,-1,0});
        LuaMod::m_pending_notify_on_new_object_callbacks.push_back({live_id,-1,0});
    }
    gate_mod=&closing;
    after_simple_actions_detached=[] {gate_mod->uninstall();};
    std::vector<LuaMod::SimpleLuaAction> actions{{&closing.lua(),closing_ref,LUA_NOREF},{&live.lua(),live_ref,LUA_NOREF}};
    bool escaped=false;
    try {process_simple_actions(actions);} catch(...) {escaped=true;}
    after_simple_actions_detached=nullptr;
    round2_assert(closing.host_close_requested() && !closing.host_close_complete(),"close did not overlap detached work");
    round2_assert(!escaped,"closing cleanup escaped dispatcher");
    round2_assert(!LuaMod::m_is_currently_executing_game_action,"closing cleanup stranded shared executing flag");
    round2_assert(actions.empty(),"closing work was requeued");
    {LuaHost::ModLuaEntry entry{live.lua()};live.lua().execute_string("assert(deliveries==1)");}
    // Exercise delayed/frame pending-removal cleanup after closing as well.
    std::vector<LuaMod::DelayedGameThreadAction> delayed;
    LuaMod::DelayedGameThreadAction cancelled{&closing.lua(),closing_ref,LUA_NOREF};
    cancelled.status=LuaMod::DelayedActionStatus::PendingRemoval;cancelled.handle=123;
    delayed.push_back(cancelled);
    process_delayed_actions<GameThreadExecutionMethod::EngineTick>(delayed);
    round2_assert(delayed.empty() && !LuaMod::m_is_currently_executing_game_action,"frame cleanup failed cancellation");
    {
        std::vector<PinnedNotify> cancelled_notify;
        {std::lock_guard guard{LuaMod::m_thread_actions_mutex};
            for(const auto& value:LuaMod::m_static_construct_object_lua_callbacks)
                if(value.callback_id==closing_id) cancelled_notify.emplace_back(value);}
        bool entered=false;
        deliver_notify(cancelled_notify.front(),[&](const auto&){entered=true;});
        round2_assert(!entered,"closing notification entered Lua");
    }
    external.reset();closing.uninstall();
    round2_assert(closing.host_close_complete(),"detached pin prevented eventual close");
    {
        std::lock_guard guard{LuaMod::m_thread_actions_mutex};
        round2_assert(std::ranges::any_of(LuaMod::m_static_construct_object_lua_callbacks,[&](const auto& value){return value.callback_id==live_id;}),"unload removed another VM notify registration");
        round2_assert(LuaMod::m_pending_notify_on_new_object_callbacks.size()==1 && LuaMod::m_pending_notify_on_new_object_callbacks.front().callback_id==live_id,"unload cancelled another VM pending notify");
    }
    std::cout<<"ROUND2 PASS close during detached action: cancellation, live successor, flags, delayed/frame cleanup\n";
}
static void round2_gates(UE4SSProgram& program,const std::filesystem::path& scratch,int mode=0) {
    if(!mode || mode==1) round2_close(program,scratch);
    if(!mode || mode==2) round2_concurrent(program,scratch,false);
    if(!mode || mode==3) round2_concurrent(program,scratch,true);
    round2_assert(LuaHost::HostBindings::instance().vm_count()==0,"round2 VM pin leaked");
    std::cout<<"ROUND2 DISPATCH GATE PASS\n";
}
extern "C" __declspec(dllexport) int UE4SS_RunLuaModDispatchGate(const wchar_t*,int);
