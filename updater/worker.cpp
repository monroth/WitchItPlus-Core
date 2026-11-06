// No engine access. WinHTTP, signature/hash verification and staging run here.
#include "core.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include "http.hpp"
#pragma comment(lib,"winhttp.lib")

static std::wstring wide(const std::string& s){return std::wstring(s.begin(),s.end());}
static std::string fetch(std::string url,size_t cap,wip::NetworkClock::time_point deadline){
#ifdef WIP_TEST_ORIGIN
 const std::string origin=WIP_TEST_ORIGIN;
 // Test-only executable has a distinct key + origin. Never a runtime switch.
 if(url.rfind("https://github.com/",0)==0)url=origin+url.substr(19);
#endif
 auto u=wide(url);URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
 wip::require(WinHttpCrackUrl(u.c_str(),DWORD(u.size()),0,&parts),"HTTPS URL parse");
#ifndef WIP_TEST_ORIGIN
 wip::require(parts.nScheme==INTERNET_SCHEME_HTTPS&&parts.nPort==443,"HTTPS required");
#endif
 auto host=std::wstring(parts.lpszHostName,parts.dwHostNameLength);auto target=std::wstring(parts.lpszUrlPath,parts.dwUrlPathLength)+std::wstring(parts.lpszExtraInfo,parts.dwExtraInfoLength);
 wip::Internet session{WinHttpOpen(L"WitchItPlus-Updater/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC)};wip::require(bool(session.h),"WinHTTP session");
 wip::require(WinHttpSetTimeouts(session,5000,5000,7000,7000),"WinHTTP timeouts");
#ifndef WIP_TEST_ORIGIN
 DWORD tls=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;wip::require(WinHttpSetOption(session,WINHTTP_OPTION_SECURE_PROTOCOLS,&tls,sizeof(tls)),"TLS policy");
#endif
 wip::Internet connection{WinHttpConnect(session,host.c_str(),parts.nPort,0)};wip::require(bool(connection.h),"WinHTTP connect");
 auto raw=WinHttpOpenRequest(connection,L"GET",target.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);wip::require(bool(raw),"WinHTTP request");
 // The worker runs beside the game, so a slow connection only takes longer.
 wip::HttpRequest request(raw,std::min(deadline,wip::NetworkClock::now()+(cap>wip::metadata_cap?wip::download_budget:wip::attempt_budget)));
 DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;wip::require(WinHttpSetOption(raw,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)),"HTTPS redirect policy");
 request.send();auto status=request.status();
 if(status==408||status==429||status==500||status==502||status==503||status==504)throw wip::NetworkError("HTTP status "+std::to_string(status));
 wip::require(status==200,"HTTP status "+std::to_string(status));
 // The menu reads this about once a second while a download runs.
 auto reported=wip::NetworkClock::now();
 return request.body(cap,[&](size_t done){
  if(cap<=wip::metadata_cap||wip::NetworkClock::now()<reported)return;
  reported=wip::NetworkClock::now()+std::chrono::milliseconds(500);wip::download_progress(done);
 });
}
int wmain(int argc,wchar_t** argv){
 wchar_t exe[32768];auto n=GetModuleFileNameW(nullptr,exe,32768);if(!n||n>=32768)return 2;auto root=wip::fs::path(exe).parent_path().parent_path();
 try{
#ifdef WIP_TEST_PUBLIC_KEY
  if(argc==2&&std::wstring(argv[1])==L"--activate"){wip::activate(root);return 0;}
#endif
  wip::require(argc==2&&std::wstring(argv[1])==L"--check","updater operation");
  // One bounded in-flight operation; nothing is scheduled after it returns.
  wip::check_with_retry([&](auto deadline){wip::check(root,[&](const auto& url,size_t cap){return fetch(url,cap,deadline);});},
   []{return wip::NetworkClock::now();},[](auto deadline){
    std::mutex mutex;std::condition_variable changed;std::unique_lock<std::mutex> lock(mutex);
    changed.wait_until(lock,deadline,[]{return false;});
   });wip::atomic_write(root/"ue4ss/.witchit-updater/check.done","done");return 0;
 }catch(const wip::NetworkError& e){try{wip::log(root,std::string("update check unavailable after bounded network retries: ")+e.what());wip::atomic_write(root/"ue4ss/.witchit-updater/check.done","network-unavailable");}catch(...){}return 1;
 }catch(const std::exception& e){try{wip::log(root,std::string("update check unavailable: ")+e.what());wip::atomic_write(root/"ue4ss/.witchit-updater/check.done","unavailable");}catch(...){}return 1;}
}
