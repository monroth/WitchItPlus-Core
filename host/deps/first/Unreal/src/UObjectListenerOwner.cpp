#include <Unreal/Hooks/Internal/UObjectListenerOwner.hpp>
#include <Unreal/Hooks/Internal/CallbackOwnerBridge.hpp>
#include <Unreal/Hooks/Internal/ListenerInvocationGate.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace RC::Unreal::Hook::Internal {
namespace {
enum class EListenerEvent { Object, Shutdown };

// These callables are compiled into the stable host. Their Native pointer is
// borrowed, never owned or destroyed here. TCallbackSlot reads/invokes them only
// inside Owner.Dispatch, after the module callback lease has been acquired.
struct FCreateBody {
    FUObjectCreateListener* Listener{};
    void operator()(EListenerEvent event, const UObjectBase* object, int32 index) const {
        if (event == EListenerEvent::Shutdown) Listener->OnUObjectArrayShutdown();
        else Listener->NotifyUObjectCreated(object, index);
    }
};
struct FDeleteBody {
    FUObjectDeleteListener* Listener{};
    void operator()(EListenerEvent event, const UObjectBase* object, int32 index) const {
        if (event == EListenerEvent::Shutdown) Listener->OnUObjectArrayShutdown();
        else Listener->NotifyUObjectDeleted(object, index);
    }
};

bool ValidOwner(const FCallbackOwnerToken& owner) {
    return owner.Kind == FCallbackOwnerToken::EKind::Host ||
           (owner.Kind == FCallbackOwnerToken::EKind::Module && owner.ModuleId &&
            owner.Generation && owner.Pin && owner.Admitted && owner.Dispatch && owner.Destroy);
}

template<class Body>
class TListenerState : public IRevocableCallback {
    using FSlot = TCallbackSlot<Body>;
    const std::shared_ptr<FSlot> Slot;
    const FCallbackOwnerToken Owner; // metadata only: no persistent pin here
    FListenerInvocationGate Gate;
public:
    TListenerState(std::shared_ptr<FSlot> slot, FCallbackOwnerToken metadata)
        : Slot(std::move(slot)), Owner(std::move(metadata)) {}
    bool IsOpen() { return Gate.IsOpen(); }
    bool SameOwner(const FCallbackOwnerToken& caller) const {
        return Owner.Kind == caller.Kind &&
               (Owner.Kind == FCallbackOwnerToken::EKind::Host ||
                (Owner.ModuleId == caller.ModuleId && Owner.Generation == caller.Generation));
    }
    std::shared_ptr<void> Detach() noexcept final {
        Gate.Close();
        return Slot->Detach();
    }
    void DrainExternalRemoval() { Gate.WaitForExternalRemoval(); }
    void Notify(const UObjectBase* object, int32 index) {
        FListenerInvocationGate::FInvocation invocation{Gate};
        if (invocation) Slot->Invoke(EListenerEvent::Object, object, index);
    }
    void Shutdown() {
        FListenerInvocationGate::FInvocation invocation{Gate};
        if (!invocation) return;
        Gate.Close();
        struct FDetachOnExit {
            TListenerState& State;
            ~FDetachOnExit() { auto payload = State.Detach(); }
        } cleanup{*this};
        Slot->Invoke(EListenerEvent::Shutdown, static_cast<const UObjectBase*>(nullptr), int32{});
    }
};

// The engine receives only these stable SDK vtables. The real exported listener
// interfaces, their method order and their destructor remain unchanged.
class FCreateProxy final : public FUObjectCreateListener, public TListenerState<FCreateBody> {
    FUObjectArray* const Array;
public:
    FCreateProxy(std::shared_ptr<TCallbackSlot<FCreateBody>> slot, FCallbackOwnerToken metadata,
                 FUObjectArray* array)
        : TListenerState<FCreateBody>(std::move(slot), std::move(metadata)), Array(array) {}
    void NotifyUObjectCreated(const UObjectBase* object, int32 index) override { Notify(object, index); }
    void OnUObjectArrayShutdown() override {
        struct FRemoveArrayOnExit {
            FUObjectArray* Array;
            FUObjectCreateListener* Proxy;
            ~FRemoveArrayOnExit() { Array->GetUObjectCreateListeners().RemoveSingleSwap(Proxy); }
        } cleanup{Array, this};
        // Real SDK shutdown listeners remove themselves from the actual engine
        // array. Even a revoked proxy must honor that shutdown protocol. Retained
        // host storage survives any engine snapshot holding the removed pointer.
        Shutdown();
    }
};
class FDeleteProxy final : public FUObjectDeleteListener, public TListenerState<FDeleteBody> {
    FUObjectArray* const Array;
public:
    FDeleteProxy(std::shared_ptr<TCallbackSlot<FDeleteBody>> slot, FCallbackOwnerToken metadata,
                 FUObjectArray* array)
        : TListenerState<FDeleteBody>(std::move(slot), std::move(metadata)), Array(array) {}
    void NotifyUObjectDeleted(const UObjectBase* object, int32 index) override { Notify(object, index); }
    void OnUObjectArrayShutdown() override {
        struct FRemoveArrayOnExit {
            FUObjectArray* Array;
            FUObjectDeleteListener* Proxy;
            ~FRemoveArrayOnExit() { Array->GetUObjectDeleteListeners().RemoveSingleSwap(Proxy); }
        } cleanup{Array, this};
        Shutdown();
    }
};

template<class Listener, class Proxy>
struct TListenerRegistry {
    std::mutex Mutex;
    std::unordered_map<Listener*, std::shared_ptr<Proxy>> Active;
    // An engine traversal/snapshot may still hold any previously appended raw
    // proxy. There is no engine-quiescence API here, so retain stable tombstones
    // for the host process lifetime. Never retain a payload pin in this vector.
    std::vector<std::shared_ptr<Proxy>> Retained;
};
template<class Listener, class Proxy>
TListenerRegistry<Listener, Proxy>& Registry() {
    // Deliberately no process-exit static destruction while engine snapshots or
    // module teardown could still call a host proxy. This is host-only memory.
    static auto* registry = new TListenerRegistry<Listener, Proxy>;
    return *registry;
}

template<class Listener, class Proxy, class Body, class Append>
void Add(Listener* listener, const void* origin, FUObjectArray* array, Append append) {
    if (!listener) throw std::runtime_error{"Cannot register a null UObject listener"};
    if (!array) throw std::runtime_error{"Cannot register a listener before GUObjectArray setup"};
    auto owner = FCallbackOwnerBridge::Capture(origin);
    if (!ValidOwner(owner))
        throw std::runtime_error{"UObject listener registration has no admitted callback owner"};
    auto slot = TCallbackSlot<Body>::Create(Body{listener}, origin);
    if (!slot) throw std::runtime_error{"UObject listener callback owner was revoked"};
    auto metadata = owner;
    metadata.Pin.reset();
    auto proxy = std::make_shared<Proxy>(std::move(slot), std::move(metadata), array);
    // Publish the proxy as well as its slot. RevokeModule closes proxy admission
    // without waiting before the host requests its exclusive retirement claim.
    if (!FCallbackOwnerBridge::Publish(owner, proxy)) {
        auto payload = proxy->Detach();
        throw std::runtime_error{"UObject listener callback owner was revoked"};
    }
    auto& registry = Registry<Listener, Proxy>();
    try {
        {
            std::lock_guard lock{registry.Mutex};
            auto existing = registry.Active.find(listener);
            if (existing != registry.Active.end()) {
                if (existing->second->IsOpen())
                    throw std::runtime_error{"Cannot add a listener because it already exists in TArray"};
                registry.Active.erase(existing); // Retained still owns the old host tombstone
            }
            registry.Retained.push_back(proxy);
            registry.Active.emplace(listener, proxy);
        }
        // No registry/control lock crosses an engine allocator or array write.
        // Caller-provided engine-safe registration ordering remains required.
        append(static_cast<Listener*>(proxy.get()));
    } catch (...) {
        {
            std::lock_guard lock{registry.Mutex};
            auto found = registry.Active.find(listener);
            if (found != registry.Active.end() && found->second == proxy)
                registry.Active.erase(found);
        }
        // Registry metadata cleanup released the mutex before payload/pin death.
        auto payload = proxy->Detach();
        throw;
    }
}

template<class Listener, class Proxy>
void Remove(Listener* listener, const void* origin) {
    if (!listener) return;
    FCallbackOwnerToken owner;
    try { owner = FCallbackOwnerBridge::Capture(origin); }
    catch (...) { return; } // removal failure must not unwind a Native destructor
    // Removal from an unknown image or without current CodeOwner authority must
    // not detach somebody else's registration. Rejected retired cleanup remains
    // safe: the pre-claim module revoker already closed/detached its proxy.
    if (!ValidOwner(owner)) return;
    auto& registry = Registry<Listener, Proxy>();
    std::shared_ptr<Proxy> proxy;
    {
        std::lock_guard lock{registry.Mutex};
        auto found = registry.Active.find(listener);
        if (found == registry.Active.end() || !found->second->SameOwner(owner)) return;
        proxy = found->second;
        registry.Active.erase(found); // Retained and local proxy keep host storage stable
    }
    auto payload = proxy->Detach();
    proxy->DrainExternalRemoval();
    // Local payload destruction and owner pin release happen outside every
    // registry/gate lock; TCallbackSlot establishes the host destruction mask.
}
}

