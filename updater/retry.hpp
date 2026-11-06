#pragma once
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace wip {
// Only transport failures are retryable. Verification, policy and disk errors
// keep their original fail-closed path through check().
struct NetworkError : std::runtime_error {using std::runtime_error::runtime_error;};
using NetworkClock=std::chrono::steady_clock;
inline constexpr auto attempt_budget=std::chrono::seconds(8);
inline constexpr auto download_budget=std::chrono::minutes(10);
inline constexpr auto check_budget=std::chrono::minutes(30);
template<class Check,class Now,class Backoff>
void check_with_retry(Check check,Now now,Backoff backoff){
 const auto deadline=now()+check_budget;
 for(unsigned attempt=1;attempt<=3;++attempt){
  try{check(deadline);return;}
  catch(const NetworkError&){
   if(attempt==3||now()>=deadline)throw;
   backoff(std::min(deadline,now()+std::chrono::seconds(attempt)));
   if(now()>=deadline)throw;
  }
 }
}
}
