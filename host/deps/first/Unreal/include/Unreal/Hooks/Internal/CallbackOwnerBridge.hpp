#pragma once

#include <Unreal/Common.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace RC::Unreal::Hook::Internal {
// Host policy supplies this record. No consumer-selected module or Lua state.
// Rejected is the fail-closed default; Host requires affirmative provenance.
struct FCallbackOwnerToken {
    enum class EKind { Rejected, Host, Module };
    EKind Kind{EKind::Rejected};
    std::uint64_t ModuleId{}, Generation{};
    std::uint64_t VmId{}, VmGeneration{}; // trusted registration context, if present
    std::shared_ptr<void> Pin;
    std::shared_ptr<void> DeferredVm; // host metadata for a proven later association
    bool (*Admitted)(const FCallbackOwnerToken&){};
    // Synchronous host trampoline: enter owner lease/TLS before invoking body.
    bool (*Dispatch)(const FCallbackOwnerToken&, void*, void (*)(void*)){};
    // Pin is still held, including after retirement; establish owner TLS/mask
    // without requesting normal admission before invoking manager destruction.
    void (*Destroy)(const FCallbackOwnerToken&, void*, void (*)(void*)) noexcept{};
};

class IRevocableCallback {
public:
    virtual ~IRevocableCallback() = default;
    virtual std::shared_ptr<void> Detach() noexcept = 0;
};

class RC_UE_API FCallbackOwnerBridge {
public:
    using FCapture = FCallbackOwnerToken (*)(const void* registration_origin);
    // Installed once by the host before external callback registration.
    static bool InstallCapture(FCapture) noexcept;
    static FCallbackOwnerToken Capture(const void* registration_origin = nullptr);
    static void RecordQuarantine() noexcept;
    static std::size_t QuarantinedCount() noexcept;
    // Validates admission while publishing metadata. No consumer body/manager
    // operation is called under this registry's mutex.
    static bool Publish(const FCallbackOwnerToken&, const std::shared_ptr<IRevocableCallback>&);
    // Called after closing lifecycle admission and BEFORE exclusive retire claim.
    // Nonblocking: snapshots already dispatching retain their own payload pins.
    static std::size_t RevokeModule(std::uint64_t id, std::uint64_t generation);
    // VM association is metadata only; idle slots never hold a VM lease.
    static std::size_t RevokeVm(std::uint64_t id, std::uint64_t generation);
};

template<class CallbackType>
class TCallbackSlot final : public IRevocableCallback {
    struct FPayload {
        FCallbackOwnerToken Owner; // destroyed after Callback / manager
        CallbackType Callback;
        FPayload(FCallbackOwnerToken owner, CallbackType callback)
            : Owner(std::move(owner)), Callback(std::move(callback)) {}
        ~FPayload() {
            const auto reset = [](void* opaque) {
                static_cast<FPayload*>(opaque)->Callback = {};
            };
            if (Owner.Destroy)
                Owner.Destroy(Owner, this, reset);
            else if (Owner.Kind == FCallbackOwnerToken::EKind::Host) reset(this);
            else std::terminate(); // incomplete policy records must never reach storage
        }
    };
    std::atomic<std::shared_ptr<FPayload>> Payload;
    explicit TCallbackSlot(std::shared_ptr<FPayload> payload) : Payload(std::move(payload)) {}
    static bool Valid(const FCallbackOwnerToken& owner) noexcept {
        if (owner.Kind == FCallbackOwnerToken::EKind::Host)
            return owner.Dispatch && owner.Destroy && (!owner.VmId ||
                (owner.VmGeneration && owner.Admitted));
        return owner.Kind == FCallbackOwnerToken::EKind::Module &&
            owner.ModuleId && owner.Generation && owner.Pin && owner.Admitted &&
            owner.Dispatch && owner.Destroy;
    }
    static std::shared_ptr<TCallbackSlot> CreateCaptured(CallbackType callback,
                                                        FCallbackOwnerToken owner) {
        // All Native manager operations happen outside registries, with the
        // original image pin alive through allocation/copy exception unwinding.
        struct FInputDisposal {
            FCallbackOwnerToken& Owner;
            CallbackType& Callback;
            ~FInputDisposal() {
                const auto reset = [](void* opaque) { *static_cast<CallbackType*>(opaque) = {}; };
                Owner.Destroy(Owner, &Callback, reset);
            }
        } input{owner, callback};
        auto payload = std::make_shared<FPayload>(owner, std::move(callback));
        auto result = std::shared_ptr<TCallbackSlot>(new TCallbackSlot(payload));
        if (!FCallbackOwnerBridge::Publish(payload->Owner, result)) return {};
        return result;
    }
public:
    static std::shared_ptr<TCallbackSlot> Create(CallbackType callback,
                                                const void* registration_origin = nullptr) {
        static_assert(std::is_nothrow_move_constructible_v<CallbackType>);
        const auto quarantine = [&] {
            // An owned unresolved manager cannot safely be destroyed. Retain
            // without executing it; exhaustion stops before unpinned unwinding.
            auto* retained = new (std::nothrow) CallbackType(std::move(callback));
            if (!retained) std::terminate();
            (void)retained;
            FCallbackOwnerBridge::RecordQuarantine();
        };
        FCallbackOwnerToken owner;
        try { owner = FCallbackOwnerBridge::Capture(registration_origin); }
        catch (...) { quarantine(); return {}; }
        if (!Valid(owner)) {
            if (owner.Pin && owner.Destroy) {
                const auto reset = [](void* opaque) { *static_cast<CallbackType*>(opaque) = {}; };
                owner.Destroy(owner, &callback, reset);
            } else quarantine();
            return {};
        }
        return CreateCaptured(std::move(callback), std::move(owner));
    }
    static std::shared_ptr<TCallbackSlot> CreateCopy(const CallbackType& callback,
                                                    const void* registration_origin) {
        // Borrowed input remains caller-owned when refused. Capture the actual
        // first-export image and pin it BEFORE copying its function manager.
        auto owner = FCallbackOwnerBridge::Capture(registration_origin);
        if (!Valid(owner) || (owner.Admitted && !owner.Admitted(owner))) return {};
        CallbackType copy{};
        std::exception_ptr exception;
        struct FCopy { const CallbackType* Input; CallbackType* Output; std::exception_ptr* Exception; }
            request{&callback, &copy, &exception};
        const auto perform = +[](void* opaque) {
            auto* request = static_cast<FCopy*>(opaque);
            try { *request->Output = *request->Input; }
            catch (...) { *request->Exception = std::current_exception(); }
        };
        // The same stable original-owner/masked manager context also surrounds
        // copy construction and any partial-manager cleanup on copy failure.
        owner.Destroy(owner, &request, perform);
        if (exception) std::rethrow_exception(exception);
        return CreateCaptured(std::move(copy), std::move(owner));
    }
    std::shared_ptr<void> Detach() noexcept override {
        return Payload.exchange({}, std::memory_order_acq_rel);
    }
    template<class... Args>
    bool Invoke(Args&&... args) {
        // This only touches host-owned metadata. The persistent pin already
        // protects the payload, and Dispatch acquires an active callback lease
        // before the body first reads the consumer std::function.
        auto payload = Payload.load(std::memory_order_acquire);
        if (!payload) return false;
        auto body = [&] { payload->Callback(std::forward<Args>(args)...); };
        const auto invoke = [](void* opaque) {
            (*static_cast<decltype(body)*>(opaque))();
        };
        return payload->Owner.Dispatch(payload->Owner, &body, invoke);
    }
};
}
