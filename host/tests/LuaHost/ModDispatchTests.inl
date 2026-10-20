#include <LuaHost/DetachedLuaWork.hpp>
#include <ExceptionHandling.hpp>
#include <atomic>
#include <thread>
#include <optional>
namespace dispatch_tests {
struct LuaMod {
    struct SimpleLuaAction {const LuaMadeSimple::Lua* lua;int32_t lua_action_function_ref{},lua_action_thread_ref{};};
    struct LuaCallbackData {struct RegistryIndex {int32_t lua_index{},identifier{};};};
    static inline std::recursive_mutex m_thread_actions_mutex;
    static inline bool m_is_currently_executing_game_action{};
};
#include "Review3ProductionDispatch.inl"
static VmHandle closing_vm;
static void close_root(LuaMadeSimple::Lua& lua,VmHandle vm) {
    auto& host=HostBindings::instance();auto* root=lua.get_lua_state();
    CHECK(host.claim_vm_close(vm)==Status::ready);CHECK(host.cleanup_vm(vm)==Status::ok);
    LuaMadeSimple::LuaFinalizationScope finalizing{root,root};lua_close(root);finalizing.close_completed();CHECK(host.finish_vm_close(vm)==Status::ok);
}
static void close_detached() {
    auto& host=HostBindings::instance();auto& one=LuaMadeSimple::new_state();auto& two=LuaMadeSimple::new_state();
    auto vm1=host.attach_vm(one.get_lua_state());auto vm2=host.attach_vm(two.get_lua_state());CHECK(vm1.status==Status::ok && vm2.status==Status::ok);
    int ref1{},ref2{};
    for(auto pair:{std::pair{&one,&ref1},std::pair{&two,&ref2}}) {
        ModLuaEntry entry{*pair.first};pair.first->open_all_libs();pair.first->execute_string("calls=0; function cb() calls=calls+1 end");
        lua_getglobal(pair.first->get_lua_state(),"cb");*pair.second=luaL_ref(pair.first->get_lua_state(),LUA_REGISTRYINDEX);
    }
    std::optional<DetachedLuaWork> external{std::in_place,&one};closing_vm=vm1.value;
    after_simple_actions_detached=[] {CHECK(HostBindings::instance().claim_vm_close(closing_vm)==Status::pending);};
    std::vector<LuaMod::SimpleLuaAction> actions{{&one,ref1,LUA_NOREF},{&two,ref2,LUA_NOREF}};
    bool escaped=false;try {process_simple_actions(actions);}catch(...) {escaped=true;}
    after_simple_actions_detached=nullptr;
    CHECK(!escaped);CHECK(!LuaMod::m_is_currently_executing_game_action);CHECK(actions.empty());
    {ModLuaEntry entry{two};two.execute_string("assert(calls==1)");}
    external.reset();close_root(one,vm1.value);close_root(two,vm2.value);
    std::cout<<"ROUND2 PASS exact production detached-action body: closing cancellation, flags, live successor\n";
}
static std::atomic<bool> gate_held{},snapshot_ready{};
static int schedule(const LuaMadeSimple::Lua&) {
    gate_held=true;while(!snapshot_ready.load()) std::this_thread::yield();
    std::lock_guard lock{LuaMod::m_thread_actions_mutex};return 0;
}
static void lock_order() {
    auto& host=HostBindings::instance();auto& one=LuaMadeSimple::new_state();auto& two=LuaMadeSimple::new_state();
    auto vm1=host.attach_vm(one.get_lua_state());auto vm2=host.attach_vm(two.get_lua_state());CHECK(vm1.status==Status::ok && vm2.status==Status::ok);
    {ModLuaEntry entry{one};one.register_function("Schedule",schedule);}
    before_detached_lua_entry=[] {snapshot_ready=true;};
    std::thread gt{[&] {
        while(!gate_held.load()) std::this_thread::yield();
        std::vector<PinnedRegistryCallback> work;
        {std::lock_guard lock{LuaMod::m_thread_actions_mutex};work.emplace_back(&two,LuaMod::LuaCallbackData::RegistryIndex{});}
        deliver_script_callback(work.front(),[]{return true;},[](const auto& lua,const auto&){(void)lua.get_stack_size();});
    }};
    one.execute_string("Schedule()");gt.join();before_detached_lua_entry=nullptr;
    close_root(one,vm1.value);close_root(two,vm2.value);CHECK(actual_gate_depth==0);
    std::cout<<"ROUND2 PASS production delivery admission vs actual LMS/native scheduling gate (two VMs)\n";
}
static void run() {if(!std::getenv("ROUND2_LOCK_ONLY")) close_detached();lock_order();CHECK(HostBindings::instance().vm_count()==0);}
}
