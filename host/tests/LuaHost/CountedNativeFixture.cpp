#include <Mod/CppUserModBase.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaHost/ModLuaEntry.hpp>
#include <Unreal/Hooks/Internal/Metadata.hpp>
#include <chrono>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>
#include <iostream>
using namespace RC;
using namespace std::chrono_literals;
namespace {
class Counted final:public CppUserModBase {
 struct Work {
  Unreal::Hook::Internal::FCallbackMetadata metadata{1,L"Native4Counted",L"completion"};
  std::binary_semaphore release{0},counted{0};std::thread thread;
 };
 std::vector<std::unique_ptr<Work>> work_;bool stopped_{};
public:
 Counted(){ModName=L"Native4Counted";}
 void on_lua_start(StringViewType name,LuaMadeSimple::Lua& lua,LuaMadeSimple::Lua&,LuaMadeSimple::Lua&,LuaMadeSimple::Lua*) override {
  if(name!=L"WitchItRuntime")return;
  if(!work_.empty())throw std::runtime_error("prior counted fixture not drained");stopped_=false;
  for(unsigned i=0;i<4;++i){auto w=std::make_unique<Work>();auto* state=w.get();auto* vm=&lua;
   w->thread=std::thread([state,vm]{state->release.acquire();bool once=false;
    if(!state->metadata.TryBeginExecution(false,once))std::_Exit(91);
    state->counted.release();
    try {LuaHost::ModLuaEntry entry{*vm};(void)vm->get_stack_size();}catch(...){std::_Exit(92);}
    state->metadata.EndExecution();});work_.push_back(std::move(w));
  }
 }
 void on_lua_stop(StringViewType name,LuaMadeSimple::Lua&,LuaMadeSimple::Lua&,LuaMadeSimple::Lua&,LuaMadeSimple::Lua*) override {
  if(name!=L"WitchItRuntime")return;
  for(auto& w:work_){w->release.release();w->counted.acquire();
   if(w->metadata.GetExecutorCount()!=1)throw std::runtime_error("executor was not counted before Lua entry");}
  stopped_=true;
 }
 void on_lua_stop_drain(StringViewType name) override {
  if(name!=L"WitchItRuntime")return;
  if(!stopped_)throw std::runtime_error("drain preceded stop");
  for(auto& w:work_)w->metadata.Invalidate();
  for(auto& w:work_){w->metadata.WaitForExecutorsToFinish();w->thread.join();
   if(w->metadata.GetExecutorCount()!=0)throw std::runtime_error("executor leaked");}
  work_.clear();
 }
 bool prepare_uninstall() override{return work_.empty();}
};
}
extern "C" __declspec(dllexport) CppUserModBase* start_mod(){return new Counted;}
extern "C" __declspec(dllexport) void uninstall_mod(CppUserModBase* value){delete value;}
