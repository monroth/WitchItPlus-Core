#include <Unreal/Hooks/Internal/CallbackOwnerBridge.hpp>
#include <mutex>
#include <vector>

namespace RC::Unreal::Hook::Internal {
namespace {
std::atomic<FCallbackOwnerBridge::FCapture> CapturePolicy{};
std::atomic<std::size_t> Quarantines{};
struct FRegistration {
    std::uint64_t Id{}, Generation{}, VmId{}, VmGeneration{};
    std::weak_ptr<IRevocableCallback> Slot;
};
std::mutex RegistryMutex;
std::vector<FRegistration> Registrations;
}
bool FCallbackOwnerBridge::InstallCapture(FCapture capture) noexcept {
    if (!capture) return false;
    FCapture expected{};
    return CapturePolicy.compare_exchange_strong(expected, capture, std::memory_order_release,
                                                 std::memory_order_acquire) || expected == capture;
}
FCallbackOwnerToken FCallbackOwnerBridge::Capture(const void* origin) {
    auto capture = CapturePolicy.load(std::memory_order_acquire);
    return capture ? capture(origin) : FCallbackOwnerToken{};
}
void FCallbackOwnerBridge::RecordQuarantine() noexcept { ++Quarantines; }
std::size_t FCallbackOwnerBridge::QuarantinedCount() noexcept { return Quarantines.load(); }
bool FCallbackOwnerBridge::Publish(const FCallbackOwnerToken& owner,
                                   const std::shared_ptr<IRevocableCallback>& slot) {
    if (!slot) return false;
    if (owner.Kind == FCallbackOwnerToken::EKind::Host && !owner.VmId) return true;
    if ((owner.Kind != FCallbackOwnerToken::EKind::Module &&
         owner.Kind != FCallbackOwnerToken::EKind::Host) || !owner.Admitted) return false;
    std::lock_guard lock{RegistryMutex};
    if (!owner.Admitted(owner)) return false;
    std::erase_if(Registrations, [](const auto& registration) { return registration.Slot.expired(); });
    Registrations.push_back({owner.ModuleId, owner.Generation, owner.VmId, owner.VmGeneration, slot});
    return true;
}
namespace {
template<class Predicate>
std::size_t Revoke(Predicate matches) {
    std::vector<std::shared_ptr<IRevocableCallback>> slots;
    {
        std::lock_guard lock{RegistryMutex};
        // Reserve before mutation: allocation failure leaves all registrations
        // attached and their code pins intact, preventing unsafe exclusive claim.
        slots.reserve(Registrations.size());
        for (const auto& item : Registrations) {
            if (matches(item))
                if (auto slot = item.Slot.lock()) slots.push_back(std::move(slot));
        }
        std::erase_if(Registrations, [&](const auto& item) {
            return item.Slot.expired() || matches(item);
        });
    }
    // Detach and destroy consumer function managers outside ALL shared control
    // locks. Concurrent snapshots hold their payload/pin until dispatch returns.
    for (auto& slot : slots) {
        auto payload = slot->Detach();
    }
    return slots.size();
}
}
std::size_t FCallbackOwnerBridge::RevokeModule(std::uint64_t id, std::uint64_t generation) {
    return Revoke([&](const auto& item) { return item.Id == id && item.Generation == generation; });
}
std::size_t FCallbackOwnerBridge::RevokeVm(std::uint64_t id, std::uint64_t generation) {
    return Revoke([&](const auto& item) { return item.VmId == id && item.VmGeneration == generation; });
}
}
