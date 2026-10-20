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
        // Status of a delayed action (mirrors UE's ETimerStatus)
        enum class DelayedActionStatus : uint8_t
        {
            Pending,        // Created but not yet started
            Active,         // Running, waiting for delay to expire
            Paused,         // Timer paused, will resume when unpaused
            Executing,      // Currently executing callback
            PendingRemoval  // Marked for removal, will be cleaned up
        };

        struct DelayedGameThreadAction
        {
            const LuaMadeSimple::Lua* lua;
            int32_t lua_action_function_ref{};
            int32_t lua_action_thread_ref{};
            GameThreadExecutionMethod method{GameThreadExecutionMethod::EngineTick};
            DelayedActionStatus status{DelayedActionStatus::Active};
            std::chrono::steady_clock::time_point execute_at{};  // Absolute time when action should execute
            int64_t time_remaining_ms{0};  // Time remaining when paused (milliseconds)
            int64_t frames_remaining{0};  // Countdown for frame-based delays
            int64_t delay_ms{0};  // Original delay in milliseconds (for loop/reset)
            int64_t delay_frames{0};  // Original delay in frames (0 means use time-based delay)
            int64_t handle{0};  // Unique handle for this action
            bool is_retriggerable{false};  // If true, can be reset by calling with same handle
            bool is_looping{false};  // If true, re-schedule after each execution
            bool pause_after_execution{false};  // Pause immediately after current callback returns
        };

static inline std::vector<DelayedGameThreadAction> m_delayed_game_thread_actions,m_pending_delayed_game_thread_actions;
static inline std::vector<SimpleLuaAction> m_pending_engine_tick_actions,m_pending_game_thread_actions;
static inline bool m_is_processing_actions{};
struct PendingNotifyOnNewObjectCallback {uint64_t callback_id;int32_t object_index,object_serial;};
static inline std::vector<PendingNotifyOnNewObjectCallback> m_pending_notify_on_new_object_callbacks;

private:
    static inline bool m_is_engine_tick_hooked{};
    bool m_is_process_event_hooked{};
    static void ensure_engine_tick_hooked();static void ensure_process_event_hooked(LuaMod*);
public:
    static void on_program_start();
    static GameThreadExecutionMethod review5_select_api(const LuaMadeSimple::Lua&,LuaMod*,const char*,GameThreadExecutionMethod);
};
namespace Unreal {
static bool IsGameThreadIdentityProducerInstalled(){return true;}
static std::thread::id game_thread;
static bool IsInGameThreadRaw(){return game_thread!=std::thread::id{} && game_thread==std::this_thread::get_id();}
struct UObject {};struct UFunction {};struct UEngine {};
struct FUObjectItem {int GetSerialNumber(){return 123;} UObject* GetUObject(){static UObject value;return &value;}};
struct FUObjectArray {static FUObjectItem* IndexToObject(int index){static FUObjectItem item;return index==1?&item:nullptr;}};
namespace Hook {template<class T> struct TCallbackIterationData {};}
}
namespace LuaType {static void auto_construct_object(const LuaMadeSimple::Lua& lua,Unreal::UObject*){lua.set_nil();}}
static void review5_set_game_thread(bool established){Unreal::game_thread=established?std::this_thread::get_id():std::thread::id{};}

#include "Review5ProductionDispatch.inl"
static bool (*offline_shared_dispatcher_registration)(bool){};
static bool (*offline_dispatcher_available)(bool){};
static bool is_engine_tick_hook_available(){return !offline_dispatcher_available || offline_dispatcher_available(true);}
static bool is_process_event_hook_available(){return !offline_dispatcher_available || offline_dispatcher_available(false);}
namespace Unreal::Hook {
    using RC::Unreal::Hook::Internal::TCallbackSlot;
    namespace Internal=RC::Unreal::Hook::Internal;
    inline constexpr unsigned ERROR_ID=0;
    struct Options {bool once,nested;File::StringType owner,description;};
    template<class F> static unsigned RegisterEngineTickPreCallback(F,Options){return offline_shared_dispatcher_registration(true)?1:0;}
    template<class F> static unsigned RegisterProcessEventPreCallback(F,Options){return offline_shared_dispatcher_registration(false)?1:0;}
}
#include "Review5SharedDispatch.inl"
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
static void review4_deliver_dispatcher(bool tick){Unreal::Hook::TCallbackIterationData<void> data;if(tick) engine_tick_hook(data,nullptr,0,false);else process_event_hook(data,nullptr,nullptr,nullptr);}
#include "Review4DispatchTests.inl"
#include "Review5Selection.inl"
static GameThreadExecutionMethod review5_select(const LuaMadeSimple::Lua& lua,LuaMod* mod,const char* api,GameThreadExecutionMethod preferred){return LuaMod::review5_select_api(lua,mod,api,preferred);}
#include "Review5DispatchTests.inl"
static int run(int mode) {
    try {review5_set_game_thread(true);if(mode>=9) review5_gates(mode);else review4_gates(mode);std::cout<<"ROUND4 ACTUAL STACK PASS\n";return 0;}
    catch(const std::exception& e){std::cerr<<"ROUND4 FAIL "<<e.what()<<"\n";return 1;}
}
}
