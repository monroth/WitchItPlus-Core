#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace RC::Unreal::Hook::Internal {
// Stable host-only bookkeeping. It never reads a listener or calls its vtable.
// Closing is nonblocking, so the module revoker can close it before claiming
// exclusive retirement. A normal external Remove can subsequently drain it.
class FListenerInvocationGate final {
    std::mutex Mutex;
    std::condition_variable Changed;
    bool Accepting{true};
    std::size_t InFlight{};
    struct FCurrent {
        FListenerInvocationGate* Gate;
        FCurrent* Previous;
    };
    inline static thread_local FCurrent* Current{};
public:
    class FInvocation final {
        FListenerInvocationGate* Gate{};
        FCurrent Frame{};
    public:
        explicit FInvocation(FListenerInvocationGate& gate) {
            {
                std::lock_guard lock{gate.Mutex};
                if (!gate.Accepting) return;
                ++gate.InFlight;
            }
            Gate = &gate;
            Frame = {Gate, Current};
            Current = &Frame;
        }
        FInvocation(const FInvocation&) = delete;
        FInvocation& operator=(const FInvocation&) = delete;
        ~FInvocation() {
            if (!Gate) return;
            Current = Frame.Previous;
            {
                std::lock_guard lock{Gate->Mutex};
                --Gate->InFlight;
            }
            Gate->Changed.notify_all();
        }
        explicit operator bool() const noexcept { return Gate != nullptr; }
    };
    void Close() noexcept {
        std::lock_guard lock{Mutex};
        Accepting = false;
    }
    bool IsOpen() {
        std::lock_guard lock{Mutex};
        return Accepting;
    }
    void WaitForExternalRemoval() {
        // Self-removal is used by the real SDK shutdown listeners. Waiting here
        // would deadlock a reentrant call (or two concurrent self-removers).
        // A borrowed listener must not destroy itself while another notification
        // is running. Full module unload instead uses the host exclusive claim.
        for (auto* current = Current; current; current = current->Previous)
            if (current->Gate == this) return;
        std::unique_lock lock{Mutex};
        Changed.wait(lock, [&] { return InFlight == 0; });
    }
};
}
