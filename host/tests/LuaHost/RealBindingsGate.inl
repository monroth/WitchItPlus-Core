// Real production preparation and bindings. Only engine lookup/patching is
// replaced; directory traversal, table/userdata dispatch and queues are real.
static std::filesystem::path round6_source,round6_fixture,round6_unload_path;
static int round6_wrapper(const LuaMadeSimple::Lua& lua) {
    const auto name=lua.get_string();
#define R6_REMOTE(Name) if(name==#Name){LuaType::Name::construct(lua,nullptr);return 1;}
    R6_REMOTE(UObject) R6_REMOTE(UClass) R6_REMOTE(UStruct)
    R6_REMOTE(UEnum) R6_REMOTE(UWorld) R6_REMOTE(AActor) R6_REMOTE(UInterface)
    R6_REMOTE(UDataTable) R6_REMOTE(FOutputDevice) R6_REMOTE(XProperty)
    R6_REMOTE(XArrayProperty) R6_REMOTE(XBoolProperty) R6_REMOTE(XEnumProperty) R6_REMOTE(XInterfaceProperty)
    R6_REMOTE(XObjectProperty) R6_REMOTE(XStructProperty) R6_REMOTE(XDelegateProperty)
    R6_REMOTE(XMulticastDelegateProperty) R6_REMOTE(XMulticastSparseDelegateProperty)
#undef R6_REMOTE
    if(name=="UFunction"){LuaType::UFunction::construct(lua,nullptr,nullptr);return 1;}
    if(name=="XFieldClass"){LuaType::XFieldClass::construct(lua,Unreal::FFieldClassVariant{});return 1;}
    if(name=="RemoteUnrealParam"){LuaType::RemoteUnrealParam::construct(lua,nullptr,Unreal::FName(int64_t{}));return 1;}
    if(name=="LocalUnrealParam"){LuaType::ScriptStructWrapper value{};LuaType::LocalUnrealParam<LuaType::ScriptStructWrapper>::construct(lua,value,nullptr,nullptr);return 1;}
    if(name=="FName"){LuaType::FName::construct(lua,Unreal::FName(int64_t{}));return 1;}
    if(name=="FText"){LuaType::FText::construct(lua,Unreal::FText{});return 1;}
    if(name=="FString"){Unreal::FString value{};LuaType::FString::construct(lua,&value);return 1;}
    if(name=="FUtf8String"){Unreal::FUtf8String value{};LuaType::FUtf8String::construct(lua,&value);return 1;}
    if(name=="FAnsiString"){Unreal::FAnsiString value{};LuaType::FAnsiString::construct(lua,&value);return 1;}
    if(name=="FURL"){LuaType::FURL::construct(lua,Unreal::FURL{});return 1;}
    if(name=="FWeakObjectPtr"){LuaType::FWeakObjectPtr::construct(lua,Unreal::FWeakObjectPtr{});return 1;}
    if(name=="TSoftObjectPtr"){Unreal::FSoftObjectPtr value{};LuaType::TSoftObjectPtr::construct(lua,value);return 1;}
    if(name=="FSoftObjectPath"){Unreal::FSoftObjectPath value{};LuaType::FSoftObjectPath::construct(lua,value);return 1;}
    if(name=="UScriptStruct"){LuaType::ScriptStructWrapper value{};LuaType::UScriptStruct::construct(lua,value);return 1;}
    if(name=="RemoteObject"){LuaMadeSimple::Type::RemoteObject<int>::construct(lua,nullptr);return 1;}
    lua.throw_error("Unknown no-engine wrapper");return 0;
}
static int round6_engine_name(const LuaMadeSimple::Lua& lua) {
    // Replace name-pool lookup only. Emit a genuine FName wrapper, not a Lua table.
    LuaType::FName::construct(lua,Unreal::FName(int64_t{}));return 1;
}
static int round6_engine_find(const LuaMadeSimple::Lua& lua) {LuaType::auto_construct_object(lua,nullptr);return 1;}
static void round6_prepare(const LuaMadeSimple::Lua& lua) {
    // The actual Object factory/getter with a non-dereferenced engine address.
    // Only GetAddress/expired IsValid run; no fake Unreal object is reflected.
    lua.register_function("GateLifetimeObject",[](const LuaMadeSimple::Lua& value)->int {
        LuaType::UObject::construct(value,reinterpret_cast<Unreal::UObject*>(0x12340));return 1;
    });
    lua.register_function("GateLifetimeDelete",[](const LuaMadeSimple::Lua&)->int {
        LuaType::remove_from_global_unreal_objects_map(reinterpret_cast<Unreal::UObject*>(0x12340));return 0;
    });
    lua.register_function("GateWrapper",round6_wrapper);
    lua.set_string(to_utf8_string(round6_unload_path));lua_setglobal(lua.get_lua_state(),"GateUnloadPath");
    lua.register_function("GateInvoke",[](const LuaMadeSimple::Lua& value)->int {value.call_function(0,0);return 0;});
    lua.set_string(to_utf8_string(round6_source));lua_setglobal(lua.get_lua_state(),"GateSource");
    lua.set_string(to_utf8_string(round6_fixture));lua_setglobal(lua.get_lua_state(),"GateFixture");
    LuaType::FName::construct(lua,Unreal::FName(int64_t{}));lua_setglobal(lua.get_lua_state(),"NAME_None");
    lua.register_function("FName",round6_engine_name);
    lua.register_function("FindFirstOf",round6_engine_find);
    lua.register_function("StaticFindObject",round6_engine_find);
}
struct Round6Preparation {
    LuaMod& mod;
    explicit Round6Preparation(LuaMod& value):mod(value){offline_real_bindings.insert(&mod);offline_preparations.emplace(&mod,round6_prepare);}
    ~Round6Preparation(){mod.uninstall();offline_preparations.erase(&mod);offline_real_bindings.erase(&mod);}
};
static void round6_write(const std::filesystem::path& path,std::string_view body) {
    std::filesystem::create_directories(path.parent_path());std::ofstream stream(path,std::ios::binary);stream<<body;
    if(!stream) throw std::runtime_error("ROUND6 FAIL fixture write");
}
static void round6_assert(LuaMod& mod,std::string_view code) {
    LuaHost::ModLuaEntry entry{mod.lua()};mod.lua().execute_string(std::string(code));
}
static void round6_gates(UE4SSProgram& program,const std::filesystem::path& scratch,const std::filesystem::path& source,int mode=0) {
    round6_source=source;round6_fixture=scratch/L"GameRoot";
    const auto content=round6_fixture/L"Game/Content/Paks/LogicMods";
    const auto mods=round6_fixture/L"Game/Binaries/Win64/ue4ss/Mods";
    round6_write(content/L"Example/config.lua","Mods.Example={AssetName='ExampleActor_C',AssetPath='/Game/Mods/Example/ModActor'}");
    round6_write(content/L"example.pak","");std::filesystem::create_directories(content/L"Empty/Deep");
    round6_write(mods/L"ExamplePackage/manifest.lua","return {}");
    round6_write(mods/L"SecondPackage/manifest.lua","return {}");
    round6_write(mods/L"\u00dcnicodePackage/manifest.lua","return {}");
    round6_write(mods/L"UnsafePackage/manifest.lua","return {}");std::filesystem::create_directories(mods/L"UnsafePackage/Scripts");
    std::filesystem::create_directories(mods/L"EmptyPackage");
    std::filesystem::copy(source/L"runtime-source/WitchItRuntime",mods/L"WitchItRuntime",
        std::filesystem::copy_options::recursive|std::filesystem::copy_options::overwrite_existing);
    review5_tick_available=review5_event_available=review5_tick_register=review5_event_register=true;
    Review5Setup setup;review5_set_game_thread(true);review6_set_identity_producer(true);
    program.get_mods_directories()[0]=source/L"source/RE-UE4SS/assets/Mods";
    const auto tests=source/L"source/RE-UE4SS/tests/LuaHost/RealBindings";
    for(int which: mode==15?std::vector<int>{15}:mode==16?std::vector<int>{16}:std::vector<int>{15,16,17}) {
        const auto path=mods/(which==15?L"RealBindingsMain":which==16?L"RealBindingsCoroutine":L"RealBindingsCallbacks");
        std::filesystem::create_directories(path/L"Scripts");
        std::filesystem::copy_file(tests/(which==15?L"main.lua":which==16?L"coroutine.lua":L"callbacks.lua"),path/L"Scripts/main.lua",std::filesystem::copy_options::overwrite_existing);
        LuaMod mod{program,STR("RealBindings"),path.wstring()};Round6Preparation preparation{mod};gate_scope_rejected=false;
        round6_unload_path=path/L"unload.ok";std::filesystem::remove(round6_unload_path);
        mod.start_mod();
        {LuaHost::ModLuaEntry entry{mod.lua()};lua_getglobal(mod.lua().get_lua_state(),"main_done");const bool done=lua_toboolean(mod.lua().get_lua_state(),-1);lua_pop(mod.lua().get_lua_state(),1);
            review4_check(done,which==16?"real coroutine metadata script incomplete":"real main metadata script incomplete");}
        review4_check(mod.is_started(),"ROUND6 FAIL real preparation/start failed");
        if(which==17){
            // These are actual registered-slot dispatches, with no caller Lua scope.
            review4_event_slots.front()->Invoke();review4_tick_slots.front()->Invoke();review4_tick_slots.front()->Invoke();
            for(int i=0;i<1000;++i){
                bool done=false;{LuaHost::ModLuaEntry entry{mod.lua()};mod.lua().execute_string("GateAsyncDone=(async_count==3)");lua_getglobal(mod.lua().get_lua_state(),"GateAsyncDone");done=lua_toboolean(mod.lua().get_lua_state(),-1);lua_pop(mod.lua().get_lua_state(),1);}
                if(done)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            round6_assert(mod,"assert(main_done and event_count==1 and tick_count==1 and frame_count==1 and async_count==3,'ROUND6 real queue callback counts'); assert(IsInGameThread() and not IsInAsyncThread())");
        } else round6_assert(mod,"assert(main_done,'ROUND6 script did not complete')");
        review4_check(!gate_scope_rejected,"ROUND6 FAIL unprotected actual-stack SDK access");
        if(which==17) {
            round6_assert(mod,"local old=GateLifetimeObject(); local address=old:GetAddress(); GateLifetimeDelete(); assert(not old:IsValid()); local fresh=GateLifetimeObject(); assert(fresh:GetAddress()==address); assert(not old:IsValid(),'expired object revived at reused address'); local ok,why=pcall(function() return old:GetAddress() end); assert(not ok,'expired object address remained accessible: '..tostring(why)); GateLifetimeDelete()");
            std::cout<<"OBJECT PASS actual wrapper deletion/address reuse\n";
            review4_check(dispatch_command_callbacks(LuaMod::m_custom_command_lua_pre_callbacks,STR("round6"),{STR("round6")},STR("round6"),[](const auto& value){value.set_nil();}),"ROUND6 FAIL console delivery");
            round6_assert(mod,"assert(command_count==1,'ROUND6 command callback count')");
        }
        mod.uninstall();review4_check(mod.host_close_complete(),"ROUND6 FAIL close did not finalize");
        review4_check(!gate_scope_rejected,"ROUND6 FAIL unload/GC admission error");
        review4_check(std::filesystem::exists(path/L"unload.ok"),"ROUND6 FAIL real OnUnload did not complete");
        std::cout<<"ROUND6 PASS real bindings "<<(which==15?"main":which==16?"coroutine":"callbacks")<<"\n";
    }
    if(mode==15 || mode==16) return;
    const auto bp=mods/L"BPModLoaderMod";
    std::filesystem::create_directories(bp/L"Scripts");std::filesystem::copy_file(source/L"source/RE-UE4SS/assets/Mods/BPModLoaderMod/Scripts/main.lua",bp/L"Scripts/main.lua",std::filesystem::copy_options::overwrite_existing);
    LuaMod mod{program,STR("BPModLoaderMod"),bp.wstring()};Round6Preparation preparation{mod};gate_scope_rejected=false;
    mod.start_mod();round6_assert(mod,"assert(type(Mods)=='table' and Mods.Example.AssetName=='ExampleActor_C' and Mods.example.AssetName=='ModActor_C','ROUND6 genuine BPModLoader config/discovery failed')");
    review4_tick_slots.front()->Invoke(); // genuine BP loader's real ExecuteInGameThread queue
    review4_check(!gate_scope_rejected,"ROUND6 FAIL genuine BPModLoader SDK admission");
    mod.uninstall();review4_check(mod.host_close_complete(),"ROUND6 FAIL BP loader close");
    std::cout<<"ROUND6 PASS genuine BPModLoader main.lua and real WitchItRuntime discovery\n";
    review4_check(LuaHost::HostBindings::instance().vm_count()==0,"ROUND6 FAIL VM leak");
}
