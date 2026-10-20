// Exact production assignment/getters, isolated from Unreal patching.
#include <thread>
#include <stdexcept>
#include <atomic>
#include <barrier>
#define STR(x) x
enum class LogLevel {Error};
namespace Output {template<LogLevel> void send(const char*) {}}
#include "ProductionIdentity.inl"
int main(){
    std::barrier ready{2};std::atomic<unsigned> matched{};
    std::thread worker{[&]{ready.arrive_and_wait();for(int i=0;i<100000;++i){
        if(Unreal::IsInGameThreadRaw()) ++matched;
        (void)Unreal::GetGameThreadIdRaw();
        if(Unreal::IsGameThreadInitialized()) (void)Unreal::GetGameThreadId();
    }}};
    ready.arrive_and_wait();Unreal::UnrealInitializer::establish_from_engine_tick();
    worker.join();return int(matched.load());
}
