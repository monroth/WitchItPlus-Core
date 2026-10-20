#include "windows.h"
#include <mutex>
#include <atomic>
static std::mutex once_mutex;
static std::atomic<int> initialized{},deleted{};
static thread_local int depth{};
extern "C" BOOL InitOnceExecuteOnce(PINIT_ONCE once,PINIT_ONCE_FN function,PVOID argument,PVOID* result) {
    std::lock_guard lock(once_mutex);
    if (once->value) return TRUE;
    if (!function(once,argument,result)) return 0;
    once->value=once; return TRUE;
}
extern "C" void InitializeCriticalSection(CRITICAL_SECTION* critical) {
    critical->value=new std::recursive_mutex; ++initialized;
}
extern "C" void DeleteCriticalSection(CRITICAL_SECTION*) {
    // Keep old erroneous source deterministic: do not invoke UB by deleting a
    // held std mutex. Its forbidden deletion is recorded and fails the test.
    ++deleted;
}
extern "C" void EnterCriticalSection(CRITICAL_SECTION* critical) {
    static_cast<std::recursive_mutex*>(critical->value)->lock(); ++depth;
}
extern "C" void LeaveCriticalSection(CRITICAL_SECTION* critical) {
    --depth; static_cast<std::recursive_mutex*>(critical->value)->unlock();
}
extern "C" int gate_initializations(){return initialized.load();}
extern "C" int gate_deletions(){return deleted.load();}
extern "C" int gate_depth(){return depth;}
