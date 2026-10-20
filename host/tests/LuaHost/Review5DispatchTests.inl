// Review-5 regressions: real registered owner slots and real VM admission.
static bool review5_tick_available=true,review5_event_available=true;
static bool review5_tick_register=true,review5_event_register=true;
static bool review5_available(bool tick){return tick?review5_tick_available:review5_event_available;}
static bool review5_register(bool tick){return (tick?review5_tick_register:review5_event_register) && review4_register_dispatcher(tick);}
struct Review5Setup {
    Review5Setup(){
        if(offline_reset_shared_dispatchers) offline_reset_shared_dispatchers();
        review4_tick_slots.clear();review4_event_slots.clear();
        offline_dispatcher_available=review5_available;offline_shared_dispatcher_registration=review5_register;
        LuaMod::on_program_start();
    }
    ~Review5Setup(){
        offline_notify_argument=nullptr;offline_dispatcher_available=nullptr;offline_shared_dispatcher_registration=nullptr;
        review4_tick_slots.clear();review4_event_slots.clear();
        if(offline_reset_shared_dispatchers) offline_reset_shared_dispatchers();
    }
};
static unsigned review5_notifications;
static std::thread::id review5_callback_thread;
static int review5_note_thread(const LuaMadeSimple::Lua&){++review5_notifications;review5_callback_thread=std::this_thread::get_id();return 0;}
static void review5_worker_notify(){
    review5_tick_available=review5_event_available=review5_tick_register=review5_event_register=true;
    Review5Setup setup;Review4Vm vm{L"Review5WorkerNotify"};
    const auto gt=std::this_thread::get_id();review5_notifications=0;
    offline_notify_argument=[](const auto& lua){lua.set_nil();};
    {LuaHost::ModLuaEntry entry{vm.lua()};auto& lua=vm.lua();auto* root=lua.get_lua_state();
        lua.register_function("RecordThread",review5_note_thread);
        lua.execute_string("function notification(_) RecordThread(); return false end");
        lua_getglobal(root,"notification");const auto ref=luaL_ref(root,LUA_REGISTRYINDEX);
        std::lock_guard lock{LuaMod::m_thread_actions_mutex};
        LuaMod::m_static_construct_object_lua_callbacks.push_back({800001,&lua,{},{},ref,LUA_NOREF});
        LuaMod::m_pending_notify_on_new_object_callbacks.push_back({800001,1,123});
    }
    review4_check(review4_event_slots.size()==1,"ProcessEvent slot missing");
    // Unknown identity cannot be inferred from even a main-thread ProcessEvent.
    review5_set_game_thread(false);review4_event_slots.front()->Invoke();
    review4_check(review5_notifications==0 && LuaMod::m_pending_notify_on_new_object_callbacks.size()==1,
        "ProcessEvent drained notification before game-thread identity was established");
    review5_set_game_thread(true);
    bool invoked=false;std::thread worker{[&]{invoked=review4_event_slots.front()->Invoke();}};worker.join();
    review4_check(invoked,"worker slot delivery was revoked");
    review4_check(review5_notifications==0 && LuaMod::m_pending_notify_on_new_object_callbacks.size()==1,
        "worker ProcessEvent drained game-thread notification");
    review4_event_slots.front()->Invoke();review4_event_slots.front()->Invoke();
    review4_check(review5_notifications==1 && review5_callback_thread==gt && LuaMod::m_pending_notify_on_new_object_callbacks.empty(),
        "game-thread notification was not delivered exactly once");
    {std::lock_guard lock{LuaMod::m_thread_actions_mutex};std::erase_if(LuaMod::m_static_construct_object_lua_callbacks,[](const auto& value){return value.callback_id==800001;});}
    vm.close();std::cout<<"ROUND4 PASS registered ProcessEvent: unknown/worker leave queued; GT delivers once\n";
}
static void review5_fallback(bool prefer_tick){
    for(int scenario=0;scenario<6;++scenario){
        // Missing preferred, failed preferred, neither available, both failed,
        // and one unavailable plus one failed (precise per-hook diagnostics).
        review5_tick_available=review5_event_available=true;
        review5_tick_register=review5_event_register=true;
        if(scenario==0) (prefer_tick?review5_tick_available:review5_event_available)=false;
        if(scenario==1) (prefer_tick?review5_tick_register:review5_event_register)=false;
        if(scenario==2) review5_tick_available=review5_event_available=false;
        if(scenario==3) review5_tick_register=review5_event_register=false;
        if(scenario==4){(prefer_tick?review5_tick_available:review5_event_available)=false;(prefer_tick?review5_event_register:review5_tick_register)=false;}
        Review5Setup setup;Review4Vm vm{L"Review5Fallback"};
        {LuaHost::ModLuaEntry entry{vm.lua()};
            for(auto api:{"ExecuteInGameThreadWithDelay","RetriggerableExecuteInGameThreadWithDelay","LoopInGameThreadWithDelay"}){
                const auto preferred=prefer_tick?GameThreadExecutionMethod::EngineTick:GameThreadExecutionMethod::ProcessEvent;
                bool threw=false;std::string diagnostic;
                try{
                    auto selected=review5_select(vm.lua(),vm.mod(),api,preferred);
                    review4_check(scenario<2 || scenario==5,"unusable dispatcher selection succeeded");
                    review4_check((selected==preferred)==(scenario==5),"delayed API did not select the installed fallback");
                }catch(const std::exception& e){threw=true;diagnostic=e.what();}
                review4_check(threw==(scenario>=2 && scenario<5),"delayed API rejected available installed fallback");
                if(threw){
                    const std::string tick=review5_tick_available?"registration failed":"unavailable";
                    const std::string event=review5_event_available?"registration failed":"unavailable";
                    review4_check(diagnostic.find(std::string(api)+": No usable game-thread dispatcher (EngineTick: "+tick+", ProcessEvent: "+event+")")!=std::string::npos,
                        "dispatcher error did not distinguish unavailable from registration failed");
                }
            }
        }
        vm.close();
    }
    std::cout<<"ROUND4 PASS delayed APIs prefer "<<(prefer_tick?"EngineTick":"ProcessEvent")<<": all three fallback/registration/error cases\n";
}
static void review5_gates(int mode=0){
    if(!mode || mode==9) review5_worker_notify();
    if(!mode || mode==10) review5_fallback(true);
    if(!mode || mode==11) review5_fallback(false);
    review4_check(LuaHost::HostBindings::instance().vm_count()==0,"review5 VM pin leaked");
}
