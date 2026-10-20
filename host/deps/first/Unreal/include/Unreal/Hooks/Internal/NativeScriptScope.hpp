#pragma once
#include <Unreal/Common.hpp>
#include <Unreal/Hooks/Internal/Constants.hpp>
namespace RC::Unreal {struct FFrame;}
namespace RC::Unreal::Hook::Internal {
template<EDetourTarget,typename> class TDetourInstance;
// Matched Native consumers may inspect only this live exact POST frame. The
// pointer never crosses the Lua API; no stored frame is an admission proof.
auto RC_UE_API CurrentWitchItStoreStaticsFrame() noexcept -> FFrame*;
auto RC_UE_API CurrentWitchItStoreStaticsSerial() noexcept -> std::uint64_t;
// Actual detour-stack lifetime, including nested masking. Never consumer-made.
class NativeScriptScope final {
    template<EDetourTarget,typename> friend class TDetourInstance;
#if defined(UE4SS_NATIVE_SCRIPT_SCOPE_TEST)
    friend struct NativeScriptScopeTestAccess;
#endif
    friend auto RC_UE_API CurrentWitchItStoreStaticsFrame() noexcept -> FFrame*;
    friend auto RC_UE_API CurrentWitchItStoreStaticsSerial() noexcept -> std::uint64_t;
    static thread_local NativeScriptScope* current_;
    static thread_local std::uint64_t next_;
    std::uint64_t serial_{};
    NativeScriptScope* previous_;
    FFrame* frame_;
    bool post_{};
    explicit NativeScriptScope(FFrame* frame) noexcept:previous_(current_),frame_(frame) {
        if(frame_){serial_=next_==UINT64_MAX ? 0 : ++next_;current_=this;}
    }
    void post() noexcept {post_=true;}
public:
    ~NativeScriptScope(){if(frame_)current_=previous_;}
    NativeScriptScope(const NativeScriptScope&)=delete;
};
}
