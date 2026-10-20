#pragma once
// Authoritative input is the actual engine viewport Tick callback, never a Lua
// world alias or a scan. All retained object references are genuine SDK weak refs.
#include "NativeWorldContext.hpp"
#include <Unreal/UGameViewportClient.hpp>
#include <Unreal/ULocalPlayer.hpp>
#include <Unreal/World.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <array>
#include <mutex>
#include <stdexcept>

namespace witchit {
class OwnedLocalWorld final : public RC::Unreal::FUObjectDeleteListener {
    using UObject=RC::Unreal::UObject;
    using Weak=RC::Unreal::FWeakObjectPtr;
    using Observation=uikit::NativeWorldObservation;
    struct Tracked {UObject* pointer{};Weak weak;};
    // viewport -> game instance -> primary local player -> controller -> world
    std::array<Tracked,5> objects_{};
    std::recursive_mutex mutex_;
    std::uint64_t generation_{};
    bool enabled_{},listeners_{},exhausted_{};
    static bool usable(UObject* p) {
        using namespace RC::Unreal;
        return p && UObject::IsReal(p)
            && !p->HasAnyFlags(static_cast<RC::Unreal::EObjectFlags>(RF_ClassDefaultObject|RF_ArchetypeObject|RF_BeginDestroyed|RF_FinishDestroyed))
            && !p->HasAnyInternalFlags(EInternalObjectFlags::PendingKill|EInternalObjectFlags::Unreachable|EInternalObjectFlags::PendingConstruction);
    }
    static bool live(const Tracked& h) noexcept {
        try{return h.pointer && h.weak.ObjectIndex>=0 && h.weak.ObjectSerialNumber>0 && h.weak.Get()==h.pointer;}catch(...){return false;}
    }
    static UObject* object_field(UObject* object,const wchar_t* key) {
        using namespace RC::Unreal;
        auto* p=CastField<FObjectProperty>(object->GetPropertyByNameInChain(FName(key,FNAME_Find)));
        if(!p || p->GetArrayDim()!=1)throw std::runtime_error("local gameplay context unavailable");
        return p->GetObjectPropertyValue(p->ContainerPtrToValuePtr<void>(object));
    }
    bool live_record() const noexcept {
        if(!enabled_ || exhausted_ || !generation_)return false;
        for(const auto& h:objects_)if(!live(h))return false;
        return true;
    }
    void revoke_locked() noexcept {
        objects_={};
        if(generation_==UINT64_MAX)exhausted_=true;
        else ++generation_;
    }
    Observation observation_locked() const noexcept {
        if(!live_record())return {};
        const auto& world=objects_[4];const auto& pc=objects_[3];
        return {generation_,world.pointer,pc.pointer,world.weak.ObjectIndex,world.weak.ObjectSerialNumber,pc.weak.ObjectIndex,pc.weak.ObjectSerialNumber};
    }
public:
    ~OwnedLocalWorld(){stop();if(listeners_)RC::Unreal::FUObjectArray::RemoveUObjectDeleteListener(this);}
    void start() {
        std::lock_guard lock(mutex_);revoke_locked();enabled_=!exhausted_;
        if(!listeners_){RC::Unreal::FUObjectArray::AddUObjectDeleteListener(this);listeners_=true;}
    }
    void stop() noexcept {std::lock_guard lock(mutex_);enabled_=false;revoke_locked();}
    void before_world() noexcept {std::lock_guard lock(mutex_);revoke_locked();}
    // Returns true only when an established record changed or was revoked.
    // Reflection reads below never execute ProcessEvent or a Lua/user callback.
    bool observe(RC::Unreal::UGameViewportClient* viewport) noexcept {
        using namespace RC::Unreal;
        std::lock_guard lock(mutex_);
        if(!enabled_ || exhausted_ || !IsInGameThreadRaw())return false;
        const bool had_record=objects_[0].pointer!=nullptr;
        try {
            if(!usable(viewport) || !viewport->IsA<UGameViewportClient>())throw std::runtime_error("local gameplay context unavailable");
            auto* gi=object_field(viewport,L"GameInstance");if(!usable(gi))throw std::runtime_error("local gameplay context unavailable");
            auto* p=CastField<FArrayProperty>(gi->GetPropertyByNameInChain(FName(L"LocalPlayers",FNAME_Find)));
            auto* inner=p?CastField<FObjectProperty>(p->GetInner()):nullptr;
            if(!p || !inner || p->GetArrayDim()!=1)throw std::runtime_error("local gameplay context unavailable");
            FScriptArrayHelper players(p,p->ContainerPtrToValuePtr<void>(gi));
            if(players.Num()<1 || players.Num()>4)throw std::runtime_error("local gameplay context unavailable");
            auto* player=inner->GetObjectPropertyValue(players.GetRawPtr(0));
            if(!usable(player) || !player->IsA<ULocalPlayer>())throw std::runtime_error("local gameplay context unavailable");
            UGameViewportClient* backlink=static_cast<ULocalPlayer*>(player)->GetViewportClient();
            if(backlink!=viewport)throw std::runtime_error("local gameplay context unavailable");
            auto* pc=object_field(player,L"PlayerController");
            if(!usable(pc) || object_field(pc,L"Player")!=player)throw std::runtime_error("local gameplay context unavailable");
            auto* world=viewport->GetWorld();
            if(!usable(world) || !world->IsA<UWorld>() || pc->GetWorld()!=world)throw std::runtime_error("local gameplay context unavailable");
            std::array<UObject*,5> raw{viewport,gi,player,pc,world};
            std::array<Tracked,5> next;
            for(std::size_t i=0;i<raw.size();++i){next[i].pointer=raw[i];next[i].weak=raw[i];if(!live(next[i]))throw std::runtime_error("local gameplay context unavailable");}
            bool same=live_record();
            for(std::size_t i=0;i<next.size();++i)same=same && objects_[i].pointer==next[i].pointer
                && objects_[i].weak.ObjectIndex==next[i].weak.ObjectIndex && objects_[i].weak.ObjectSerialNumber==next[i].weak.ObjectSerialNumber;
            if(same)return false;
            revoke_locked();if(exhausted_)return had_record;
            objects_=next;return had_record;
        }catch(...){if(had_record){revoke_locked();return true;}return false;}
    }
    Observation capture() noexcept {
        std::lock_guard lock(mutex_);if(!RC::Unreal::IsInGameThreadRaw())return {};return observation_locked();
    }
    bool current(Observation held) noexcept {
        std::lock_guard lock(mutex_);if(!RC::Unreal::IsInGameThreadRaw())return false;
        const auto now=observation_locked();
        return held.generation && held.generation==now.generation && held.world==now.world && held.local_anchor==now.local_anchor
            && held.world_index==now.world_index && held.world_serial==now.world_serial
            && held.anchor_index==now.anchor_index && held.anchor_serial==now.anchor_serial;
    }
    UObject* game_instance(Observation held) noexcept {
        std::lock_guard lock(mutex_);return current(held)?objects_[1].pointer:nullptr;
    }
    uikit::NativeWorldSource source() noexcept {
        return {this,[](void* p) noexcept{return static_cast<OwnedLocalWorld*>(p)->capture();},
            [](void* p,Observation held) noexcept{return static_cast<OwnedLocalWorld*>(p)->current(held);}};
    }
    void NotifyUObjectDeleted(const RC::Unreal::UObjectBase* object,RC::Unreal::int32 index) override {
        std::lock_guard lock(mutex_);
        for(const auto& h:objects_)if(h.pointer==object && h.weak.ObjectIndex==index){revoke_locked();return;}
    }
    void OnUObjectArrayShutdown() override {
        stop();if(listeners_){listeners_=false;RC::Unreal::FUObjectArray::RemoveUObjectDeleteListener(this);}
    }
};
}
