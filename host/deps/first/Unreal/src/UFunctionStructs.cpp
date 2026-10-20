#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/Internal/NativeWriterGate.hpp>
#include <Unreal/Hooks/Internal/NativeScriptScope.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <DynamicOutput/Output.hpp>
#include <Helpers/Format.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/Core/HAL/UnrealMemory.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/Hooks/Internal/CallbackOwnerBridge.hpp>
#include <windows.h>
#include <Unreal/Internal/WriterRecoveryStore.hpp>
#include <set>
#include <vector>
#include <memory>
#if defined(_MSC_VER)
#include <intrin.h>
#define UE4SS_WIR_WRITER_REGISTRATION_ORIGIN _ReturnAddress()
#else
#define UE4SS_WIR_WRITER_REGISTRATION_ORIGIN __builtin_extract_return_addr(__builtin_return_address(0))
#endif
#if defined(UE4SS_CAULDRON_ROUNDTRIP_DIAGNOSTIC)
#include <Unreal/Internal/WitchItMetadataPayload.hpp>
#endif
#include "WitchItWriterTransaction.inl"

namespace RC::Unreal
{
    thread_local Hook::Internal::NativeScriptScope* Hook::Internal::NativeScriptScope::current_{};
    thread_local std::uint64_t Hook::Internal::NativeScriptScope::next_{};
    auto Hook::Internal::CurrentWitchItStoreStaticsSerial() noexcept -> std::uint64_t {
        return Hook::Internal::CurrentWitchItStoreStaticsFrame() ? NativeScriptScope::current_->serial_ : 0;
    }
    auto Hook::Internal::CurrentWitchItStoreStaticsFrame() noexcept -> FFrame* {
        auto* scope=NativeScriptScope::current_;
        if(!scope || !scope->post_ || !scope->frame_ || !IsInGameThreadRaw())return nullptr;
        try {
            auto* frame=scope->frame_;
            return frame->Node() && frame->Node()->GetPathName()==STR("/Game/Blueprints/Match/BFL_MapLayoutLoading.BFL_MapLayoutLoading_C:StoreStatics")
                ? frame : nullptr;
        } catch(...) {return nullptr;}
    }
    thread_local Internal::NativeWriterGate* Internal::NativeWriterGate::current_{};
    auto UnrealScriptFunctionCallableContext::DenyNativeWriterCall() noexcept -> bool {
        return Internal::NativeWriterGate::deny(this);
    }
    auto Internal::DenyNativeWriterCallForCaller(const FFrame* Caller) noexcept -> bool {
        return Internal::NativeWriterGate::deny(nullptr,Caller);
    }
    std::atomic<CallbackId> UnrealScriptFunctionData::HookIndexCounter{1};

    UnrealScriptFunctionData::UnrealScriptFunctionData(UnrealScriptFunction OriginalFuncPtr) {
        this->OriginalFunc = OriginalFuncPtr;
    }

    CallbackId UnrealScriptFunctionData::AddPreCallback(const UnrealScriptFunctionCallable& Callable, void* CustomData, UObject* FireOnInstance)
    {
        CallbackId NewCallbackId = MakeNewId();
        this->PreCallbacks.emplace(std::piecewise_construct, std::forward_as_tuple(NewCallbackId), std::forward_as_tuple(Callable, CustomData, FireOnInstance));
        return NewCallbackId;
    }

    CallbackId UnrealScriptFunctionData::AddPostCallback(const UnrealScriptFunctionCallable& Callable, void* CustomData, UObject* FireOnInstance)
    {
        CallbackId NewCallbackId = MakeNewId();
        this->PostCallbacks.emplace(std::piecewise_construct, std::forward_as_tuple(NewCallbackId), std::forward_as_tuple(Callable, CustomData, FireOnInstance));
        return NewCallbackId;
    }

    bool UnrealScriptFunctionData::RemoveCallback(CallbackId CallbackId)
    {
        auto CallbackData = GetCallbackData(CallbackId);
        if (CallbackData)
        {
            if (bIsMidExecution)
            {
                // Hook will be unregistered after the callback is done executing.
                CallbackData->UnregistrationRequested = true;
                return true;
            }
            else
            {
                return PreCallbacks.erase(CallbackId) || PostCallbacks.erase(CallbackId);
            }
        }
        else
        {
            return false;
        }
    }

