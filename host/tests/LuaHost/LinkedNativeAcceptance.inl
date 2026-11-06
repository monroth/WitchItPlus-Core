namespace {
struct Native4Program final:UE4SSProgram {
 using UE4SSProgram::UE4SSProgram;using UE4SSProgram::setup_mods;
};
static void native4_prepare(const LuaMadeSimple::Lua& lua) {
 gate_prepare(lua);
 lua.register_function("Native4ErrorA",+[](const LuaMadeSimple::Lua&)->int{throw std::runtime_error("native4 SDK first exact reason");});
 lua.register_function("Native4ErrorB",+[](const LuaMadeSimple::Lua&)->int{throw std::runtime_error("native4 SDK second distinct reason");});
}
}
extern "C" __declspec(dllexport) int UE4SS_RunNative4Linked(const wchar_t* scratch_dir,const wchar_t* producer,const wchar_t* counted,bool negative) {
 try {
  const std::filesystem::path scratch{scratch_dir};std::filesystem::create_directories(scratch);
  const auto mods=scratch/L"Mods";
  for(auto [name,image]:{std::pair{L"WitchItNativeProducer",producer},std::pair{L"Native4Counted",counted}}){
   const auto path=mods/name/L"dlls";std::filesystem::create_directories(path);
   std::filesystem::copy_file(image,path/L"main.dll",std::filesystem::copy_options::overwrite_existing);
  }
  // This is a disposable no-engine process. Keep engine-only listeners disabled
  // while exercising the actual Producer's scalar/primitives and teardown paths.
  std::array<wchar_t,32768> executable{};auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
  round2_assert(length && length<executable.size(),"test executable path unavailable");
  const auto control=std::filesystem::path(executable.data()).parent_path()/L"ue4ss/Mods/Core";
  std::filesystem::create_directories(control);
  std::ofstream(control/L"source-geometry-debug.json")<<R"({"producer_disable":["writer_safety","save_load_startup","viewport","travel","local_world","ship_binding","file_bridge","schemas","clipboard","update_file"]})";
  Output::set_default_devices<GateOutput>();
  Native4Program program{UE4SSProgram::OfflineLuaGate{},mods,scratch/L"Game/Binaries/Win64"};
  review5_set_game_thread(true);review6_set_identity_producer(true);
  program.setup_mods();
  std::vector<std::shared_ptr<CppMod>> native;
  for(auto& value:program.get_mods_snapshot())if(auto mod=std::dynamic_pointer_cast<CppMod>(value)){mod->start_mod();round2_assert(mod->is_started(),"real native DLL failed startup");native.push_back(mod);}
  round2_assert(native.size()==2,"expected real Producer and counted-executor module");
  const auto runtime=scratch/L"RuntimeFixture";std::filesystem::create_directories(runtime/L"Scripts");
  std::ofstream(runtime/L"Scripts/main.lua")<<R"(
for _,v in ipairs{{Native4ErrorA,'native4 SDK first exact reason'},{Native4ErrorB,'native4 SDK second distinct reason'}} do
 -- The protected Lua boundary appends a traceback after the original message.
 local ok,why=pcall(v[1]);local message=tostring(why):match('^[^\r\n]*')
 assert(not ok and message==v[2],tostring(why))
end
sdk_errors_verified=true
)";
  for(unsigned cycle=0;cycle<(negative?1u:25u);++cycle){
   LuaMod lua{program,StringType(L"WitchItRuntime"),runtime.wstring()};
   struct Retirement {
    LuaMod& lua;
    ~Retirement(){offline_preparations.erase(&lua);lua.uninstall();}
   } retirement{lua};
   offline_preparations.emplace(&lua,native4_prepare);lua.start_mod();
   round2_assert(lua.is_started(),"real LuaMod failed startup");
   {
    LuaHost::ModLuaEntry entry{lua.lua()};lua.lua().execute_string("assert(sdk_errors_verified)");
    for(auto& mod:native)mod->fire_on_lua_start(L"WitchItRuntime",const_cast<LuaMadeSimple::Lua&>(lua.lua()),*lua.m_main_lua,*lua.m_async_lua,lua.m_hook_lua);
    lua.lua().execute_string("assert(WitchItNativeDebug('native_source_hooks,native_object_listener'));local native=WitchItNativePrimitives();assert(type(native)=='function','real Producer primitives unavailable')");
   }
   offline_preparations.erase(&lua); // stop uses the production registry and gate/drain path
   const auto begin=std::chrono::steady_clock::now();
   if(negative){LuaHost::ModLuaEntry held{lua.lua()};lua.uninstall();throw std::runtime_error("negative held-gate control unexpectedly completed");}
   lua.uninstall();round2_assert(lua.host_close_complete(),"native drain did not permit VM close");
   round2_assert(std::chrono::steady_clock::now()-begin<std::chrono::seconds(5),"native stop exceeded 5s");
   std::cout<<"NATIVE4 LINKED cycle="<<cycle<<" SDK=2 executors=4 stop/reload=PASS\n"<<std::flush;
  }
  for(auto& mod:native){mod->uninstall();round2_assert(mod->host_retirement_complete(),"real CppMod unload incomplete");}
  std::cout<<"NATIVE4 LINKED PASS real host/Producer, 25 VM generations, 100 counted completions; SDK reasons exact\n"<<std::flush;
  return 0;
 }catch(const std::exception& error){std::cerr<<"NATIVE4 LINKED FAIL "<<error.what()<<"\n";return 1;}
}
