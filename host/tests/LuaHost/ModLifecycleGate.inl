// Included in LuaMod.cpp so the gate delivers through the REAL action dispatcher,
// not a test reimplementation. CMake links the tiny EXE to the freshly built DLL.
static std::atomic<bool> gate_scope_rejected{};
class GateOutput final : public Output::OutputDevice {
public:
    bool has_optional_arg() const override { return false; }
    void receive(File::StringViewType message) const override {
        const auto text=to_string(message);
        if(text.find("SDK Lua access lacks a protected actual VM execution scope")!=std::string::npos || text.find("Lua wrapper has no proven cached root identity")!=std::string::npos) gate_scope_rejected=true;
        std::cerr << text;
    }
};
static LuaMod* gate_mod{};
static std::vector<LuaMod::SimpleLuaAction> gate_actions;
static unsigned gate_registrations{}, gate_engine_calls{};
static int gate_enqueue(const LuaMadeSimple::Lua& lua) {
    const int top=lua.get_stack_size();
    int function=0;
    for(int i=1;i<=top;++i) if(lua.is_function(i)) {function=i;break;}
    if(!function) lua.throw_error("gate registration requires a callback");
    lua_pushvalue(lua.get_lua_state(),function);
    const int ref=luaL_ref(lua.get_lua_state(),LUA_REGISTRYINDEX);
    gate_actions.push_back({&lua,ref,LUA_NOREF});
    ++gate_registrations;
    lua.set_integer(gate_registrations);lua.set_integer(gate_registrations);
    return 2;
}
static int gate_first_engine_call(const LuaMadeSimple::Lua&) {++gate_engine_calls;return 0;}
static int gate_on_unload(const LuaMadeSimple::Lua& lua) {
    lua_pushvalue(lua.get_lua_state(),1);
    gate_mod->set_on_unload_callback(luaL_ref(lua.get_lua_state(),LUA_REGISTRYINDEX));return 0;
}
static void gate_prepare(const LuaMadeSimple::Lua& lua) {
    // Registration inspects the actual stack just as real binding setup does.
    // It must inherit the host prepare scope, before any engine-only stubs.
    (void)lua.get_stack_size();
    for(auto name:{"RegisterConsoleCommandHandler","RegisterHook","ExecuteInGameThread","RegisterKeyBind",
                   "NotifyOnNewObject","LoopAsync","ExecuteInGameThreadAfterFrames"})
        lua.register_function(name,gate_enqueue);
    lua.register_function("IsInGameThread",gate_first_engine_call);
    lua.register_function("GateOnUnload",gate_on_unload);
    lua.execute_string("prepared=true; callbacks=0; unloaded=false; function CreateInvalidObject() return {IsValid=function() return false end} end; StaticFindObject=CreateInvalidObject");
}
#include "ModDispatchGate.inl"
static UE4SSProgram* review4_program;
static std::filesystem::path review4_scratch;
struct Review4Vm {
    std::unique_ptr<LuaMod> value;
    std::unique_ptr<Round2Preparation> preparation;
    explicit Review4Vm(const wchar_t* name) {
        const auto path=review4_scratch/name;
        std::filesystem::create_directories(path/L"Scripts");std::ofstream(path/L"Scripts/main.lua")<<"started=true";
        value=std::make_unique<LuaMod>(*review4_program,StringType(name),path.wstring());
        preparation=std::make_unique<Round2Preparation>(*value);value->start_mod();
        round2_assert(value->is_started(),"review4 mod startup failed");
    }
    ~Review4Vm(){close();}
    auto& lua(){return value->lua();}
    LuaMod* mod(){return value.get();}
    void close(){if(value && !value->host_close_complete()) value->uninstall();}
};
namespace Unreal::UnrealInitializer {extern std::thread::id GGameThreadId;extern std::atomic<bool> GGameThreadIdInitialized;extern std::atomic<bool> GGameThreadIdentityProducerInstalled;}
static void review5_set_game_thread(bool established){
    Unreal::UnrealInitializer::GGameThreadId=established?std::this_thread::get_id():std::thread::id{};
    Unreal::UnrealInitializer::GGameThreadIdInitialized.store(established,std::memory_order_release);
}
static void review6_set_identity_producer(bool installed){Unreal::UnrealInitializer::GGameThreadIdentityProducerInstalled.store(installed,std::memory_order_release);}
static void review6_publish_first_tick(){
    auto& id=Unreal::UnrealInitializer::GGameThreadId;
    if(id==std::thread::id{}) {
        id=std::this_thread::get_id();
        Unreal::UnrealInitializer::GGameThreadIdInitialized.store(id!=std::thread::id{},std::memory_order_release);
    }
}
static void review6_enqueue_simple(const LuaMadeSimple::Lua& lua,LuaMod* mod){queue_game_thread_action(lua,mod,GameThreadExecutionMethod::ProcessEvent,1);}
static GameThreadExecutionMethod review5_select(const LuaMadeSimple::Lua& lua,LuaMod*,const char* api,GameThreadExecutionMethod preferred){
    return offline_select_delayed_dispatcher(lua,api,preferred);
}
static bool review4_dispatch_console(bool global) {
    return dispatch_command_callbacks(global?LuaMod::m_global_command_lua_callbacks:LuaMod::m_custom_command_lua_pre_callbacks,
        STR("shared"),{STR("shared")},STR("shared"),[](const auto& lua){lua.set_nil();});
}
static void review4_deliver_dispatcher(bool tick) {
    Unreal::Hook::TCallbackIterationData<void> data{STR("Review4Dispatcher")};
    if(tick) engine_tick_hook(data,nullptr,0,false);else process_event_hook(data,nullptr,nullptr,nullptr);
}
#include "Review4DispatchTests.inl"
#include "Review5DispatchTests.inl"
#include "Review6DispatchTests.inl"
#include "RealBindingsGate.inl"