    void UnrealScriptFunctionData::RemoveAllCallbacks()
    {
        for (auto PreCallbackIterator = PreCallbacks.begin(); PreCallbackIterator != PreCallbacks.end();)
        {
            if (bIsMidExecution)
            {
                // Hook will be unregistered after the callback is done executing.
                PreCallbackIterator->second.UnregistrationRequested = true;
                ++PreCallbackIterator;
            }
            else
            {
                PreCallbackIterator = PreCallbacks.erase(PreCallbackIterator);
            }
        }

        for (auto PostCallbackIterator = PostCallbacks.begin(); PostCallbackIterator != PostCallbacks.end();)
        {
            if (bIsMidExecution)
            {
                // Hook will be unregistered after the callback is done executing.
                PostCallbackIterator->second.UnregistrationRequested = true;
                ++PostCallbackIterator;
            }
            else
            {
                PostCallbackIterator = PostCallbacks.erase(PostCallbackIterator);
            }
        }
    }

    void UnrealScriptFunctionData::FirePreCallbacks(UnrealScriptFunctionCallableContext& Context)
    {
        for (auto PreCallbackIterator = PreCallbacks.begin(); PreCallbackIterator != PreCallbacks.end();)
        {
            // Skip callbacks that don't match the instance filter
            if (PreCallbackIterator->second.FireOnInstance && PreCallbackIterator->second.FireOnInstance != Context.Context)
            {
                ++PreCallbackIterator;
                continue;
            }
            // Skip callbacks that have been marked for unregistration or have invalid Callable
            if (PreCallbackIterator->second.UnregistrationRequested || !PreCallbackIterator->second.Callable)
            {
                PreCallbackIterator = PreCallbacks.erase(PreCallbackIterator);
                continue;
            }
            Context.CallableId = PreCallbackIterator->first;
            PreCallbackIterator->second.Callable(Context, PreCallbackIterator->second.CustomData);
            if (PreCallbackIterator->second.UnregistrationRequested)
            {
                PreCallbackIterator = PreCallbacks.erase(PreCallbackIterator);
            }
            else
            {
                ++PreCallbackIterator;
            }
        }
    }

    void UnrealScriptFunctionData::FirePostCallbacks(UnrealScriptFunctionCallableContext& Context)
    {
        for (auto PostCallbackIterator = PostCallbacks.begin(); PostCallbackIterator != PostCallbacks.end();)
        {
            // Skip callbacks that have been marked for unregistration or have invalid Callable
            if (PostCallbackIterator->second.UnregistrationRequested || !PostCallbackIterator->second.Callable)
            {
                PostCallbackIterator = PostCallbacks.erase(PostCallbackIterator);
                continue;
            }
            Context.CallableId = PostCallbackIterator->first;
            PostCallbackIterator->second.Callable(Context, PostCallbackIterator->second.CustomData);
            if (PostCallbackIterator->second.UnregistrationRequested)
            {
                PostCallbackIterator = PostCallbacks.erase(PostCallbackIterator);
            }
            else
            {
                ++PostCallbackIterator;
            }
        }
    }

    UnrealScriptCallbackData* UnrealScriptFunctionData::GetCallbackData(CallbackId CallbackId)
    {
        auto PreIterator = PreCallbacks.find(CallbackId);
        if (PreIterator != PreCallbacks.end()) { return &PreIterator->second; }

        auto PostIterator = PostCallbacks.find(CallbackId);
        if (PostIterator != PostCallbacks.end()) { return &PostIterator->second; }

        return nullptr;
    }

    CallbackId UnrealScriptFunctionData::MakeNewId()
    {
        return HookIndexCounter++;
    }

    UnrealScriptFunctionCallableContext::UnrealScriptFunctionCallableContext(UObject* Context, FFrame& TheStack, void* RESULT_DECL)
            : Context(Context),
              TheStack(TheStack),
              RESULT_DECL(RESULT_DECL) {}

