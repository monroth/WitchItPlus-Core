#pragma once
#include <Windows.h>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
namespace witchit::updater {
HANDLE launch_process(const wchar_t* operation,const std::wstring& argument);
// One OS process-completion notification. No game-frame or status-file observer.
// UnregisterWaitEx joins an executing callback before this DLL can unload.
class CheckCompletion final {
 std::mutex mutex_;HANDLE process_{},wait_{};bool started_{},done_{},stopped_{};
 std::function<void()> notify_;
 static void CALLBACK completed(void* context,BOOLEAN) {
  auto& self=*static_cast<CheckCompletion*>(context);std::function<void()> notify;
  {std::lock_guard lock(self.mutex_);if(self.stopped_)return;self.done_=true;notify=self.notify_;}
  if(notify)notify(); // owned SDK scalar signal; never Lua on an OS worker
 }
public:
 void subscribe(std::function<void()> notify) {
  bool done;{std::lock_guard lock(mutex_);if(stopped_)return;notify_=notify;done=done_;}
  if(done && notify)notify();
 }
 bool start() {
  std::lock_guard lock(mutex_);if(stopped_ || started_&&!done_)return false;
  // A finished check can run again: the player asked for the download.
  if(wait_){UnregisterWait(wait_);wait_=nullptr;}
  if(process_){CloseHandle(process_);process_=nullptr;}
  started_=true;done_=false;
  process_=launch_process(L"--check",L"");
  if(!RegisterWaitForSingleObject(&wait_,process_,completed,this,INFINITE,WT_EXECUTEONLYONCE)){
   CloseHandle(process_);process_=nullptr;throw std::runtime_error("wait for updater completion: "+std::to_string(GetLastError()));
  }
  return true;
 }
 void stop() noexcept {
  HANDLE wait{},process{};
  {std::lock_guard lock(mutex_);stopped_=true;notify_={};wait=wait_;wait_=nullptr;process=process_;process_=nullptr;}
  // Never wait under mutex_: an in-flight completed() needs it to leave.
  if(wait)UnregisterWaitEx(wait,INVALID_HANDLE_VALUE);
  if(process)CloseHandle(process);
 }
 ~CheckCompletion(){stop();}
};
}