int run_lua_mod_lifecycle_gate(const wchar_t* candidate, bool expect_failure) {
    unsigned checks=0;
    const auto gate_check=[&](bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);};
    try {
        const std::filesystem::path base(candidate);
        const auto scratch=std::filesystem::temp_directory_path()/L"UE4SS-LuaMod-lifecycle-gate";
        std::filesystem::create_directories(scratch);
        Output::set_default_devices<GateOutput>();
        std::cerr << "LIFECYCLE installing host integration\n";
        UE4SSProgram program{UE4SSProgram::OfflineLuaGate{},scratch,scratch/L"GameRoot/Game/Binaries/Win64"};
        // Stock-shaped startup includes require, registration and Lua 5.4 libs.
        // The genuine stock ConsoleCommandsMod is tested too (engine calls stop at registration).
        const auto fixture=scratch/L"LifecycleFixture"/L"Scripts";
        std::filesystem::create_directories(fixture);
        std::ofstream(fixture/L"helper.lua") << "return 42";
        std::ofstream(fixture/L"main.lua") << R"(
assert(prepared and require('helper')==42)
local function cb() callbacks=callbacks+1 end
RegisterHook('/Script/Gate.Test:Call',cb)
ExecuteInGameThread(cb); RegisterKeyBind(1,cb); NotifyOnNewObject('Gate',cb); LoopAsync(1,cb)
GateOnUnload(function() unloaded=true; local f=io.open(os.getenv('TEMP')..'/UE4SS-LuaMod-lifecycle-gate/unloaded.txt','w'); f:write('closed'); f:close() end)
started=true
)";
        const auto unload_marker=scratch/L"unloaded.txt";
        std::filesystem::remove(unload_marker);
        for(const auto& [name,path]:std::vector<std::pair<StringType,std::filesystem::path>>{
            {STR("LifecycleFixture"),scratch/L"LifecycleFixture"},
            {STR("ConsoleCommandsMod"),base/L"source/RE-UE4SS/assets/Mods/ConsoleCommandsMod"},
            {STR("WitchItRuntime"),base/L"runtime-source/WitchItRuntime"}}) {
            std::cerr << "LIFECYCLE constructing " << to_string(name) << "\n";
            LuaMod mod{program,StringType(name),path.wstring()};gate_mod=&mod;
            struct PreparationBinding {
                const LuaMod* mod;
                explicit PreparationBinding(const LuaMod* value):mod(value) { offline_preparations.emplace(mod,gate_prepare); }
                ~PreparationBinding() { offline_preparations.erase(mod); }
            } preparation{&mod};
            gate_registrations=0;gate_engine_calls=0;gate_scope_rejected=false;gate_actions.clear();
            // Preserve the stock require path to shared UEHelpers (no engine call at load).
            program.get_mods_directories()[0]=base/L"source/RE-UE4SS/assets/Mods";
            std::cerr << "LIFECYCLE starting " << to_string(name) << "\n";
            mod.start_mod();
            const bool started=mod.is_started();
            if(expect_failure) {
                gate_check(!started && gate_scope_rejected,"negative control did not reproduce checked_lua_state rejection");
                mod.uninstall();gate_check(mod.host_close_complete(),"negative control VM did not close");
                std::cout << "BASELINE_REPRODUCED: real LuaMod startup refused missing actual VM scope\n";
                return 0;
            }
            if(!started) mod.uninstall();
            gate_check(started,"REAL LuaMod::start_mod failed");
            gate_check(mod.main_lua() && mod.async_lua(),"startup threads missing");
            gate_check(gate_registrations>0,"main.lua failed before callback registration");
            if(name==STR("WitchItRuntime")) gate_check(gate_registrations==1,"real WIR did not schedule first engine entry");
            // Do not grant the caller a scope: production ingress must establish it.
            if(name!=STR("ConsoleCommandsMod")) process_simple_actions(gate_actions);
            else {
                LuaHost::ModLuaEntry entry{mod.lua()};
                mod.lua().execute_string("assert(package.loaded.set and package.loaded.dump_object and package.loaded.summon_unloaded_assets)");
                for(auto& action:gate_actions) luaL_unref(action.lua->get_lua_state(),LUA_REGISTRYINDEX,action.lua_action_function_ref);
                gate_actions.clear();
            }
            gate_check(gate_actions.empty(),"production dispatcher did not drain callbacks");
            if(name==STR("LifecycleFixture")) {
                LuaHost::ModLuaEntry entry{mod.lua()};
                mod.lua().execute_string("assert(started and callbacks==5)");
                gate_check(lua_gettop(mod.main_lua()->get_lua_state())==0,"callback stack leaked");
            }
            if(name==STR("WitchItRuntime")) gate_check(gate_engine_calls==1,"real WIR callback did not reach first engine call");
            mod.uninstall();gate_check(mod.host_close_complete(),"real VM finalization did not complete");
            gate_check(!LuaMadeSimple::cached_root_lua_state(mod.get_lua_state()),"closed VM cache survived");
            bool refused=false;
            try { LuaHost::ModLuaEntry stale{mod.lua()};mod.lua().get_stack_size(); }
            catch(const std::exception&) {refused=true;}
            gate_check(refused,"closed VM was admitted again");
            std::cout << "LIFECYCLE PASS " << to_string(name) << "\n";
        }
        gate_check(std::filesystem::exists(unload_marker),"OnUnload callback was skipped");
        gate_check(LuaHost::HostBindings::instance().vm_count()==0,"VM lease/cache leaked");
        round2_gates(program,scratch);
        review4_program=&program;review4_scratch=scratch;review5_set_game_thread(true);review6_set_identity_producer(true);review4_gates();review5_gates();review6_gates();
        round6_gates(program,scratch,base);
        Unreal::Hook::FScriptNameFilter filter{{1,2,3,4}};
        gate_check(filter.Accepts(3) && !filter.Accepts(99),"script filter admission changed");
        constexpr unsigned calls=1000000;
        unsigned admitted=0;
        const auto begin=std::chrono::steady_clock::now();
        for(unsigned i=0;i<calls;++i) admitted+=filter.Accepts(static_cast<int>(i)+100);
        const auto nanos=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
        gate_check(admitted==0,"unrelated global script call admitted owner dispatch");
        std::cout << "GLOBAL_SCRIPT_FILTER " << double(nanos)/calls << " ns/rejected call; owner/VM dispatches=" << admitted << "\n";
        std::cout << checks << " real LuaMod startup/callback/unload assertions PASS\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "LIFECYCLE GATE FAIL: " << error.what() << "\n";return 1;
    }
}
extern "C" __declspec(dllexport) int UE4SS_RunLuaModLifecycleGate(const wchar_t* root, bool negative) {
    return run_lua_mod_lifecycle_gate(root,negative);
}

extern "C" __declspec(dllexport) int UE4SS_RunLuaModDispatchGate(const wchar_t* candidate,int mode) {
    try {
        Output::set_default_devices<GateOutput>();
        auto scratch=std::filesystem::temp_directory_path()/L"UE4SS-LuaMod-lifecycle-gate";
        UE4SSProgram program{UE4SSProgram::OfflineLuaGate{},scratch,scratch/L"GameRoot/Game/Binaries/Win64"};
        if(mode>=15) round6_gates(program,scratch,std::filesystem::path(candidate),mode);
        else if(mode>=4) {review4_program=&program;review4_scratch=scratch;review5_set_game_thread(true);review6_set_identity_producer(true);if(mode>=12) review6_gates(mode);else if(mode>=9) review5_gates(mode);else review4_gates(mode);}
        else round2_gates(program,scratch,mode);
        return 0;
    } catch(const std::exception& e) {std::cerr<<(mode>=15?"ROUND6 FAIL ":mode>=12?"ROUND5 FAIL ":mode>=9?"ROUND4 FAIL ":mode>=4?"ROUND3 FAIL ":"ROUND2 FAIL ")<<e.what()<<"\n";return 1;}
}
