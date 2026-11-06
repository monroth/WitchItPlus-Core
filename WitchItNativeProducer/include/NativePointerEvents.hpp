#pragma once
#include <Windows.h>
#include <UE4SSProgram.hpp>
#include <memory>
#include <atomic>
#include <functional>
#include <stdexcept>
namespace witchit {
// Actual window-thread raw mouse/keyboard messages, only while aim is enabled.
// No viewport tick, cursor-position sampler, OS timer or UObject read here.
class NativePointerEvents {
 struct State {std::atomic<bool> active{true},queued{};std::function<void()> signal,deliver;};
 std::shared_ptr<State> state_;HHOOK hook_{};
 inline static thread_local std::weak_ptr<State> current_;
 static LRESULT CALLBACK message(int code,WPARAM remove,LPARAM value) {
  if(code==HC_ACTION && remove==PM_REMOVE)if(auto state=current_.lock();state && state->active) {
   auto* m=reinterpret_cast<MSG*>(value);bool changed=m->message==WM_KEYUP || m->message==WM_SYSKEYUP || m->message==WM_KEYDOWN || m->message==WM_SYSKEYDOWN;
   if(m->message==WM_INPUT) {
    RAWINPUT input{};UINT bytes=sizeof(input);
    if(GetRawInputData(reinterpret_cast<HRAWINPUT>(m->lParam),RID_INPUT,&input,&bytes,sizeof(RAWINPUTHEADER))!=UINT(-1)
     && input.header.dwType==RIM_TYPEMOUSE)changed=input.data.mouse.lLastX!=0 || input.data.mouse.lLastY!=0 || input.data.mouse.usButtonFlags!=0;
   }
   if(changed && state->active && !state->queued.exchange(true)){state->signal();}
  }
  return CallNextHookEx(nullptr,code,remove,value);
 }
public:
 ~NativePointerEvents(){stop();}
 NativePointerEvents()=default;
 NativePointerEvents(const NativePointerEvents&)=delete;
 NativePointerEvents& operator=(const NativePointerEvents&)=delete;
 void start(std::function<void()> deliver) {
  if(state_)return;
  // UnhookWindowsHookEx explicitly does not join another thread's hook
  // procedure. Its last instructions/CallNextHookEx can outlive stop(). Keep
  // this OS callback's code mapped until process termination; no destructor,
  // delayed cleanup or game-thread callback is needed to release this pin.
  HMODULE code{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
       reinterpret_cast<LPCWSTR>(&message),&code))throw std::runtime_error("pointer callback code pin unavailable");
  auto state=std::make_shared<State>();state->deliver=std::move(deliver);std::weak_ptr<State> weak=state;
  state->signal=RC::UE4SSProgram::get_program().make_game_thread_event([weak]{
   if(auto s=weak.lock();s && s->active){s->queued=false;s->deliver();}
  },false);
  if(!state->signal)throw std::runtime_error("pointer game-thread dispatcher unavailable");
  if(!current_.expired())throw std::runtime_error("pointer event owner already registered");
  current_=state;hook_=SetWindowsHookExW(WH_GETMESSAGE,message,nullptr,GetCurrentThreadId());
  if(!hook_){current_.reset();throw std::runtime_error("pointer message hook unavailable");}
  state_=std::move(state);
 }
 void stop(){if(state_){state_->active=false;if(current_.lock()==state_)current_.reset();}if(hook_)UnhookWindowsHookEx(hook_);hook_=nullptr;state_.reset();}
};
}
