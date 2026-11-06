// Staged native producer for UE4SS e3ba1016 + UEPseudo 38e7171e.
// Optional carrier input copying starts after GT attach; vanilla writers stay native.
#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/Output.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/FURL.hpp>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "NativeUpdateFiles.hpp"
#include "NativeUpdater.hpp"
#include "NativeDebug.hpp"
#include "NativeBootControls.hpp"
#include <fstream>
#include "NativeClipboard.hpp"
#include "OperationLedger.hpp"
#include "LoadLedger.hpp"
#include "ScopedGameplay.hpp"
#include "NativeBoundarySinks.hpp"
#include "OwnedSaveStatics.hpp"
#include "SaveMetadataPayload.hpp"
#include "OwnedLocalWorld.hpp"
#include "ShipUE4SSEngine.hpp"
#include "ShipNativeBinding.hpp"
#include <Unreal/Hooks/Internal/NativeScriptScope.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/World.hpp>

namespace {
using namespace RC;
using namespace RC::Unreal;
using Lua=LuaMadeSimple::Lua;
constexpr auto save_path=L"/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C:SaveLayout";
constexpr auto writer_path=L"/Script/PropWitchHuntModule.ProjectVersionBlueprint:SavePropLayoutToFile";
constexpr auto compressed_writer_path=L"/Script/PropWitchHuntModule.ProjectVersionBlueprint:SavePropLayoutToFileCompressed";
constexpr auto load_path=L"/Game/Blueprints/Core/B_GameMode.B_GameMode_C:LoadPropLayout";
constexpr auto creative_load_path=L"/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C:LoadLayout";
constexpr auto base_apply_path=L"/Game/Blueprints/Core/B_GameMode.B_GameMode_C:LoadPropLayoutFromSaveGame";
constexpr auto creative_apply_path=L"/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C:LoadPropLayoutFromSaveGame";
constexpr auto reader_path=L"/Script/PropWitchHuntModule.ProjectVersionBlueprint:LoadPropLayoutFromFile";
constexpr auto compressed_reader_path=L"/Script/PropWitchHuntModule.ProjectVersionBlueprint:LoadPropLayoutFromFileCompressed";

std::string utf8(const StringType& value) {
    if(value.empty())return {};
    int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    if(!count)throw std::runtime_error("invalid native string");
    std::string out(count,'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),count,nullptr,nullptr)!=count)
        throw std::runtime_error("native string conversion failed");
    return out;
}
FProperty* property(UFunction* node,const wchar_t* name) {
    auto p=node->FindProperty(FName(name,FNAME_Find));
    if(!p)throw std::runtime_error("required reflected property missing");
    return p;
}
bool boolean(FFrame& frame,const wchar_t* name) {
    auto p=CastField<FBoolProperty>(property(frame.Node(),name));
    if(!p || !frame.Locals())throw std::runtime_error("required reflected bool missing");
    if(p->HasAnyPropertyFlags(CPF_OutParm)) {
        auto address=FindOutParamValueAddress(frame,p);
        if(!address)throw std::runtime_error("required bool OUT address missing");
        return p->GetPropertyValue(address);
    }
    return p->GetPropertyValueInContainer(frame.Locals());
}
std::string string(FFrame& frame,const wchar_t* name) {
    auto p=CastField<FStrProperty>(property(frame.Node(),name));
    if(!p || !frame.Locals())throw std::runtime_error("required reflected string missing");
    const auto data=p->HasAnyPropertyFlags(CPF_OutParm) ? FindOutParamValueAddress(frame,p)
        : p->ContainerPtrToValuePtr<void>(frame.Locals());
    if(!data)throw std::runtime_error("required string address missing");
    const auto value=static_cast<FString*>(data);
    if(value->Len()<0 || value->Len()>4096 || (value->Len() && !**value))
        throw std::runtime_error("native string outside bounded contract");
    return utf8(StringType(**value,value->Len()));
}
std::string layout_string(FFrame& frame,const wchar_t* member) {
    auto layout=CastField<FStructProperty>(property(frame.Node(),L"Layout"));
    if(!layout || !frame.Locals() || !layout->GetStruct())throw std::runtime_error("exact Layout struct missing");
    auto field=CastField<FStrProperty>(layout->GetStruct()->FindProperty(FName(member,FNAME_Find)));
    if(!field)throw std::runtime_error("exact Layout string field missing");
    const auto container=layout->HasAnyPropertyFlags(CPF_OutParm) ? FindOutParamValueAddress(frame,layout)
        : layout->ContainerPtrToValuePtr<void>(frame.Locals());
    if(!container)throw std::runtime_error("exact Layout address missing");
    const auto value=field->ContainerPtrToValuePtr<FString>(container);
    if(value->Len()<0 || value->Len()>4096 || (value->Len() && !**value))throw std::runtime_error("Layout string outside bounded contract");
    return utf8(StringType(**value,value->Len()));
}
witchit::FrameKey key(FFrame& frame){return reinterpret_cast<witchit::FrameKey>(&frame);}

class NativeProducer;
NativeProducer* instance{};
class NativeProducer final: public CppUserModBase {
    witchit::boot::Controls boot_;
    std::recursive_mutex mutex_; // stop/loader thread cannot race Lua dereference
    witchit::NativeBoundarySinks sinks_;
    std::uint64_t generation_{};
    bool attached_{},registered_{},save_load_started_{},startup_callback_registered_{},unreal_initialized_{},writer_quarantine_ready_{};
    witchit::OperationLedger ledger_;
    std::shared_ptr<witchit::ScopedGameplay> gameplay_;
    witchit::LoadLedger loads_;
    witchit::LoadBarriers load_barriers_;
    witchit::OwnedLocalWorld local_world_;
    using ShipBinding=ship_parts::NativeBinding<ship_parts::UE4SSEngine>;
    std::atomic<std::shared_ptr<ShipBinding>> ship_parts_;
    void retire_ship_parts() noexcept {if(auto held=ship_parts_.exchange({}))held->retire();}
    void ship_parts_before_world() noexcept {if(auto held=ship_parts_.load())held->before_world();}
    struct Intent {std::uintptr_t owner;bool recovery;};
    std::unordered_map<witchit::FrameKey,Intent> intents_;
    std::unordered_map<witchit::FrameKey,witchit::FrameKey> applies_;
    std::vector<Hook::GlobalCallbackId> global_ids_;
    std::vector<witchit::FrameKey> save_scopes_;
    struct SaveScopeExit {
        NativeProducer* owner;witchit::FrameKey frame;
        ~SaveScopeExit(){owner->metadata_.erase(frame);std::erase(owner->save_scopes_,frame);}
    };
    struct NativeHook {UFunction* function;std::pair<int,int> ids;};
    std::vector<NativeHook> native_hooks_;
    FName save_name_,load_name_,intent_name_,apply_name_,store_name_;

    #include "NativeSaveMetadata.inl"

    bool emit_load(const char* phase,const witchit::LoadEntry& e) {
        if(!attached_ || generation_!=e.bridge_generation)return false;
        const auto generation=generation_;
        return sinks_.load(phase,e,[this,generation]{return attached_ && generation_==generation;});
    }
    witchit::FrameKey ancestor_load(FFrame* frame) {
        return load_barriers_.nearest(reinterpret_cast<witchit::FrameKey>(frame),
            [](auto address){return reinterpret_cast<witchit::FrameKey>(
                reinterpret_cast<FFrame*>(address)->PreviousFrame());},
            [this](auto address){return loads_.find(address)!=nullptr;});
    }
    void load_pre(UObject* context,FFrame& frame) {
        auto name=frame.Node()->GetFName();
        if(name!=load_name_ && name!=intent_name_ && name!=apply_name_)return;
        auto path=frame.Node()->GetPathName();
        if(name==intent_name_ && path==creative_load_path) {
            intents_[key(frame)]={reinterpret_cast<std::uintptr_t>(context),boolean(frame,L"IsLoadAfterPlayTest")};return;
        }
        if(name==load_name_ && path==load_path) {
            load_barriers_.enter(key(frame)); // retained through reject/abandon until exact POST
            if(!context || !UObject::IsReal(context) || frame.Code()!=frame.Node()->GetScript().GetData())return;
            bool recovery=false,known=false;
            auto* f=frame.PreviousFrame();
            for(int depth=0;f && !known && depth<64;++depth,f=f->PreviousFrame()) {
                auto it=intents_.find(key(*f));
                if(it!=intents_.end() && it->second.owner==reinterpret_cast<std::uintptr_t>(context)) {
                    recovery=it->second.recovery;known=true;
                }
            }
            auto e=loads_.enter(key(frame),generation_,std::to_string(reinterpret_cast<std::uintptr_t>(context))+"|"+utf8(context->GetFullName()),
                                reinterpret_cast<std::uintptr_t>(context),boolean(frame,L"UseCompression"),recovery);
            if(e){
                e->recovery_intent_known=known;
                e->layout_name=layout_string(frame,L"LayoutName_8_C5C516E347406BABDAF0F59A6150BC5A");
                e->layout_world=layout_string(frame,L"MapName_10_67B8E5E54A16D786C60266AF5B396E36");
                e->layout_path=layout_string(frame,L"FileName_6_F3CC8069497D007AD2E96A9AE00D2700");
                if(auto live=loads_.find(key(frame))){
                    live->layout_name=e->layout_name;live->layout_world=e->layout_world;live->layout_path=e->layout_path;
                    live->recovery_intent_known=known;
                }
                bool captured=emit_load("entry",*e);
                if(auto live=loads_.find(key(frame)))live->captured=captured;
                if(!captured)Output::send(STR("[WIR] native load companion unavailable serial={} layout={} world={} path={}; vanilla load continues\n"),
                    e->serial,ensure_str(e->layout_name),ensure_str(e->layout_world),ensure_str(e->layout_path));
            }
            return;
        }
        if(name==apply_name_ && (path==base_apply_path || path==creative_apply_path)) {
            auto outer=ancestor_load(frame.PreviousFrame());auto e=loads_.find(outer);if(!e)return;
            if(loads_.apply_enter(outer,key(frame),reinterpret_cast<std::uintptr_t>(context)))applies_[key(frame)]=outer;
        }
    }
    void load_post(FFrame& frame) {
        if(!witchit::debug::enabled("native_boundary_hooks"))return;
        load_barriers_.leave(key(frame));
        intents_.erase(key(frame));
        auto apply=applies_.find(key(frame));
        if(apply!=applies_.end()) {
            auto outer=apply->second;applies_.erase(apply);
            if(loads_.find(outer)) {
                loads_.apply_return(outer,key(frame));
            }
            return;
        }
        auto live=loads_.find(key(frame));if(!live)return;
        auto completed=loads_.finish(key(frame),generation_);
        if(completed)emit_load("return",*completed); // facts only: not all actors/destination-world ready
    }
    void reader(UnrealScriptFunctionCallableContext& call,bool compressed,bool post) {
        std::lock_guard lock(mutex_);
        if(!witchit::debug::enabled("native_boundary_hooks") || !attached_ || !IsInGameThreadRaw())return;
        auto* caller=call.TheStack.PreviousFrame();if(!caller)return;
        auto live=loads_.find(key(*caller));if(!live)return;
        auto held=*live;

        auto path=string(call.TheStack,L"AbsoluteFileName");
        if(!post) {
            if(path!=held.layout_path || !loads_.reader(key(*caller),path,compressed))loads_.abandon(key(*caller));
        } else {
            auto returned=CastField<FBoolProperty>(property(call.TheStack.Node(),L"ReturnValue"));
            if(!returned || !call.RESULT_DECL)throw std::runtime_error("actual native reader return missing");
            if(!loads_.read(key(*caller),path,returned->GetPropertyValue(call.RESULT_DECL)))loads_.abandon(key(*caller));
        }
    }

    bool emit(const char* phase,const witchit::SaveEntry& e,const std::string& target={},bool success=false) {
        if(!attached_ || generation_!=e.bridge_generation)return false;
        const auto generation=generation_;
        return sinks_.save(phase,e,target,success,[this,generation]{return attached_ && generation_==generation;});
    }
    void script_pre(UObject* context,FFrame& frame) {
        std::lock_guard lock(mutex_);
        if(!witchit::debug::enabled("native_boundary_hooks") || !attached_ || !IsInGameThreadRaw() || !frame.Node())return;
        load_pre(context,frame);
        if(frame.Node()->GetFName()!=save_name_)return;
        if(frame.Node()->GetPathName()!=save_path)return;
        // Only entry before any original instruction; no guessed locals offset.
        auto& script=frame.Node()->GetScript();
        if(!context || !UObject::IsReal(context) || !frame.Locals() || frame.Code()!=script.GetData())return;
        save_scopes_.push_back(key(frame));
        auto entry=ledger_.enter(key(frame),generation_,std::to_string(reinterpret_cast<std::uintptr_t>(context))+"|"+utf8(context->GetFullName()),
                                 string(frame,L"LayoutName"),boolean(frame,L"UseCompression"),boolean(frame,L"IsAutoSave"));
        if(!entry)return;
        bool captured=emit("entry",*entry);
        ledger_.captured(key(frame),entry->serial,captured);
    }
    void script_post(FFrame& frame) {
        std::lock_guard lock(mutex_);
        if(!IsInGameThreadRaw())return;
        SaveScopeExit exit{this,key(frame)};
        load_post(frame);
        auto live=ledger_.find(key(frame));if(!live)return;
        auto held=*live;
        // Exact active frame and bridge generation own physical file facts.

        auto completed=ledger_.finish(key(frame),generation_,string(frame,L"ItemFileName"),boolean(frame,L"IsSuccess"),
                                      string(frame,L"SafeLayoutName"));
        if(completed)emit("complete",*completed,completed->actual_target,completed->native_success);
        else emit("abandon",held);
    }
    void writer(UnrealScriptFunctionCallableContext& call,bool compressed,bool post) {
        std::lock_guard lock(mutex_);
        if(!witchit::debug::enabled("native_boundary_hooks") || !attached_ || !IsInGameThreadRaw())return;
        auto* caller=call.TheStack.PreviousFrame();if(!caller)return;
        if(std::find(save_scopes_.begin(),save_scopes_.end(),key(*caller))==save_scopes_.end())return;
        const auto frame=key(*caller);auto* live=ledger_.find(frame);if(!live)return;
        auto held=*live;
        const auto target=string(call.TheStack,L"AbsoluteFileName");
        if(!post) {
            if(target!=string(*caller,L"ItemFileName") || !ledger_.writer(frame,held.serial,target,compressed)) {
                ledger_.abandon(frame);emit("abandon",held);return;
            }
            // Companion acceptance is only a Core receipt. It never controls IO.
            const bool admitted=emit("before_write",held,target);
            ledger_.admitted(frame,held.serial,admitted);
            auto metadata=metadata_.find(frame);
            if(metadata!=metadata_.end() && metadata->second.rows) {
                const auto& rows=*metadata->second.rows;
                if(!call.UseWitchItWriterStatics(rows.source(),rows.owned()))
                    Output::send(STR("[WIR] native carrier input copy unavailable; vanilla save continues\n"));
            }
        } else {
            auto* returned=CastField<FBoolProperty>(property(call.TheStack.Node(),L"ReturnValue"));
            if(!returned || !call.RESULT_DECL)throw std::runtime_error("actual native writer return missing");
            const bool success=returned->GetPropertyValue(call.RESULT_DECL);
            if(!ledger_.written(frame,held.serial,target,success)){ledger_.abandon(frame);emit("abandon",held);}
        }
    }
    static int attach(const Lua& lua) {
        if(!instance || !IsInGameThreadRaw()){lua.set_bool(false);return 1;}
        if(!instance->boot_.enabled("file_bridge")){Output::send(STR("[WIR] Producer boot: skip file bridge attach\n"));lua.set_bool(false);return 1;}
        std::lock_guard lock(instance->mutex_);
        const bool initialized_before=instance->save_load_started_;
        instance->initialize_save_load_on_game_thread();
        const bool callbacks=instance->registered_ && instance->sinks_.attach(witchit::uikit::resolve_host_values());
        Output::send(STR("[WIR] native SaveLoad attach: initialized_before={} native_routes={} callbacks={} boundary_hooks={} metadata={}\n"),
            initialized_before,instance->native_hooks_.size(),callbacks,witchit::debug::enabled("native_boundary_hooks"),witchit::debug::enabled("native_metadata"));
        if(!callbacks){lua.set_bool(false);return 1;}
        instance->ledger_.retire();instance->loads_.retire();instance->load_barriers_.retire();instance->intents_.clear();instance->applies_.clear();instance->save_scopes_.clear();instance->metadata_.clear();
        ++instance->generation_;instance->attached_=true;
        lua.set_bool(true);lua.set_integer(instance->generation_);return 2;
    }
    static int detach_bridge(const Lua& lua) {
        if(!instance || !IsInGameThreadRaw()){lua.set_bool(false);return 1;}
        std::lock_guard lock(instance->mutex_);
        if(!lua.is_integer() || lua.get_integer()!=instance->generation_){lua.set_bool(false);return 1;}
        instance->detach();lua.set_bool(true);return 1;
    }
    static const char* recovery_state(WitchItRecoveryState state) {
        switch(state){case WitchItRecoveryState::Ready:return "READY";case WitchItRecoveryState::Writing:return "WRITING";
            case WitchItRecoveryState::Uncertain:return "UNCERTAIN";default:return "UNAVAILABLE";}
    }
    static int recovery_snapshot(const Lua& lua,bool unresolved_only) {
        if(!instance || !IsInGameThreadRaw()){lua.set_nil();return 1;}
        try {
            std::lock_guard lock(instance->mutex_);const auto* api=witchit::uikit::resolve_host_values();
            if(!api || !instance->sinks_.bound_current()){lua.set_nil();return 1;}
            witchit::uikit::HostStack stack(api);WitchItRecoverySnapshot value;
            if(stack.top()!=0 || !GetWitchItWriterRecoverySnapshot(value)){stack.nil();return 1;}
            if(unresolved_only && value.state==WitchItRecoveryState::Ready){stack.nil();return 1;}
            stack.table();const auto t=stack.top();
            auto text=[&](const char* key,const std::string& v){stack.string(v);stack.set_field(t,key);};
            auto number=[&](const char* key,std::uint64_t v){if(v>INT64_MAX)throw std::runtime_error("recovery counter outside Lua range");stack.integer(v);stack.set_field(t,key);};
            number("version",1);text("state",recovery_state(value.state));text("session",value.session);text("incident",value.incident);
            text("target_path",value.target);text("backup_path",value.source);text("resolved_incident",value.resolved);text("resolution_proof",value.proof);
            number("revision",value.revision);number("serial",value.serial);number("native_generation",value.generation);return 1;
        }catch(...){lua.set_nil();return 1;}
    }
    static int writer_recovery(const Lua& lua){return recovery_snapshot(lua,true);}
    static int writer_recovery_snapshot(const Lua& lua){return recovery_snapshot(lua,false);}
    static int recovery_check(const Lua& lua,unsigned kind) {
        if(!instance || !IsInGameThreadRaw()){lua.set_bool(false);return 1;}
        try {
            std::lock_guard lock(instance->mutex_);const auto* api=witchit::uikit::resolve_host_values();
            if(!api || !instance->sinks_.bound_current()){lua.set_bool(false);return 1;}
            witchit::uikit::HostStack stack(api);const int count=kind==2?4:2;
            if(stack.top()!=count){lua.set_bool(false);return 1;}
            auto first=stack.get_string(1);auto revision=stack.read(kind==2?3:2);
            if(!first || first->empty() || first->size()>256 || revision.kind!=UE4SS_LUA_VALUE_INTEGER || revision.integer_value<=0){lua.set_bool(false);return 1;}
            bool ok=false;
            if(kind==0)ok=ResolveWitchItRestoredOriginal(*first,revision.integer_value);
            else if(kind==1)ok=VerifyWitchItWriterRecoveryReady(*first,revision.integer_value);
            else {auto session=stack.get_string(2),proof=stack.get_string(4);
                if(session && proof && !session->empty() && !proof->empty() && session->size()<=256 && proof->size()<=256)
                    ok=VerifyWitchItWriterRecoveryResolution(*first,*session,revision.integer_value,*proof);}
            lua.set_bool(ok && instance->sinks_.bound_current());return 1;
        }catch(...){lua.set_bool(false);return 1;}
    }
    static int recovery_resolve(const Lua& lua){return recovery_check(lua,0);}
    static int recovery_ready(const Lua& lua){return recovery_check(lua,1);}
    static int recovery_verified(const Lua& lua){return recovery_check(lua,2);}
    void detach() {attached_=false;++generation_;ledger_.retire();loads_.retire();load_barriers_.retire();intents_.clear();applies_.clear();save_scopes_.clear();metadata_.clear();sinks_.stop();}
public:
    NativeProducer(){instance=this;ModName=STR("WitchItNativeProducer");ModVersion=STR("0.1.0-stage");ModDescription=STR("Exact-frame save boundaries; runtime validation pending");ModAuthors=STR("monroth");
        const auto path=witchit::update_files::directory()/L"Mods"/L"Core"/L"source-geometry-debug.json";
        try {
            std::error_code ec;const bool exists=std::filesystem::exists(path,ec);
            if(ec)throw std::runtime_error("Producer boot diagnostic availability unknown");
            if(exists){std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Producer boot diagnostic read failed");
                std::string bytes(16385,'\0');input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));bytes.resize(static_cast<std::size_t>(input.gcount()));
                if(input.bad())throw std::runtime_error("Producer boot diagnostic read failed");boot_=witchit::boot::Controls::parse(bytes);}
            Output::send(STR("[WIR] Producer boot diagnostics: file={} disabled_count={}\n"),path.wstring(),boot_.disabled.size());
        }catch(const std::exception& e){boot_.disabled={"all"};Output::send(STR("[WIR] Producer boot diagnostics invalid; skip all native activations: {}\n"),ensure_str(e.what()));}
        for(const auto& name:witchit::boot::Controls::names)if(!boot_.enabled(name.c_str()))Output::send(STR("[WIR] Producer boot: skip {}\n"),ensure_str(name));
    }
    bool prepare_uninstall() override {
        if(!gameplay_)return true;
        gameplay_->stop();gameplay_->drain();
        if(!gameplay_->cleanup_complete())return false;
        gameplay_.reset();return true;
    }
    ~NativeProducer() override {
        witchit::updater::stop();
        retire_ship_parts();
        if(gameplay_){gameplay_->stop();gameplay_->drain();gameplay_.reset();}
        local_world_.stop();
        {std::lock_guard lock(mutex_);detach();}
        for(auto id:global_ids_)Hook::UnregisterCallback(id);
        for(const auto& h:native_hooks_)UObjectGlobals::UnregisterHook(h.function,h.ids);
        instance=nullptr;
    }
    Hook::FScriptNameFilter save_script_filter() const {
        return {{static_cast<std::int32_t>(save_name_.GetComparisonIndex().ToUnstableInt()),static_cast<std::int32_t>(load_name_.GetComparisonIndex().ToUnstableInt()),
                 static_cast<std::int32_t>(intent_name_.GetComparisonIndex().ToUnstableInt()),static_cast<std::int32_t>(apply_name_.GetComparisonIndex().ToUnstableInt())}};
    }
    void install_writer_quarantine() {
        // Observe actual serializer PRE/POST; metadata is optional input data.
        // No Core/package/capture failure may veto the native writer.
        unsigned installed=0;
        for(auto [path,compressed]:{std::pair{writer_path,false},std::pair{compressed_writer_path,true}}) {
            auto fn=UObjectGlobals::StaticFindObject<UFunction*>(nullptr,nullptr,path);
            if(!fn || !fn->HasAnyFunctionFlags(EFunctionFlags::FUNC_Native)){
                Output::send(STR("WIR SaveLoad: UNAVAILABLE exact physical writer {}\n"),path);continue;
            }
            try {
                auto ids=UObjectGlobals::RegisterHook(fn,
                    [this,compressed](auto& c,void*){
                        try{writer(c,compressed,false);}catch(...){
                            std::lock_guard lock(mutex_);auto* caller=c.TheStack.PreviousFrame();
                            if(caller)ledger_.abandon(key(*caller));
                            Output::send(STR("[WIR] native save observer unavailable; vanilla save continues\n"));
                        }
                    },
                    [this,compressed](auto& c,void*){writer(c,compressed,true);},nullptr);
                native_hooks_.push_back({fn,ids});++installed;
            }catch(const std::exception& error){
                Output::send(STR("WIR SaveLoad: UNAVAILABLE physical writer quarantine {}: {}\n"),path,ensure_str(error.what()));
            }
        }
        writer_quarantine_ready_=installed==2;
    }
    void initialize_save_load_on_game_thread() {
        if(!boot_.enabled("save_load_startup") || !witchit::debug::enabled("native_boundary_hooks") || !IsInGameThreadRaw())return;
        std::lock_guard lock(mutex_);
        if(save_load_started_)return;
        save_load_started_=true;
        bool saveLoadReady=InitializeWitchItWriterRecovery(); // storage availability, not write permission
        // Preflight managed routes independently of physical writer quarantine.
        // Missing cooked routes must not suppress UIKit/preview initialization.
        const wchar_t* unavailable=nullptr;
        for(auto path:{writer_path,compressed_writer_path,reader_path,compressed_reader_path}){
            auto fn=UObjectGlobals::StaticFindObject<UFunction*>(nullptr,nullptr,path);
            if(!fn || !fn->HasAnyFunctionFlags(EFunctionFlags::FUNC_Native)){unavailable=path;break;}
        }
        if(unavailable){saveLoadReady=false;Output::send(STR("WIR SaveLoad: UNAVAILABLE exact native route {}\n"),unavailable);}
        else if(!writer_quarantine_ready_)Output::send(STR("WIR SaveLoad: UNAVAILABLE physical writer quarantine\n"));
        else if(!saveLoadReady)Output::send(STR("WIR SaveLoad: UNAVAILABLE recovery store\n"));
        saveLoadReady=saveLoadReady && writer_quarantine_ready_;
        // Blueprint names may not yet be in the pool at Unreal initialization.
        save_name_=FName(STR("SaveLayout"),FNAME_Add);
        store_name_=FName(STR("StoreStatics"),FNAME_Add);
        load_name_=FName(STR("LoadPropLayout"),FNAME_Add);
        intent_name_=FName(STR("LoadLayout"),FNAME_Add);
        apply_name_=FName(STR("LoadPropLayoutFromSaveGame"),FNAME_Add);
        if(saveLoadReady){
        for(auto [path,compressed]:{std::pair{reader_path,false},std::pair{compressed_reader_path,true}}) {
            auto fn=UObjectGlobals::StaticFindObject<UFunction*>(nullptr,nullptr,path);
            auto ids=UObjectGlobals::RegisterHook(fn,
                [this,compressed](auto& c,void*){reader(c,compressed,false);},
                [this,compressed](auto& c,void*){reader(c,compressed,true);},nullptr);
            native_hooks_.push_back({fn,ids});
        }
        global_ids_.push_back(Hook::RegisterProcessLocalScriptFunctionPreCallback(
            [this](auto&,UObject* c,FFrame& f,void*){
                // Optional companion capture observes the Blueprint. It cannot
                // suppress its reader, travel, actor application or OUT counts.
                try {script_pre(c,f);}catch(const std::exception& error){
                    std::lock_guard lock(mutex_);ledger_.abandon(key(f));loads_.abandon(key(f));intents_.erase(key(f));
                    Output::send(STR("[WIR] native SaveLoad observation unavailable: {}; vanilla operation continues\n"),ensure_str(error.what()));
                }catch(...){
                    std::lock_guard lock(mutex_);ledger_.abandon(key(f));loads_.abandon(key(f));intents_.erase(key(f));
                    Output::send(STR("[WIR] native SaveLoad observation unavailable; vanilla operation continues\n"));
                }
            },
            {false,true,STR("WitchItNativeProducer"),STR("Save/Load observational entries"),save_script_filter()}));
        global_ids_.push_back(Hook::RegisterProcessLocalScriptFunctionPostCallback(
            [this](auto&,UObject*,FFrame& f,void*){try{script_post(f);}catch(const std::exception&){
                std::lock_guard lock(mutex_);ledger_.abandon(key(f));loads_.abandon(key(f));load_barriers_.leave(key(f));intents_.erase(key(f));applies_.erase(key(f));}},
            {false,true,STR("WitchItNativeProducer"),STR("SaveLayout exact return"),save_script_filter()}));
        // StoreStatics can first load DURING SaveLayout. Cached name filtering
        // observes its physical POST without a Lua finite-install/discovery race.
        const Hook::FScriptNameFilter storeFilter{{static_cast<std::int32_t>(store_name_.GetComparisonIndex().ToUnstableInt())}};
        global_ids_.push_back(Hook::RegisterProcessLocalScriptFunctionPostCallback(
            [this](auto&,UObject*,FFrame& f,void*){store_statics(f);},
            {false,true,STR("WitchItNativeProducer"),STR("physical carrier payload copy"),storeFilter}));
        global_ids_.push_back(Hook::RegisterProcessInternalPostCallback(
            [this](auto&,UObject*,FFrame& f,void*){store_statics(f);},
            {false,true,STR("WitchItNativeProducer"),STR("physical internal carrier payload copy"),storeFilter}));
        const auto endpointLoadName=FName(STR("LoadStatics"),FNAME_Add);
        const Hook::FScriptNameFilter endpointLoadFilter{{static_cast<std::int32_t>(endpointLoadName.GetComparisonIndex().ToUnstableInt())}};
        global_ids_.push_back(Hook::RegisterProcessLocalScriptFunctionPostCallback(
            [this](auto&,UObject*,FFrame& f,void*){load_carrier_payload(f);},
            {false,true,STR("WitchItNativeProducer"),STR("native endpoint load payload"),endpointLoadFilter}));
        global_ids_.push_back(Hook::RegisterProcessInternalPostCallback(
            [this](auto&,UObject*,FFrame& f,void*){load_carrier_payload(f);},
            {false,true,STR("WitchItNativeProducer"),STR("native internal endpoint load payload"),endpointLoadFilter}));
        registered_=true;
        }
    }
    void on_unreal_init() override {
        std::lock_guard lock(mutex_);
        if(unreal_initialized_)return;
        unreal_initialized_=true;
        if(boot_.enabled("writer_safety"))install_writer_quarantine();
        // Unreal-init is the host/control thread. EngineTick PRE establishes GT
        // identity; bOnce removes this POST callback after provisioning the routes.
        // Attach may complete the same phase earlier, but only from verified GT.
        if(boot_.enabled("save_load_startup")){
        const auto startup_id=Hook::RegisterEngineTickPostCallback(
            [this](auto&,UEngine*,float,bool){initialize_save_load_on_game_thread();},
            {true,true,STR("WitchItNativeProducer"),STR("owned SaveLoad game-thread startup")});
        if(startup_id==Hook::ERROR_ID){
            Output::send(STR("WIR SaveLoad: UNAVAILABLE game-thread startup callback; physical writer quarantine {}\n"),
                writer_quarantine_ready_?STR("installed"):STR("incomplete"));
        }else{
            startup_callback_registered_=true;global_ids_.push_back(startup_id);
        }
        }
        // The Host capabilities are fixed for this process. Do not register
        // global gameplay callbacks when the value bridge cannot start: even an
        // inactive body pays the Native/VM owner dispatch cost on every event.
        if (witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1)) {
            if(boot_.enabled("travel"))global_ids_.push_back(Hook::RegisterLoadMapPreCallback(
                [this](auto&,UEngine*,FWorldContext&,FURL,UPendingNetGame*,FString&){if(gameplay_)gameplay_->before_world();local_world_.before_world();ship_parts_before_world();},
                {false,true,STR("WitchItNativeProducer"),STR("pure Native world retirement for all consumers")}));
        }

    }
    void on_lua_start(StringViewType mod,Lua& lua,Lua&,Lua&,Lua* hook) override {
        if(mod!=STR("WitchItRuntime"))return;
        retire_ship_parts();
        if(gameplay_){
            if(!gameplay_->stop())throw std::runtime_error("native previous generation cleanup incomplete");
            // on_lua_stop_drain already unregistered all external callbacks.
            if(!gameplay_->drained())throw std::runtime_error("native previous generation has not drained");
            gameplay_.reset();
        }
        if(!gameplay_)witchit::debug::configure("");
        local_world_.stop();
        std::lock_guard lock(mutex_);detach();
        if(boot_.enabled("local_world")&&witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1))local_world_.start();
        try {
            if(auto* host=witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1);boot_.enabled("ship_binding")&&host) {
                auto binding=std::make_shared<ShipBinding>(host,local_world_.source());
                binding->activate();ship_parts_.store(std::move(binding));
            }
        }catch(const std::exception&){retire_ship_parts();}
        lua.register_function("WitchItNativeShipPartsBridge",[](const Lua& caller){
            auto held=instance?instance->ship_parts_.load():nullptr;
            if(!held){caller.set_nil();return 1;}
            try{return held->accessor();}catch(const std::exception&){caller.set_nil();return 1;}
        });
        lua.register_function("WitchItNativeProducerAttach",attach);
        lua.register_function("WitchItNativeWriterRecovery",writer_recovery);
        lua.register_function("WitchItNativeWriterRecoverySnapshotV1",writer_recovery_snapshot);
        lua.register_function("WitchItNativeWriterRecoveryResolveRestoredV1",recovery_resolve);
        lua.register_function("WitchItNativeWriterRecoveryVerifyReadyV1",recovery_ready);
        lua.register_function("WitchItNativeWriterRecoveryVerifyResolutionV1",recovery_verified);
        lua.register_function("WitchItNativeProducerDetach",detach_bridge);
        lua.register_function("WitchItNativeDebug",[](const Lua& caller){
            try {
                witchit::uikit::HostStack s(witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1),UE4SS_LUA_HOST_CAP_SCALARS_V1);
                const auto result=witchit::debug::configure_stack(s,instance && bool(instance->gameplay_));
                if(result==1)Output::send(STR("[WIR] Core native diagnostics: disabled={}\n"),ensure_str(s.get_string(1).value_or("")));
                return result;
            }catch(const std::exception& e){caller.set_bool(false);caller.set_string(e.what());return 2;}
        });
        lua.register_function("WitchItNativePrimitives",[](const Lua& caller){
            if(!instance||!instance->boot_.enabled("primitives")){Output::send(STR("[WIR] Producer boot: skip Native primitives accessor\n"));caller.set_nil();return 1;}
            const auto* host=witchit::uikit::resolve_host_values();
            try {
                if(!instance->gameplay_)instance->gameplay_=std::make_shared<witchit::ScopedGameplay>(host,instance->local_world_);
                return instance->gameplay_->push()?1:0;
            }catch(const std::exception& e){caller.throw_error(e.what());return 0;}
        });
        lua.register_function("WitchItNativeClipboard",[](const Lua& caller){
            if(!instance||!instance->boot_.enabled("clipboard")){Output::send(STR("[WIR] Producer boot: skip clipboard accessor\n"));caller.set_nil();return 1;}
            try {
                witchit::uikit::HostStack s(witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1),UE4SS_LUA_HOST_CAP_SCALARS_V1);
                auto op=s.get_string(1);if(!op)throw std::runtime_error("Native.clipboard: operation required");
                auto result=witchit::clipboard::execute(*op,s.get_string(2));
                if(result)s.string(*result);else s.nil();return 1;
            }catch(const std::exception& e){caller.throw_error(e.what());return 0;}
        });
        lua.register_function("WitchItNativeUpdater",[](const Lua& caller){
            try {
                witchit::uikit::HostStack s(witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1),UE4SS_LUA_HOST_CAP_SCALARS_V1);
                auto operation=s.get_string(1);if(!operation)throw std::runtime_error("updater operation required");
                auto value=witchit::updater::operation(*operation);
                if(value)s.string(*value);else s.nil();return 1;
            }catch(const std::exception& e){caller.throw_error(e.what());return 0;}
        });
        lua.register_function("WitchItNativeUpdateFile",[](const Lua& caller){
            if(!instance||!instance->boot_.enabled("update_file")){Output::send(STR("[WIR] Producer boot: skip updater file accessor\n"));caller.set_nil();return 1;}
            try {
                witchit::uikit::HostStack s(witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1),UE4SS_LUA_HOST_CAP_SCALARS_V1);
                auto operation=s.get_string(1);
                if(!operation)throw std::runtime_error("Native.update_file: operation required");
                auto result=witchit::update_files::execute(*operation,s.get_string(2));
                if(result)s.string(*result);else s.nil();return 1;
            }catch(const std::exception& e){caller.throw_error(e.what());return 0;}
        });
        // Metadata only: execution and marshaling remain in the existing SDK.
        lua.register_function("WitchItNativeFunctionSchema",[](const Lua& caller){
            if(!instance||!instance->boot_.enabled("schemas")){Output::send(STR("[WIR] Producer boot: skip function schema accessor\n"));caller.set_nil();return 1;}
            try {
                witchit::uikit::HostStack s(witchit::uikit::resolve_host_values());
                auto* object=static_cast<UObject*>(s.object(1));
                auto name=s.get_string(2);
                if(!object || !name)throw std::runtime_error("Native schema requires object and function name");
                std::wstring wide(name->begin(),name->end());
                auto* fn=object->GetFunctionByNameInChain(FName(wide.c_str(),FNAME_Find));
                if(!fn){s.nil();return 1;} // SDK methods are not UFunctions.
                s.table();const int result=s.top();int count=0;
                for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated)) {
                    if(!p->HasAnyPropertyFlags(CPF_Parm))continue;
                    auto key=utf8(p->GetName());
                    if(p->HasAnyPropertyFlags(CPF_ReturnParm)) {s.string(key);s.set_field(result,"return_name");continue;}
                    auto index=std::to_string(++count);
                    s.string(key);s.set_field(result,index);
                    s.boolean(p->HasAnyPropertyFlags(CPF_OutParm) && !p->HasAnyPropertyFlags(CPF_ConstParm));
                    s.set_field(result,"out:"+index);
                }
                s.integer(count);s.set_field(result,"count");return 1;
            }catch(const std::exception& e){caller.throw_error(e.what());return 0;}
        });

        Output::send(STR("[WIR] NativeProducer ready: runtime=WitchItRuntime host_values={} writer_safety={} startup={} viewport={} travel={} local_world={} ship_binding={} primitives={} file_bridge={} schemas={} clipboard={} update_file={}\n"),
            witchit::uikit::resolve_host_values(UE4SS_LUA_HOST_CAP_SCALARS_V1)!=nullptr,boot_.enabled("writer_safety"),boot_.enabled("save_load_startup"),boot_.enabled("viewport"),boot_.enabled("travel"),boot_.enabled("local_world"),boot_.enabled("ship_binding"),boot_.enabled("primitives"),boot_.enabled("file_bridge"),boot_.enabled("schemas"),boot_.enabled("clipboard"),boot_.enabled("update_file"));
    }
    void on_lua_stop(StringViewType mod,Lua& lua,Lua&,Lua&,Lua*) override {
        if(mod!=STR("WitchItRuntime"))return;
        retire_ship_parts();
        if(gameplay_)gameplay_->stop();
        local_world_.stop();
        std::lock_guard lock(mutex_);detach();
    }
    void on_lua_stop_drain(StringViewType mod) override {
        if(mod!=STR("WitchItRuntime"))return;
        if(gameplay_){
            gameplay_->drain();
            if(gameplay_->cleanup_complete())gameplay_.reset();
            else Output::send(STR("[WIR] Native teardown retains failed actor rollback until cleanup or world retirement\n"));
        }
    }
};
}
extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod(){return new NativeProducer;}
extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod){delete mod;}
