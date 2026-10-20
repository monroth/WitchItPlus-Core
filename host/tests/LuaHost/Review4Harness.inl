#include <LuaHost/DetachedLuaWork.hpp>
#include <algorithm>
#include <File/Macros.hpp>
#include <Helpers/String.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <mutex>
#include <unordered_map>
#include <optional>
namespace review4_tests {
enum class GameThreadExecutionMethod {EngineTick,ProcessEvent};
struct LuaMod {
    struct SimpleLuaAction {const LuaMadeSimple::Lua* lua;int32_t lua_action_function_ref{},lua_action_thread_ref{};};
    struct LuaCallbackData {struct RegistryIndex {int32_t lua_index{},identifier{};};const LuaMadeSimple::Lua* lua;void* instance_of_class;std::vector<std::pair<const LuaMadeSimple::Lua*,RegistryIndex>> registry_indexes;bool scheduled_for_removal{};};
    struct LuaCancellableCallbackData {uint64_t callback_id{};const LuaMadeSimple::Lua* lua;int name{},outer{};int32_t lua_callback_function_ref{},lua_callback_thread_ref{};};
    static inline std::vector<LuaCancellableCallbackData> m_static_construct_object_lua_callbacks;
    static inline std::vector<SimpleLuaAction> m_engine_tick_actions,m_game_thread_actions;
    static inline std::unordered_map<File::StringType,LuaCallbackData> m_global_command_lua_callbacks,m_custom_command_lua_pre_callbacks;
    static inline std::recursive_mutex m_thread_actions_mutex;
    static inline bool m_is_currently_executing_game_action{};
private:
    static inline bool m_is_engine_tick_hooked{};
    bool m_is_process_event_hooked{};
    static void ensure_engine_tick_hooked();static void ensure_process_event_hooked(LuaMod*);
public:
    static void on_program_start();
};
#include "Review4ProductionDispatch.inl"
static void engine_tick_hook(){process_simple_actions(LuaMod::m_engine_tick_actions);}
static void process_event_hook(){process_simple_actions(LuaMod::m_game_thread_actions);}
static bool is_engine_tick_hook_available(){return true;}
static bool is_process_event_hook_available(){return true;}
namespace Unreal::Hook {
    using RC::Unreal::Hook::Internal::TCallbackSlot;
    namespace Internal=RC::Unreal::Hook::Internal;
    inline constexpr unsigned ERROR_ID=0;
    struct Options {bool once,nested;File::StringType owner,description;};
    static unsigned RegisterEngineTickPreCallback(void(*)(),Options);
    static unsigned RegisterProcessEventPreCallback(void(*)(),Options);
}
namespace Unreal {static bool IsGameThreadIdentityProducerInstalled(){return true;}}
#include "Review4SharedDispatch.inl"
namespace Unreal::Hook {
    static unsigned RegisterEngineTickPreCallback(void(*)(),Options){return offline_shared_dispatcher_registration(true)?1:0;}
    static unsigned RegisterProcessEventPreCallback(void(*)(),Options){return offline_shared_dispatcher_registration(false)?1:0;}
}
#include "Review4ConsoleDispatch.inl"
struct Review4Vm {
    LuaMadeSimple::Lua& value;
    VmHandle vm;
    LuaMod instance;
    bool closed{};
    explicit Review4Vm(const wchar_t*):value(LuaMadeSimple::new_state()) {
        auto result=HostBindings::instance().attach_vm(value.get_lua_state());CHECK(result.status==Status::ok);vm=result.value;
        ModLuaEntry entry{value};value.open_all_libs();
    }
    ~Review4Vm(){close();}
    auto& lua(){return value;}
    LuaMod* mod(){return &instance;}
    void close(){
        if(closed) return;
        // Model only LuaMod's record erasure; real lifecycle/broker/cache/VM close.
        for(auto* container:{&LuaMod::m_global_command_lua_callbacks,&LuaMod::m_custom_command_lua_pre_callbacks}) {
            std::lock_guard lock{LuaMod::m_thread_actions_mutex};
            std::erase_if(*container,[&](const auto& item){return item.second.lua==&value;});
        }
        auto& host=HostBindings::instance();auto* root=value.get_lua_state();
        CHECK(host.claim_vm_close(vm)==Status::ready);CHECK(host.cleanup_vm(vm)==Status::ok);
        LuaMadeSimple::LuaFinalizationScope closing{root,root};lua_close(root);closing.close_completed();CHECK(host.finish_vm_close(vm)==Status::ok);closed=true;
    }
};
static void review4_deliver_dispatcher(bool tick){if(tick) engine_tick_hook();else process_event_hook();}
#include "Review4DispatchTests.inl"
static int run(int mode) {
    try {review4_gates(mode);std::cout<<"ROUND3 ACTUAL STACK PASS\n";return 0;}
    catch(const std::exception& e){std::cerr<<"ROUND3 FAIL "<<e.what()<<"\n";return 1;}
}
}
