#pragma once
#include "retry.hpp"
#include <windows.h>
#include <winhttp.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
namespace wip {
struct Internet {HINTERNET h{};~Internet(){if(h)WinHttpCloseHandle(h);}operator HINTERNET()const{return h;}};
inline void network_result(bool ok,const char* what){
 if(ok)return;
 auto error=GetLastError();
 switch(error){
 case ERROR_WINHTTP_TIMEOUT:case ERROR_WINHTTP_NAME_NOT_RESOLVED:
 case ERROR_WINHTTP_CANNOT_CONNECT:case ERROR_WINHTTP_CONNECTION_ERROR:
 case ERROR_WINHTTP_RESEND_REQUEST:
  throw NetworkError(std::string(what)+" (WinHTTP "+std::to_string(error)+")");
 default:throw std::runtime_error(std::string(what)+" (WinHTTP "+std::to_string(error)+")");
 }
}
// Async WinHTTP gives the worker a real end-to-end deadline, including DNS,
// proxy negotiation, redirects and a stalled/dribbling response body. A timeout
// cancels the request, not a synchronous handle in use by another thread.
class HttpRequest {
 struct State {
  std::mutex mutex;std::condition_variable changed;
  bool done{};DWORD error{},read{};char buffer[65536];
 };
 std::shared_ptr<State> state=std::make_shared<State>();
 HINTERNET handle{};DWORD_PTR context{};NetworkClock::time_point deadline;
 static void CALLBACK callback(HINTERNET,DWORD_PTR context,DWORD status,void* info,DWORD size){
  if(!context)return;
  auto binding=reinterpret_cast<std::shared_ptr<State>*>(context);
  auto state=*binding;
  // WinHTTP's final callback owns the context and read buffer until cancellation
  // has retired. No waiting thread or callback uses stack-owned response bytes.
  if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING){delete binding;return;}
  std::lock_guard<std::mutex> lock(state->mutex);
  switch(status){
  case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:state->error=static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;break;
  case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:state->read=size;break;
  case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:break;
  default:return;
  }
  state->done=true;state->changed.notify_one();
 }
 template<class Start>void operation(Start start){
  {std::lock_guard<std::mutex> lock(state->mutex);state->done=false;state->error=0;}
  if(NetworkClock::now()>=deadline)throw NetworkError("network deadline");
  network_result(start(),"network request");
  std::unique_lock<std::mutex> lock(state->mutex);
  if(!state->changed.wait_until(lock,deadline,[&]{return state->done;}))throw NetworkError("network deadline");
  if(state->error){SetLastError(state->error);network_result(false,"network unavailable");}
 }
public:
 HttpRequest(HINTERNET request,NetworkClock::time_point until):handle(request),deadline(until){
  auto binding=new std::shared_ptr<State>(state);context=reinterpret_cast<DWORD_PTR>(binding);
  if(!WinHttpSetOption(handle,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))){delete binding;WinHttpCloseHandle(handle);throw std::runtime_error("WinHTTP context");}
  if(WinHttpSetStatusCallback(handle,callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK){
   delete binding;WinHttpCloseHandle(handle);throw std::runtime_error("WinHTTP callback");
  }
 }
 HttpRequest(const HttpRequest&)=delete;
 ~HttpRequest(){WinHttpCloseHandle(handle);}
 void send(){operation([&]{return WinHttpSendRequest(handle,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,context);});
             operation([&]{return WinHttpReceiveResponse(handle,nullptr);});}
 DWORD status(){DWORD value=0,size=sizeof(value);network_result(WinHttpQueryHeaders(handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&value,&size,WINHTTP_NO_HEADER_INDEX),"HTTP status query");return value;}
 std::string body(size_t cap,const std::function<void(size_t)>& progress={}){
  std::string bytes;
  for(;;){operation([&]{return WinHttpReadData(handle,state->buffer,sizeof(state->buffer),nullptr);});
   if(!state->read)return bytes;
   if(bytes.size()+state->read>cap)throw std::runtime_error("download cap");
   bytes.append(state->buffer,state->read);
   if(progress)progress(bytes.size());
  }
 }
};
}