void AddOwnedCreateListener(FUObjectCreateListener* listener, const void* origin) {
    auto* array = GUObjectArray;
    Add<FUObjectCreateListener, FCreateProxy, FCreateBody>(listener, origin, array, [array](FUObjectCreateListener* proxy) {
        // The actual SDK GUObjectArray/TArray path. The SDK's object-array
        // Lock/Unlock are TODO no-ops, so the caller still needs an engine-safe
        // registration point; this mutex does not synchronize engine traversal.
        array->GetUObjectCreateListeners().Append(TArray{proxy});
    });
}
void RemoveOwnedCreateListener(FUObjectCreateListener* listener, const void* origin) {
    Remove<FUObjectCreateListener, FCreateProxy>(listener, origin);
}
void AddOwnedDeleteListener(FUObjectDeleteListener* listener, const void* origin) {
    auto* array = GUObjectArray;
    Add<FUObjectDeleteListener, FDeleteProxy, FDeleteBody>(listener, origin, array, [array](FUObjectDeleteListener* proxy) {
        array->GetUObjectDeleteListeners().Append(TArray{proxy});
    });
}
void RemoveOwnedDeleteListener(FUObjectDeleteListener* listener, const void* origin) {
    Remove<FUObjectDeleteListener, FDeleteProxy>(listener, origin);
}
}
