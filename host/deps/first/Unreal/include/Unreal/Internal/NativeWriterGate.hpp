#pragma once
namespace RC::Unreal {class UObject;struct FFrame;}
namespace RC::Unreal::Internal {
auto UnrealScriptFunctionHook(UObject*,FFrame&,void*) -> void;
// HOST INTERNAL. Only the physical native hook owns this scope. No payload,
// resolver, VM/root selection or consumer-constructed admission authority.
class NativeWriterGate final {
    friend auto UnrealScriptFunctionHook(UObject*,FFrame&,void*) -> void;
#if defined(UE4SS_NATIVE_WRITER_GATE_TEST)
    friend struct NativeWriterGateTestAccess;
#endif
    static thread_local NativeWriterGate* current_;
    NativeWriterGate* previous_;
    const void* context_;
    const void* caller_;
    bool eligible_;
    bool pre_{true};
    bool denied_{};
    NativeWriterGate(const void* context,const void* caller,bool eligible) noexcept
        :previous_(current_),context_(context),caller_(caller),eligible_(eligible){current_=this;}
public:
    ~NativeWriterGate(){current_=previous_;}
    NativeWriterGate(const NativeWriterGate&)=delete;
    NativeWriterGate& operator=(const NativeWriterGate&)=delete;
    static bool deny(const void* context,const void* caller=nullptr) noexcept {
        auto* gate=current_;
        if(!gate || !gate->eligible_ || !gate->pre_ || (!context && !caller)
            || (context && context!=gate->context_) || (caller && caller!=gate->caller_))return false;
        gate->denied_=true;return true;
    }
    bool denied() const noexcept {return denied_;}
    static bool accepts(const void* context,bool include_denied=false) noexcept {
        return current_ && current_->eligible_ && current_->pre_ && (include_denied || !current_->denied_) && current_->context_==context;
    }
    void post() noexcept {pre_=false;}
};
}
