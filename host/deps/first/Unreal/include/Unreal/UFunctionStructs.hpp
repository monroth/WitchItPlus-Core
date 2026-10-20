#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <Unreal/Common.hpp>
#include <Unreal/FFrame.hpp>
#include <map>
#include <atomic>

namespace RC::Unreal
{
    class UObject;

    using UnrealScriptFunction = void(*)(UObject* Context, FFrame& TheStack, void* RESULT_DECL);
    using CallbackId = int32_t;
    static constexpr CallbackId InvalidCallbackId = -1;
    enum class WitchItWriterPhase { BeforeSerialize, BeforePublish, Complete };
    enum class WitchItPublicationState { NotPublished, Committed, RolledBack, Uncertain };
    enum class WitchItRecoveryState { Unavailable, Ready, Writing, Uncertain };
    struct WitchItRecoverySnapshot {
        WitchItRecoveryState state{WitchItRecoveryState::Unavailable};
        std::string session,incident,target,source,resolved,proof;
        std::uint64_t revision{},serial{},generation{};
    };
    auto RC_UE_API InitializeWitchItWriterRecovery() noexcept -> bool;
    // Cached-only queries: no disk scan on status, frame or Lua heartbeat.
    auto RC_UE_API GetWitchItWriterRecoverySnapshot(WitchItRecoverySnapshot&) noexcept -> bool;
    auto RC_UE_API ResolveWitchItRestoredOriginal(const std::string& Incident,std::uint64_t Revision) noexcept -> bool;
    auto RC_UE_API VerifyWitchItWriterRecoveryResolution(const std::string& Incident,const std::string& Session,std::uint64_t Revision,const std::string& Proof) noexcept -> bool;
    auto RC_UE_API VerifyWitchItWriterRecoveryReady(const std::string& Session,std::uint64_t Revision) noexcept -> bool;
    // Host-owned unresolved receipts survive Native consumer/VM retirement.
    auto RC_UE_API GetWitchItUnresolvedWrite(StringType& Target,StringType& Backup) noexcept -> bool;

    class RC_UE_API UnrealScriptFunctionCallableContext
    {
    public:
        UObject* Context;
        FFrame& TheStack;
        void* RESULT_DECL;
        // CallableId is set dynamically by FirePreCallbacks for each callback, not by 'UnrealScriptFunctionHook'.
        CallbackId CallableId{InvalidCallbackId};

        UnrealScriptFunctionCallableContext(UObject* Context, FFrame& TheStack, void* RESULT_DECL);

        // Only the physical native PRE context, and only the two exact bool
        // WitchIt layout writers. Context layout/vtable remain unchanged.
        auto DenyNativeWriterCall() noexcept -> bool;
        using WitchItWriterValidator=std::function<void(UnrealScriptFunctionCallableContext&,WitchItWriterPhase,bool&)>;
        auto UseWitchItWriterStatics(const void* ExpectedStatics,const void* OwnedStatics) -> bool;
        auto PrepareWitchItWriter(const void* ExpectedStatics,const void* OwnedStatics,const WitchItWriterValidator&,std::uint64_t Serial,std::uint64_t Generation) -> bool;
        // Called only inside the managed final validator, after the host has
        // validated the temporary native file and all POST observers completed.
        auto CommitWitchItWriterFile() noexcept -> bool;
        auto GetWitchItWriterOutcome(WitchItPublicationState&,StringType& Target,StringType& Backup) const noexcept -> bool;

        template<typename ReturnType>
        auto SetReturnValue(ReturnType NewValue) -> void
        {
            *static_cast<ReturnType*>(RESULT_DECL) = NewValue;
        }

        template<typename ParamStruct>
        auto GetParams() -> ParamStruct&
        {
            return *static_cast<ParamStruct*>(static_cast<void*>(TheStack.Locals()));
        }
    };
    using UnrealScriptFunctionCallable = std::function<void(UnrealScriptFunctionCallableContext& Context, void* CustomData)>;

    struct UnrealScriptCallbackData
    {
        UnrealScriptFunctionCallable Callable;
        void* CustomData;
        std::atomic<bool> UnregistrationRequested{};
        UObject* FireOnInstance{};

        UnrealScriptCallbackData() = default;
        UnrealScriptCallbackData(UnrealScriptFunctionCallable Callable, void* CustomData, UObject* FireOnInstance) : Callable(Callable), CustomData(CustomData), FireOnInstance(FireOnInstance) {};
    };

    class RC_UE_API UnrealScriptFunctionData
    {
    private:
        UnrealScriptFunction OriginalFunc;
        std::map<int32_t, UnrealScriptCallbackData> PreCallbacks;
        std::map<int32_t, UnrealScriptCallbackData> PostCallbacks;

    private:
        static std::atomic<CallbackId> HookIndexCounter;

    public:
        std::atomic<bool> bIsMidExecution{};

    public:
        UnrealScriptFunctionData(UnrealScriptFunction OriginalFuncPtr);
        inline auto GetOriginalFuncPtr() const -> UnrealScriptFunction { return OriginalFunc; }

        CallbackId AddPreCallback(const UnrealScriptFunctionCallable& Callable, void* CustomData = nullptr, UObject* FireOnInstance = nullptr);
        CallbackId AddPostCallback(const UnrealScriptFunctionCallable& Callable, void* CustomData = nullptr, UObject* FireOnInstance = nullptr);
        bool RemoveCallback(CallbackId CallbackId);

        void RemoveAllCallbacks();

        void FirePreCallbacks(UnrealScriptFunctionCallableContext& Context);
        void FirePostCallbacks(UnrealScriptFunctionCallableContext& Context);

        UnrealScriptCallbackData* GetCallbackData(CallbackId);

        static CallbackId MakeNewId();
    };

    namespace Internal {
        using HookedUFunctionMap = std::unordered_map<const UFunction*, UnrealScriptFunctionData>;
        auto RC_UE_API GetHookedFunctionsMap() -> HookedUFunctionMap&;

        auto UnrealScriptFunctionHook(UObject* Context, FFrame& TheStack, void* RESULT_DECL) -> void;
        // Caller is an exact still-live SaveLayout frame held by its producer;
        // no frame or context pointer is ever returned to Lua.
        auto RC_UE_API DenyNativeWriterCallForCaller(const FFrame* Caller) noexcept -> bool;
    }
}


