// No new identity source: installed producer is independent of current ID.
static void review6_missing_producer(bool delayed_only=false){
    for(int scenario=0;scenario<4;++scenario){
        review5_tick_available=scenario>=2;review5_tick_register=scenario==3;
        review5_event_available=review5_event_register=true;
        Review5Setup setup;review6_set_identity_producer(false);review5_set_game_thread(false);
        Review4Vm vm{L"Review6MissingProducer"};
        {LuaHost::ModLuaEntry entry{vm.lua()};auto& lua=vm.lua();auto* root=lua.get_lua_state();
            lua.execute_string("function s() error('unreachable') end");
            // Simple default and explicit ProcessEvent share this queue boundary.
            for(int form=0;form<(delayed_only?0:2);++form){
                lua_settop(root,0);lua_getglobal(root,"s");bool refused=false;
                try{review6_enqueue_simple(lua,vm.mod());}catch(const std::exception& e){
                    refused=std::string(e.what()).find("ExecuteInGameThread: ProcessEvent game-thread identity producer not installed")!=std::string::npos;
                }
                lua_settop(root,0);
                review4_check(refused,"ProcessEvent accepted simple work without an identity producer");
            }
            for(auto api:{"ExecuteInGameThreadWithDelay","RetriggerableExecuteInGameThreadWithDelay","LoopInGameThreadWithDelay"}){
                for(auto preferred:{GameThreadExecutionMethod::EngineTick,GameThreadExecutionMethod::ProcessEvent}){
                    bool refused=false;auto selected=preferred;
                    try{selected=review5_select(lua,vm.mod(),api,preferred);}catch(const std::exception& e){
                        refused=std::string(e.what()).find("ProcessEvent: game-thread identity producer not installed")!=std::string::npos;
                    }
                    review4_check(scenario==3 ? !refused && selected==GameThreadExecutionMethod::EngineTick : refused,
                        "delayed selection accepted ProcessEvent without an identity producer");
                }
            }
        }
        review4_check(LuaMod::m_game_thread_actions.empty() && LuaMod::m_pending_game_thread_actions.empty() &&
            LuaMod::m_delayed_game_thread_actions.empty(),"rejected ProcessEvent work retained queue refs");
        for(int i=0;i<100;++i) review4_check(review4_event_slots.front()->Invoke(),"host ProcessEvent slot revoked");
        review4_check(!Unreal::IsGameThreadInitialized() && !LuaMod::m_is_processing_actions && !LuaMod::m_is_currently_executing_game_action,
            "missing producer invented identity or stranded flags");
        vm.close();
    }
    review6_set_identity_producer(true);review5_set_game_thread(true);
    std::cout<<"ROUND5 PASS missing identity producer: disabled/unavailable/detour/initializer failure reject ProcessEvent; tick fallback preserved\n";
}
static void review6_waiting_and_publication(){
    for(bool tick_dispatcher:{true,false}) {
    review5_tick_available=review5_event_available=review5_event_register=true;review5_tick_register=tick_dispatcher;
    Review5Setup setup;review6_set_identity_producer(true);review5_set_game_thread(false);
    Review4Vm vm{L"Review6FirstTick"};offline_notify_argument=[](const auto& lua){lua.set_nil();};
    {LuaHost::ModLuaEntry entry{vm.lua()};auto& lua=vm.lua();auto* root=lua.get_lua_state();
        lua.execute_string("simple=0; delayed=0; notify=0; function s() simple=simple+1 end; function d() delayed=delayed+1 end; function n(_) notify=notify+1; return false end");
        lua_settop(root,0);lua_getglobal(root,"s");review6_enqueue_simple(lua,vm.mod());lua_settop(root,0);
        auto selected=review5_select(lua,vm.mod(),"ExecuteInGameThreadWithDelay",GameThreadExecutionMethod::ProcessEvent);
        review4_check(selected==GameThreadExecutionMethod::ProcessEvent,"installed producer did not allow pre-first-tick waiting");
        auto getref=[&](const char* name){lua_getglobal(root,name);return luaL_ref(root,LUA_REGISTRYINDEX);};
        LuaMod::DelayedGameThreadAction action{};action.lua=&lua;action.lua_action_function_ref=getref("d");action.lua_action_thread_ref=LUA_NOREF;action.method=selected;action.handle=600001;
        auto notify=getref("n");
        std::lock_guard lock{LuaMod::m_thread_actions_mutex};LuaMod::m_delayed_game_thread_actions.push_back(action);
        LuaMod::m_static_construct_object_lua_callbacks.push_back({600001,&lua,{},{},notify,LUA_NOREF});
        LuaMod::m_pending_notify_on_new_object_callbacks.push_back({600001,1,123});
    }
    for(int i=0;i<100;++i) review4_event_slots.front()->Invoke();
    review4_check(LuaMod::m_game_thread_actions.size()==1 && LuaMod::m_delayed_game_thread_actions.size()==1 &&
        LuaMod::m_pending_notify_on_new_object_callbacks.size()==1,"pre-first-tick delivery consumed queued work");
    std::atomic<bool> ready{},publish{};bool worker_ok=true;
    std::thread worker{[&]{ready.store(true,std::memory_order_release);while(!publish.load(std::memory_order_acquire)) std::this_thread::yield();
        for(int i=0;i<100000;++i){
            if(Unreal::IsInGameThreadRaw()) worker_ok=false;
            (void)Unreal::GetGameThreadIdRaw();
            if(Unreal::IsGameThreadInitialized()) (void)Unreal::GetGameThreadId();
            if(!review4_event_slots.front()->Invoke()) worker_ok=false;
        }
    }};
    while(!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    publish.store(true,std::memory_order_release);review6_publish_first_tick();worker.join();
    review4_check(worker_ok && Unreal::IsInGameThreadRaw(),"first-tick/worker identity publication failed");
    if(tick_dispatcher) review4_check(review4_tick_slots.front()->Invoke(),"trusted tick slot missing");
    else review4_check(review4_tick_slots.empty(),"test unexpectedly installed Lua tick dispatcher");
    {LuaHost::ModLuaEntry entry{vm.lua()};vm.lua().execute_string(tick_dispatcher?"assert(simple==0 and delayed==0 and notify==1)":"assert(simple==0 and delayed==0 and notify==0)");}
    review4_event_slots.front()->Invoke();review4_event_slots.front()->Invoke();
    {LuaHost::ModLuaEntry entry{vm.lua()};vm.lua().execute_string("assert(simple==1 and delayed==1 and notify==1)");}
    {std::lock_guard lock{LuaMod::m_thread_actions_mutex};std::erase_if(LuaMod::m_static_construct_object_lua_callbacks,[](const auto& x){return x.callback_id==600001;});}
    vm.close();
    }
    std::cout<<"ROUND5 PASS installed producer: pre-first-tick waits; concurrent worker rejected; trusted tick then delivers once\n";
}
static void review6_gates(int mode=0){
    if(!mode || mode==12) review6_missing_producer();
    if(mode==14) review6_missing_producer(true);
    if(!mode || mode==13) review6_waiting_and_publication();
    review4_check(LuaHost::HostBindings::instance().vm_count()==0,"review6 VM pin leaked");
}