    static Internal::HookedUFunctionMap GHookedScriptFunctions{};

    auto Internal::GetHookedFunctionsMap() -> HookedUFunctionMap&
    {
        return GHookedScriptFunctions;
    }

    auto Internal::UnrealScriptFunctionHook(UObject* Context, FFrame& TheStack, void* RESULT_DECL) -> void
    {
        try
        {
            HookedUFunctionMap& FunctionMap = GetHookedFunctionsMap();
            auto FunctionBeingExecuted = TheStack.CurrentNativeFunction() ? TheStack.CurrentNativeFunction() : *std::bit_cast<UFunction**>(&TheStack.Code()[0-sizeof(uint64)]);
            auto Iterator = FunctionMap.find(FunctionBeingExecuted);
            if (Iterator == FunctionMap.end() && TheStack.Code())
            {
                // TODO: Is &Code[0-sizeof(uint64)] always a valid pointer to access ?
                //       We don't care if the deref ends up being garbage, the map lookup will fail and save us.
                //       But if the address of Code-8 is invalid, that will cause a crash.
                //       There has to be a better way to do this surely ?
                FunctionBeingExecuted = *std::bit_cast<UFunction**>(&TheStack.Code()[0-sizeof(uint64)]);
                Iterator = FunctionMap.find(FunctionBeingExecuted);
            }

            if (Iterator == FunctionMap.end())
            {
                // For some reason, CurrentNativeFunction isn't hooked.
                // This could be due to a bug.
                // Either way, if CurrentNativeFunction is nullptr, it might be safe to call ProcessInternal.
                // If CurrentNativeFunction is non-nullptr, then we call whatever is stored in CurrentNativeFunction.Func.
                if (FunctionBeingExecuted)
                {
                    Output::send<LogLevel::Warning>(STR("Tried to execute UFunction::FuncPtr hook but there was no function map entry for UFunction:\n\t{:016X} {}.\n\tExecuting original function instead.\n"),
                                                    std::bit_cast<uintptr_t>(FunctionBeingExecuted),
                                                    FunctionBeingExecuted ? FunctionBeingExecuted->GetFullName() : STR("None"));
                    FunctionBeingExecuted->GetFuncPtr()(Context, TheStack, RESULT_DECL);
                }
                else
                {
                    Output::send<LogLevel::Warning>(STR("Tried to execute UFunction::FuncPtr hook but there was no function map entry for UFunction:\n\t{:016X} {}.\n\tNo original function to execute.\n\tExecuting ProcessInternal instead.\n"),
                                                    std::bit_cast<uintptr_t>(FunctionBeingExecuted),
                                                    FunctionBeingExecuted ? FunctionBeingExecuted->GetFullName() : STR("None"));
                    static auto Object_ExecuteUbergraph = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/CoreUObject.Object:ExecuteUbergraph"));
                    static auto ProcessInternal = Object_ExecuteUbergraph->GetFuncPtr();
                    Output::send(STR("Object_ExecuteUbergraph: {}\n"), (void*)Object_ExecuteUbergraph);
                    Output::send(STR("ProcessInternal: {}\n"), (void*)ProcessInternal);
                    ProcessInternal(Context, TheStack, RESULT_DECL);
                }
            }
            else
            {
                FFrame* NewStackPtr = &TheStack;
                std::map<FProperty*, FOutParmRec*> OriginalOutParmsMap; // Map to track original out params

                if (TheStack.Code() && GNatives_Internal)
                {
                    // Create a new stack frame
                    NewStackPtr = FFrame::MallocAndMemsetNewFrame();
                    NewStackPtr->Object() = Context;
                    NewStackPtr->Node() = FunctionBeingExecuted;
                    NewStackPtr->PreviousFrame() = &TheStack;
                    if (Version::IsBelow(4, 25))
                    {
                        NewStackPtr->PropertyChainForCompiledIn() = std::bit_cast<FField*>(FunctionBeingExecuted->GetChildren().Get());
                    }
                    else
                    {
                        NewStackPtr->PropertyChainForCompiledIn() = FunctionBeingExecuted->GetChildProperties();
                    }
                    auto NewLocals = static_cast<uint8*>(alloca(FunctionBeingExecuted->GetParmsSize()));
                    NewStackPtr->Locals() = NewLocals;

                    // Map out parameters for later use
                    FOutParmRec* CurrentOutParm = TheStack.OutParms();
                    while (CurrentOutParm)
                    {
                        OriginalOutParmsMap[CurrentOutParm->Property] = CurrentOutParm;
                        CurrentOutParm = CurrentOutParm->NextOutParm;
                    }

                    // Loop through parameters and evaluate them
                    for (FProperty* prop : TFieldRange<FProperty>(FunctionBeingExecuted, EFieldIterationFlags::IncludeDeprecated))
                    {
                        // Skip non parameters and return values
                        if (!prop->HasAnyPropertyFlags(CPF_Parm) || prop->HasAnyPropertyFlags(CPF_ReturnParm)) 
                            continue;

                        // Initialize property storage
                        uint8* PropertyData = prop->ContainerPtrToValuePtr<uint8>(NewStackPtr->Locals());
                        prop->InitializeValue(PropertyData);

                        if (prop->HasAnyPropertyFlags(CPF_OutParm))
                        {
                            // Create new out parameter slot and add it to the linked list
                            FOutParmRec* NewOutParam = static_cast<FOutParmRec*>(alloca(sizeof(Unreal::FOutParmRec)));
                            NewOutParam->Property = prop;
                            NewOutParam->NextOutParm = NewStackPtr->OutParms();
                            NewStackPtr->OutParms() = NewOutParam;

                            // Step the original stack to get the address for the out param
                            TheStack.StepCompiledIn(PropertyData, prop);
                            NewOutParam->PropAddr = TheStack.MostRecentPropertyAddress();
                        }
                        else
                        {
                            // Regular parameter - evaluate it from the original stack
                            TheStack.StepCompiledIn(PropertyData, prop);
                        }
                    }

                    // Code pointer needs to move past the parameters
                    TheStack.Code() += !!TheStack.Code();
                }

                // Create function context with our (possibly new) stack frame
                UnrealScriptFunctionCallableContext FuncContext(Context, *NewStackPtr, RESULT_DECL);
                
                Iterator->second.bIsMidExecution = true;
                const auto writer_path=FunctionBeingExecuted->GetPathName();
                const bool exact_writer=writer_path==STR("/Script/PropWitchHuntModule.ProjectVersionBlueprint:SavePropLayoutToFile")
                    || writer_path==STR("/Script/PropWitchHuntModule.ProjectVersionBlueprint:SavePropLayoutToFileCompressed");
                auto writer_result=exact_writer ? CastField<FBoolProperty>(FunctionBeingExecuted->GetReturnProperty()) : nullptr;
                Internal::NativeWriterGate writer_gate{&FuncContext,NewStackPtr->PreviousFrame(),exact_writer && writer_result && RESULT_DECL && IsInGameThreadRaw()};
                Internal::NativeWriterTransactionScope writer_scope{&FuncContext};
                const auto original_function=Iterator->second.GetOriginalFuncPtr();

                try
                {
                    // Fire pre callbacks with all parameters now properly populated
                    Iterator->second.FirePreCallbacks(FuncContext);
                }
                catch (std::exception& e)
                {
                    Output::send(STR("Error executing hook pre-callback {}: {}\n"), 
                                 FunctionBeingExecuted->GetPathName(), ensure_str(e.what()));
                }
                catch (...)
                {
                    if(!exact_writer)throw;
                    Output::send(STR("Error executing WitchIt writer pre-callback: unknown exception\n"));
                }

                // Recheck after ALL PRE observers; serialize only initialized
                // host-owned inputs into the reserved temporary destination.
                if(writer_scope.transaction_ && !writer_scope.transaction_->before())(void)FuncContext.DenyNativeWriterCall();
                bool native_writer_success=false,post_observers_ok=true;
                const bool writer_denied=writer_gate.denied();
                if(!writer_denied) {
                    try {
                        if(writer_scope.transaction_)writer_scope.transaction_->serialize(original_function);
                        else if(writer_scope.input_copy_)writer_scope.input_copy_->serialize(original_function);
                        else original_function(Context,*NewStackPtr,RESULT_DECL);
                        native_writer_success=exact_writer && writer_result && RESULT_DECL && writer_result->GetPropertyValue(RESULT_DECL);
                    } catch(...) {
                        if(exact_writer){(void)FuncContext.DenyNativeWriterCall();if(writer_result && RESULT_DECL)writer_result->SetPropertyValue(RESULT_DECL,false);}
                        else throw;
                    }
                } else if(writer_result && RESULT_DECL)writer_result->SetPropertyValue(RESULT_DECL,false);
                writer_gate.post();writer_scope.pre_=false;
                
                try
                {
                    // POST observers (including FAPP's marker-count cache) see
                    // the same owned inputs the native serializer just received.
                    auto post_callbacks=[&] {
                        if(auto current=FunctionMap.find(FunctionBeingExecuted);current!=FunctionMap.end())current->second.FirePostCallbacks(FuncContext);
                        else post_observers_ok=false;
                    };
                    if(writer_scope.input_copy_)writer_scope.input_copy_->observe(post_callbacks);
                    else post_callbacks();
                }
                catch (std::exception& e)
                {
                    post_observers_ok=false;
                    Output::send(STR("Error executing hook post-callback {}: {}\n"), 
                                 FunctionBeingExecuted->GetPathName(), ensure_str(e.what()));
                }

                // A POST observer cannot turn an intentionally skipped writer
                // into success. No original disk write happened in this scope.
                if((writer_denied || writer_gate.denied()) && writer_result && RESULT_DECL)writer_result->SetPropertyValue(RESULT_DECL,false);

                if(writer_scope.transaction_) {
                    const bool observed=post_observers_ok && RESULT_DECL && writer_result->GetPropertyValue(RESULT_DECL);
                    const bool result=writer_scope.transaction_->finalize(native_writer_success,observed,writer_gate.denied());
                    if(RESULT_DECL)writer_result->SetPropertyValue(RESULT_DECL,result);
                } else if(writer_scope.completion_ && writer_result && RESULT_DECL) {
                    bool result=native_writer_success && !writer_gate.denied() && post_observers_ok && writer_result->GetPropertyValue(RESULT_DECL);
                    const bool actual=result;
                    try{(void)writer_scope.completion_->Invoke(FuncContext,WitchItWriterPhase::Complete,result);}catch(...){}
                    writer_result->SetPropertyValue(RESULT_DECL,actual);
                }

                // Copy out parameter values back to the original frame
                if (TheStack.Code() && GNatives_Internal)
                {
                    // Copy out parameter values back to the original frame
                    for (FOutParmRec* OutParam = NewStackPtr->OutParms(); OutParam; OutParam = OutParam->NextOutParm)
                    {
                        auto OriginalIt = OriginalOutParmsMap.find(OutParam->Property);
                        if (OriginalIt != OriginalOutParmsMap.end())
                        {
                            // Copy the value back to the original out parameter
                            OutParam->Property->CopyCompleteValue(OriginalIt->second->PropAddr, OutParam->PropAddr);
                        }
                    }

                    // Clean up the temporary stack frame
                    for (FProperty* prop : TFieldRange<FProperty>(FunctionBeingExecuted, EFieldIterationFlags::IncludeDeprecated))
                    {
                        if (!prop->HasAnyPropertyFlags(CPF_Parm)) continue;
                        if (prop->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

                        uint8* PropertyData = prop->ContainerPtrToValuePtr<uint8>(NewStackPtr->Locals());
                        prop->DestroyValue(PropertyData);
                    }
                    FMemory::Free(NewStackPtr);
                }
                    
                if(auto current=FunctionMap.find(FunctionBeingExecuted);current!=FunctionMap.end())current->second.bIsMidExecution = false;
            }
        }
        catch (std::exception& e)
        {
            Output::send(STR("Error executing hooked function: {}\n"), ensure_str(e.what()));
        }
    }
}
