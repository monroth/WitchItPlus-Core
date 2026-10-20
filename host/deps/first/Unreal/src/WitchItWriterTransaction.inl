// Host-owned narrow transaction for the two exact WitchIt native writers.
// Included in UFunctionStructs.cpp; no extra public frame fields or vtable.
namespace RC::Unreal::Internal {
using WriterContext=UnrealScriptFunctionCallableContext;
using WriterValidator=WriterContext::WitchItWriterValidator;
using WriterSlot=Hook::Internal::TCallbackSlot<WriterValidator>;
class WriterParameters final {
    UFunction* function_;std::vector<FProperty*> fields_;void* data_{};
public:
    explicit WriterParameters(UFunction* fn):function_(fn) {
        const auto size=fn->GetParmsSize();if(!size || size>1048576)throw std::runtime_error("invalid writer parameter block");
        std::size_t alignment=8;std::vector<std::pair<std::size_t,std::size_t>> spans;
        for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm)){
            const auto offset=p->GetOffset_Internal(),n=p->GetSize();
            if(offset<0 || n<=0 || p->GetArrayDim()!=1 || p->GetMinAlignment()<=0 || p->GetMinAlignment()>256 || std::uint64_t(offset)+n>size)throw std::runtime_error("writer parameter outside block");
            for(auto [a,b]:spans)if(std::size_t(offset)<b && a<std::size_t(offset+n))throw std::runtime_error("writer parameters overlap");
            spans.push_back({offset,offset+n});alignment=(std::max)(alignment,std::size_t(p->GetMinAlignment()));
        }
        // Bookkeeping must never allocate after InitializeValue succeeds.
        fields_.reserve(spans.size());
        data_=FMemory::Malloc(size,alignment);if(!data_)throw std::bad_alloc();FMemory::Memzero(data_,size);
        try {for(auto* p:TFieldRange<FProperty>(fn,EFieldIterationFlags::IncludeDeprecated))if(p->HasAnyPropertyFlags(CPF_Parm)){p->InitializeValue(p->ContainerPtrToValuePtr<void>(data_));fields_.push_back(p);}}
        catch(...){clear();throw;}
    }
    ~WriterParameters(){clear();}
    void clear() noexcept {if(data_){for(auto it=fields_.rbegin();it!=fields_.rend();++it)try{(*it)->DestroyValue((*it)->ContainerPtrToValuePtr<void>(data_));}catch(...){std::terminate();}FMemory::Free(data_);data_=nullptr;}}
    void* data() const{return data_;}
    void* value(FProperty* p) const{return p->ContainerPtrToValuePtr<void>(data_);}
    std::vector<FProperty*> inputs() const {std::vector<FProperty*> v;for(auto* p:fields_)if(!p->HasAnyPropertyFlags(CPF_ReturnParm))v.push_back(p);return v;}
};
inline void* writer_argument(FFrame& frame,FProperty* p) {
    auto* value=p->HasAnyPropertyFlags(CPF_OutParm)?FindOutParamValueAddress(frame,p):p->ContainerPtrToValuePtr<void>(frame.Locals());
    if(!value)throw std::runtime_error("writer input address unavailable");return value;
}
#if defined(UE4SS_CAULDRON_ROUNDTRIP_DIAGNOSTIC)
#include "CauldronSdkRoundtripFixture.inl"
#endif
// Optional reflected input copy. The serializer keeps its real filename and
// return value; metadata availability never grants or denies a vanilla write.
class WitchItWriterInputCopy final {
    WriterContext& context_;
    WriterParameters parameters_;
    std::vector<std::pair<FOutParmRec*,uint8*>> outs_;
public:
    WitchItWriterInputCopy(WriterContext& context,const void* expected,const void* owned)
        :context_(context),parameters_(context.TheStack.Node()) {
        const auto inputs=parameters_.inputs();
        if(inputs.size()!=8)throw std::runtime_error("writer input schema differs");
        auto* statics=CastField<FArrayProperty>(inputs[3]);
        if(!statics || !expected || !owned || !statics->Identical(writer_argument(context.TheStack,statics),expected))
            throw std::runtime_error("captured statics differ from actual writer input");
        for(auto* p:inputs)p->CopyCompleteValue(parameters_.value(p),writer_argument(context.TheStack,p));
        statics->CopyCompleteValue(parameters_.value(statics),owned);
        if(!statics->Identical(parameters_.value(statics),owned))throw std::runtime_error("writer metadata copy differs");
        for(auto* out=context.TheStack.OutParms();out;out=out->NextOutParm)
            if(std::find(inputs.begin(),inputs.end(),out->Property)!=inputs.end())outs_.push_back({out,out->PropAddr});
    }
    template<class Fn> void observe(Fn body) {
        auto& frame=context_.TheStack;
        struct Restore {
            FFrame& frame;uint8* locals;std::vector<std::pair<FOutParmRec*,uint8*>>& outs;
            ~Restore(){frame.Locals()=locals;for(auto [p,address]:outs)p->PropAddr=address;}
        } restore{frame,frame.Locals(),outs_};
        for(auto [p,address]:outs_)p->PropAddr=static_cast<uint8*>(parameters_.value(p->Property));
        frame.Locals()=static_cast<uint8*>(parameters_.data());
        body();
    }
    template<class Original> void serialize(Original original) {
        observe([&]{original(context_.Context,context_.TheStack,context_.RESULT_DECL);});
    }
};
class WriterFile final {
    struct Handle {HANDLE value{INVALID_HANDLE_VALUE};~Handle(){reset();}void reset(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}};
    static inline std::atomic<std::uint64_t> serial_{};
    static inline std::set<std::wstring> active_;
    struct RecoveryRecord {std::wstring backup;bool uncertain{};};
    static inline std::map<std::wstring,RecoveryRecord> recovery_;
    static inline std::recursive_mutex recoveryMutex_;
    RecoveryRecord* receipt_{};
    std::wstring target_,temporary_,backup_,journalPath_;
    Handle original_,temporaryPin_,journal_;
    std::vector<std::uint8_t> old_,new_;
    BY_HANDLE_FILE_INFORMATION originalInfo_{},temporaryInfo_{};
    enum class Outcome { NotPublished, Published, RolledBack, Uncertain };
    Outcome outcome_{Outcome::NotPublished};
    bool existed_{},temporaryOwned_{},backupOwned_{},published_{},frozen_{};
    bool journalOwned_{};
    std::string intent_;bool intentComplete_{};
    static std::wstring absolute_target(const std::wstring& path) {
        if(path.empty() || path.size()>4096 || path.find(L'\0')!=std::wstring::npos)throw std::runtime_error("layout target invalid");
        std::wstring result(32768,L'\0');
        const auto n=GetFullPathNameW(path.c_str(),DWORD(result.size()),result.data(),nullptr);
        if(!n || n>=result.size() || n>4096)throw std::runtime_error("absolute layout target unavailable");
        result.resize(n);return result;
    }
    static std::vector<std::uint8_t> read(HANDLE h) {
        LARGE_INTEGER size{},zero{};
        if(!GetFileSizeEx(h,&size) || size.QuadPart<0 || size.QuadPart>256*1024*1024 || !SetFilePointerEx(h,zero,nullptr,FILE_BEGIN))throw std::runtime_error("layout file outside readable bound");
        std::vector<std::uint8_t> data(std::size_t(size.QuadPart));std::size_t at{};
        while(at<data.size()){DWORD n{};const auto chunk=DWORD(std::min<std::size_t>(data.size()-at,1048576));if(!ReadFile(h,data.data()+at,chunk,&n,nullptr) || !n)throw std::runtime_error("layout file read failed");at+=n;}
        return data;
    }
    static void write_bytes(HANDLE handle,const void* data,std::size_t size) {
        auto* bytes=static_cast<const std::uint8_t*>(data);std::size_t at{};
        while(at<size){DWORD wrote{};const auto chunk=DWORD(std::min<std::size_t>(size-at,1048576));
            if(!WriteFile(handle,bytes+at,chunk,&wrote,nullptr) || !wrote || wrote>chunk)throw std::runtime_error("recovery journal write failed");at+=wrote;}
    }
    void journal_before_publish() {
        journal_.value=CreateFileW(journalPath_.c_str(),GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(journal_.value==INVALID_HANDLE_VALUE)throw std::runtime_error("recovery journal exists or cannot be reserved; publication refused");
        journalOwned_=true;
        // Non-executable bounded binary record, flushed before publication.
        // Full expected bytes retain recovery evidence without an unverified hash.
        const char magic[]="WIRJNL1";write_bytes(journal_.value,magic,8);
        for(auto n:{std::uint64_t(target_.size()),std::uint64_t(backup_.size()),std::uint64_t(old_.size()),std::uint64_t(new_.size())})write_bytes(journal_.value,&n,sizeof(n));
        const std::uint32_t oldId[]{originalInfo_.dwVolumeSerialNumber,originalInfo_.nFileIndexHigh,originalInfo_.nFileIndexLow};
        const std::uint32_t newId[]{temporaryInfo_.dwVolumeSerialNumber,temporaryInfo_.nFileIndexHigh,temporaryInfo_.nFileIndexLow};
        write_bytes(journal_.value,oldId,sizeof(oldId));write_bytes(journal_.value,newId,sizeof(newId));
        write_bytes(journal_.value,target_.data(),target_.size()*sizeof(wchar_t));write_bytes(journal_.value,backup_.data(),backup_.size()*sizeof(wchar_t));
        write_bytes(journal_.value,old_.data(),old_.size());write_bytes(journal_.value,new_.data(),new_.size());
        if(!FlushFileBuffers(journal_.value))throw std::runtime_error("recovery journal flush failed");
    }
    bool clear_journal() noexcept {
        if(!journalOwned_)return true;
        try{if(!dispose_handle(journal_))return false;journalOwned_=false;return true;}catch(...){return false;}
    }
    static bool same_file(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) {
        return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh && a.nFileIndexLow==b.nFileIndexLow;
    }
    static bool absent(const std::wstring& path) {
        if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
        const auto e=GetLastError();return e==ERROR_FILE_NOT_FOUND || e==ERROR_PATH_NOT_FOUND;
    }
    bool old_current() {
        Handle h;h.value=CreateFileW(target_.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(!existed_)return h.value==INVALID_HANDLE_VALUE && GetLastError()==ERROR_FILE_NOT_FOUND;
        if(h.value==INVALID_HANDLE_VALUE)return false;BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(h.value,&info) && same_file(originalInfo_,info) && read(h.value)==old_;
    }
    bool verify_file_bytes(const std::wstring& file,const std::vector<std::uint8_t>& expected) {
        Handle h;h.value=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        return h.value!=INVALID_HANDLE_VALUE && read(h.value)==expected;
    }
    static bool identity(HANDLE h,const BY_HANDLE_FILE_INFORMATION& expected) {
        BY_HANDLE_FILE_INFORMATION info{};
        return h!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(h,&info) && same_file(info,expected);
    }
    static bool dispose_handle(Handle& h) {
        FILE_DISPOSITION_INFO info{};info.DeleteFile=TRUE;
        if(!SetFileInformationByHandle(h.value,FileDispositionInfo,&info,sizeof(info)))return false;
        h.reset();return true;
    }
    static bool dispose_owned(const std::wstring& path,const BY_HANDLE_FILE_INFORMATION& expected) noexcept {
        try {Handle h;h.value=CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            return identity(h.value,expected) && dispose_handle(h);
        }catch(...){return false;}
    }
    bool target_is_new() {
        Handle h;h.value=CreateFileW(target_.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        return identity(h.value,temporaryInfo_) && read(h.value)==new_;
    }
    bool restore_previous() noexcept {
        try {
            // An unchanged original (or original absence) needs no rollback.
            if(old_current())return true;
            Handle backup;
            if(existed_) {
                backup.value=CreateFileW(backup_.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(!identity(backup.value,originalInfo_) || read(backup.value)!=old_)return false;
                backupOwned_=true;
            }
            if(!absent(target_)) {
                Handle current;current.value=CreateFileW(target_.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                // Never erase/replace a conflicting target by pathname. DELETE
                // is applied to the verified replacement HANDLE itself.
                if(!identity(current.value,temporaryInfo_) || !dispose_handle(current))return false;
            }
            if(!existed_)return absent(target_);
            const auto size=sizeof(FILE_RENAME_INFO)+(target_.size()+1)*sizeof(wchar_t);
            auto storage=std::make_unique<unsigned char[]>(size);
            auto* rename=std::construct_at(reinterpret_cast<FILE_RENAME_INFO*>(storage.get()));
            rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;
            rename->FileNameLength=DWORD(target_.size()*sizeof(wchar_t));
            std::copy(target_.begin(),target_.end(),rename->FileName);
            // Rename the pinned ORIGINAL handle; never overwrite a target that
            // appeared after disposal. Failure keeps the original recoverable.
            if(!SetFileInformationByHandle(backup.value,FileRenameInfo,rename,DWORD(size)))return false;
            backupOwned_=false;return old_current();
        }catch(...){return false;}
    }
public:
    explicit WriterFile(std::wstring target,std::uint64_t serial,std::uint64_t generation):target_(absolute_target(target)) {
        std::lock_guard lock(recoveryMutex_);
        if(blocked(target_))throw std::runtime_error("layout target has unresolved publication; write blocked");
        journalPath_.clear();
        if(target_.empty() || target_.size()>4096 || target_.find(L'\0')!=std::wstring::npos || !active_.insert(target_).second)throw std::runtime_error("layout target invalid/already writing");
        try {
            original_.value=CreateFileW(target_.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(original_.value!=INVALID_HANDLE_VALUE){existed_=true;if(!GetFileInformationByHandle(original_.value,&originalInfo_) || originalInfo_.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("original layout identity unavailable");old_=read(original_.value);}
            else if(GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("original layout availability unknown");
            for(unsigned attempt=0;attempt<16;++attempt){
                const auto number=serial_.fetch_add(1,std::memory_order_relaxed);if(number==UINT64_MAX)throw std::runtime_error("writer file serial exhausted");
                auto suffix=L".wir-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(number);
                temporary_=target_+suffix+L".tmp";backup_=target_+suffix+L".bak";
                if(!absent(backup_))continue;
                temporaryPin_.value=CreateFileW(temporary_.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(temporaryPin_.value!=INVALID_HANDLE_VALUE){
                    if(!GetFileInformationByHandle(temporaryPin_.value,&temporaryInfo_))throw std::runtime_error("layout temp identity unavailable");
                    temporaryOwned_=true;
                    // Allocate the durable host receipt before native serialization.
                    auto [i,inserted]=recovery_.try_emplace(target_,RecoveryRecord{backup_,false});
                    if(!inserted)throw std::runtime_error("layout recovery receipt already owned");
                    receipt_=&i->second;receipt_->backup.reserve(32768);
                    intent_=WriterRecoveryStore::instance().begin(this,serial,generation,target_,backup_,existed_,originalInfo_,old_);
                    journalPath_=WriterRecoveryStore::instance().publication_path(intent_);return;
                }
                if(GetLastError()!=ERROR_FILE_EXISTS && GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error("exclusive layout temp create failed");
            }
            throw std::runtime_error("layout temp reservation exhausted");
        } catch(...){if(!intent_.empty())WriterRecoveryStore::instance().uncertain(this);temporaryPin_.reset();if(temporaryOwned_)(void)dispose_owned(temporary_,temporaryInfo_);if(receipt_ && !receipt_->uncertain)recovery_.erase(target_);active_.erase(target_);throw;}
    }
    ~WriterFile(){std::lock_guard lock(recoveryMutex_);settle_not_published();temporaryPin_.reset();original_.reset();if(temporaryOwned_)(void)dispose_owned(temporary_,temporaryInfo_);/* failed publication backups/journal stay recoverable */journal_.reset();if(receipt_ && !receipt_->uncertain)recovery_.erase(target_);active_.erase(target_);}
    const std::wstring& temporary() const{return temporary_;}
    void flush_and_pin() {
        Handle write;write.value=CreateFileW(temporary_.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(write.value==INVALID_HANDLE_VALUE || !FlushFileBuffers(write.value))throw std::runtime_error("layout temp flush failed");write.reset();
        // The native writer has returned. Freeze bytes AND filename while the
        // matching native reader validates. Existing open writers cause refusal.
        temporaryPin_.reset();
        temporaryPin_.value=CreateFileW(temporary_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(!identity(temporaryPin_.value,temporaryInfo_))throw std::runtime_error("layout temp changed before freeze");
        new_=read(temporaryPin_.value);if(new_.empty())throw std::runtime_error("native writer produced empty file");
        frozen_=true;
    }
    bool publish() noexcept {
        std::lock_guard lock(recoveryMutex_);
        try {
            if(outcome_!=Outcome::NotPublished || !temporaryOwned_ || !frozen_ || !old_current() || !absent(backup_)
                || !identity(temporaryPin_.value,temporaryInfo_) || read(temporaryPin_.value)!=new_)return false;
            journal_before_publish(); // durable before releasing the replacement pin
            // No engine/Lua callback occurs between the final current-owner
            // witness and these same-volume native publication calls.
            temporaryPin_.reset();
            // ReplaceFile opens replacement without sharing, so this release is
            // required. Do not claim an atomic/no-clobber guarantee for the gap.
            outcome_=Outcome::Uncertain;
            receipt_->uncertain=true;
            const bool ok=existed_?ReplaceFileW(target_.c_str(),temporary_.c_str(),backup_.c_str(),0,nullptr,nullptr)
                :MoveFileExW(temporary_.c_str(),target_.c_str(),MOVEFILE_WRITE_THROUGH);
            if(ok)temporaryOwned_=false;
            if(ok && target_is_new() && clear_journal() && WriterRecoveryStore::instance().complete(this,intent_,true,0)) {
                published_=true;outcome_=Outcome::Published;
                receipt_->uncertain=false;
                if(existed_ && dispose_owned(backup_,originalInfo_)){original_.reset();backupOwned_=false;}
                return true;
            }
        } catch(...) {}
        if(outcome_==Outcome::Uncertain && restore_previous() && clear_journal() && WriterRecoveryStore::instance().complete(this,intent_,true,1)){outcome_=Outcome::RolledBack;receipt_->uncertain=false;}
        if(outcome_==Outcome::NotPublished && journalOwned_ && !clear_journal()){
            outcome_=Outcome::Uncertain;receipt_->uncertain=true;receipt_->backup=journalPath_;
        }
        if(outcome_==Outcome::Uncertain)WriterRecoveryStore::instance().uncertain(this);
        return false;
    }
    void settle_not_published() noexcept {
        if(intentComplete_ || outcome_!=Outcome::NotPublished || intent_.empty())return;
        try {if(old_current() && WriterRecoveryStore::instance().complete(this,intent_,true,2)){intentComplete_=true;return;}}catch(...){}
        outcome_=Outcome::Uncertain;receipt_->uncertain=true;WriterRecoveryStore::instance().uncertain(this);
    }
    bool published() const noexcept{return published_;}
    bool uncertain() const noexcept{return outcome_==Outcome::Uncertain;}
    const std::wstring& target() const noexcept{return target_;}
    const std::wstring& backup() const noexcept{return backup_;}
    const std::wstring& recovery_path() const noexcept{return receipt_?receipt_->backup:backup_;}
    WitchItPublicationState state() const noexcept {
        if(outcome_==Outcome::Published)return WitchItPublicationState::Committed;
        if(outcome_==Outcome::RolledBack)return WitchItPublicationState::RolledBack;
        if(outcome_==Outcome::Uncertain)return WitchItPublicationState::Uncertain;
        return WitchItPublicationState::NotPublished;
    }
    static bool blocked(const std::wstring& target) noexcept {
        std::lock_guard lock(recoveryMutex_);
        try{
            auto& store=WriterRecoveryStore::instance();(void)store.initialize();if(!store.allowed())return true;
            // Quarantine is process-wide, regardless of target spelling,
            // short filename, hardlink alias or SaveAs destination.
            for(const auto& [name,record]:recovery_)if(record.uncertain)return true;
            const auto path=absolute_target(target);auto i=recovery_.find(path);
            if(i!=recovery_.end() && i->second.uncertain)return true;
            const auto journal=path+L".wir-journal";
            if(!absent(journal)) {
                // Foreign/truncated journals are preserved and never evaluated.
                // A new process also refuses overwrite while this file exists.
                auto [record,inserted]=recovery_.try_emplace(path,RecoveryRecord{journal,true});
                record->second.uncertain=true;return true;
            }
            return false;}catch(...){return true;}
    }
    static bool has_active(const std::wstring& target) noexcept {std::lock_guard lock(recoveryMutex_);return active_.contains(target);}
    static void clear_resolved(const std::wstring& target) noexcept {std::lock_guard lock(recoveryMutex_);if(!active_.contains(target))recovery_.erase(target);}
    static bool unresolved_for(const std::wstring& target,StringType& backup) noexcept {
        std::lock_guard lock(recoveryMutex_);
        try{const auto path=absolute_target(target);if(!blocked(path))return false;
            auto i=recovery_.find(path);if(i==recovery_.end())return false;backup=i->second.backup;return true;}catch(...){return false;}
    }
    static bool unresolved(StringType& target,StringType& backup) noexcept {
        std::lock_guard lock(recoveryMutex_);
        try{for(const auto& [path,r]:recovery_)if(r.uncertain){target=path;backup=r.backup;return true;}}catch(...){}
        return false;
    }
#if defined(UE4SS_WRITER_FILE_TEST)
    static void simulate_process_memory_reset_for_test(){std::lock_guard lock(recoveryMutex_);active_.clear();recovery_.clear();}
#endif
};
class WitchItWriterTransaction final {
    WriterContext& context_;std::unique_ptr<WriterParameters> parameters_;
    std::vector<FProperty*> inputs_;
    std::unique_ptr<WriterParameters> original_;
    std::unique_ptr<WriterFile> file_;
    std::shared_ptr<WriterSlot> validator_;
    UFunction* reader_{};bool complete_{},publishAllowed_{},committed_{};
    std::uint64_t serial_{},bridge_generation_{};
    bool witness(WitchItWriterPhase phase,bool initial) noexcept {
        if(!validator_)return false;
        bool value=initial;
        try {return validator_->Invoke(context_,phase,value) && value;}catch(...){return false;}
    }
    bool inputs_current() {
        for(auto* p:inputs_)if(!p->Identical(original_->value(p),writer_argument(context_.TheStack,p)))return false;
        return true;
    }
    void validate_file() {
        if(!witness(WitchItWriterPhase::BeforeSerialize,true))throw std::runtime_error("reader validation lease retired");
        file_->flush_and_pin();
        WriterParameters output(reader_);auto fields=output.inputs();
        if(fields.size()!=8 || !CastField<FStrProperty>(fields[0]))throw std::runtime_error("matching reader schema differs");
        *static_cast<FString*>(output.value(fields[0]))=FString(file_->temporary().c_str());
        auto* result=CastField<FBoolProperty>(reader_->GetReturnProperty());if(!result)throw std::runtime_error("matching native reader bool missing");
        context_.Context->ProcessEvent(reader_,output.data());
        // Reader hooks may retire the Core/document lease. Recheck its pure
        // owner predicate before reading any reflected output or old input.
        if(!witness(WitchItWriterPhase::BeforeSerialize,true))throw std::runtime_error("reader validation lease retired on return");
        if(!result->GetPropertyValue(output.value(result)))throw std::runtime_error("native temp layout reader refused");
        for(std::size_t i=1;i<inputs_.size();++i){
            auto* writer=inputs_[i];auto* reader=fields[i];
            // Native writer/read functions must share the exact reflected type,
            // not merely equal widths or a Lua-projected subset.
            if(writer->GetClass()!=reader->GetClass() || writer->GetSize()!=reader->GetSize() || !writer->SameType(reader))throw std::runtime_error("reader/writer parameter type differs");
            if(auto* a=CastField<FArrayProperty>(writer)){
                auto* b=CastField<FArrayProperty>(reader);auto* x=CastField<FStructProperty>(a->GetInner());auto* y=CastField<FStructProperty>(b->GetInner());
                if(!x || !y || x->GetStruct()!=y->GetStruct())throw std::runtime_error("reader/writer row struct differs");
            }
            if(!writer->Identical(parameters_->value(writer),output.value(reader)))throw std::runtime_error("native temp layout payload differs");
        }
#if defined(UE4SS_CAULDRON_ROUNDTRIP_DIAGNOSTIC)
        const auto facts=cauldron_sdk_roundtrip_fixture(inputs_,fields,*original_,*parameters_,output);
        Output::send(STR("[CauldronSdkRoundtrip] save_serial={} bridge_generation={} args={} arrays={} reader_outputs={} cauldrons={} respawns={} grown_empty={} grown_short={} actual_native_reader=true\n"),
            serial_,bridge_generation_,facts.arguments,facts.arrayArguments,facts.readerOutputs,facts.cauldrons,facts.respawns,facts.grownEmpty,facts.grownShort);
#endif
    }
public:
    WitchItWriterTransaction(WriterContext& context,const void* expected,const void* owned,std::shared_ptr<WriterSlot> validator,std::uint64_t serial,std::uint64_t generation)
        :context_(context),validator_(std::move(validator)),serial_(serial),bridge_generation_(generation) {
        auto* function=context.TheStack.Node();parameters_=std::make_unique<WriterParameters>(function);original_=std::make_unique<WriterParameters>(function);inputs_=parameters_->inputs();
        if(inputs_.size()!=8 || !CastField<FStrProperty>(inputs_[0]) || !CastField<FArrayProperty>(inputs_[1]) || !CastField<FNumericProperty>(inputs_[2]))throw std::runtime_error("native writer schema differs");
        for(unsigned i=3;i<8;++i)if(!CastField<FArrayProperty>(inputs_[i]))throw std::runtime_error("native writer utility arrays differ");
        for(auto* p:inputs_){auto* value=writer_argument(context.TheStack,p);p->CopyCompleteValue(parameters_->value(p),value);p->CopyCompleteValue(original_->value(p),value);}
        auto* statics=CastField<FArrayProperty>(inputs_[3]);auto* row=CastField<FStructProperty>(statics->GetInner());
        if(!expected || !owned || !row || !row->GetStruct() || row->GetStruct()->GetPathName()!=STR("/Script/PropWitchHuntModule.PlaceableStaticToSave") || !statics->Identical(original_->value(statics),expected))throw std::runtime_error("sealed statics differ from actual native writer input");
        statics->CopyCompleteValue(parameters_->value(statics),owned);
        auto* filename=static_cast<FString*>(original_->value(inputs_[0]));if(filename->Len()<=0 || filename->Len()>4096)throw std::runtime_error("layout filename outside contract");
        file_=std::make_unique<WriterFile>(std::wstring(**filename,filename->Len()),serial,generation);
        *static_cast<FString*>(parameters_->value(inputs_[0]))=FString(file_->temporary().c_str());
        const auto compressed=function->GetPathName()==STR("/Script/PropWitchHuntModule.ProjectVersionBlueprint:SavePropLayoutToFileCompressed");
        reader_=UObjectGlobals::StaticFindObject<UFunction*>(nullptr,nullptr,compressed?STR("/Script/PropWitchHuntModule.ProjectVersionBlueprint:LoadPropLayoutFromFileCompressed"):STR("/Script/PropWitchHuntModule.ProjectVersionBlueprint:LoadPropLayoutFromFile"));
        if(!reader_ || !reader_->HasAnyFunctionFlags(EFunctionFlags::FUNC_Native))throw std::runtime_error("matching native reader unavailable");
    }
    bool before() noexcept {try{return inputs_current() && witness(WitchItWriterPhase::BeforeSerialize,true) && inputs_current();}catch(...){return false;}}
    template<class Original> void serialize(Original original) {
        auto& frame=context_.TheStack;auto* locals=frame.Locals();
        struct Reset {FFrame& frame;uint8* locals;std::vector<std::pair<FOutParmRec*,uint8*>> out;
            ~Reset(){frame.Locals()=locals;for(auto [p,address]:out)p->PropAddr=address;}} reset{frame,locals,{}};
        for(auto* out=frame.OutParms();out;out=out->NextOutParm)if(std::find(inputs_.begin(),inputs_.end(),out->Property)!=inputs_.end()){
            reset.out.push_back({out,out->PropAddr});out->PropAddr=static_cast<uint8*>(parameters_->value(out->Property));
        }
        frame.Locals()=static_cast<uint8*>(parameters_->data());original(context_.Context,frame,context_.RESULT_DECL);
    }
    bool finalize(bool native,bool observed,bool denied) noexcept {
        bool success=false;
        try {
            if(native && observed && !denied && inputs_current()){
                validate_file();
                if(inputs_current()){
                    publishAllowed_=true;
                    (void)witness(WitchItWriterPhase::BeforePublish,true);
                    publishAllowed_=false;
                    success=committed_ && file_->published();
                }
            }
        }catch(...){publishAllowed_=false;}
        if(file_)file_->settle_not_published();
        complete_=true;
        if(file_ && file_->uncertain())try {
            Output::send(STR("[WitchItWriter] UNCERTAIN publication/rollback; target={} backup={}; no unchanged-target guarantee\n"),file_->target(),file_->backup());
        }catch(...){}
        // Completion is factual: a subscriber cannot rewrite publication result.
        (void)witness(WitchItWriterPhase::Complete,success);
        return success;
    }
    bool commit() noexcept {
        if(!publishAllowed_ || complete_ || committed_)return false;
        publishAllowed_=false;committed_=file_->publish();return committed_;
    }
    bool outcome(WitchItPublicationState& state,StringType& target,StringType& backup) const noexcept {
        try{state=file_->state();target=file_->target();backup=state==WitchItPublicationState::Uncertain?file_->recovery_path():StringType{};return true;}catch(...){return false;}
    }
};
class NativeWriterTransactionScope final {
    friend auto UnrealScriptFunctionHook(UObject*,FFrame&,void*) -> void;
    friend class RC::Unreal::UnrealScriptFunctionCallableContext;
    static thread_local NativeWriterTransactionScope* current_;
    NativeWriterTransactionScope* previous_;WriterContext* context_;bool pre_{true};
    std::unique_ptr<WitchItWriterInputCopy> input_copy_;
    std::unique_ptr<WitchItWriterTransaction> transaction_;
    std::shared_ptr<WriterSlot> completion_;
    explicit NativeWriterTransactionScope(WriterContext* context):previous_(current_),context_(context){current_=this;}
public:
    ~NativeWriterTransactionScope(){current_=previous_;}
};
thread_local NativeWriterTransactionScope* NativeWriterTransactionScope::current_{};
}
namespace RC::Unreal {
namespace {
StringType recovery_wide(const std::string& value) {
    if(value.empty())return {};
    auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if(n<=0 || n>32768)throw std::runtime_error("invalid recovery UTF8");StringType output(n,L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),output.data(),n)!=n)throw std::runtime_error("invalid recovery UTF8");return output;
}
}
auto InitializeWitchItWriterRecovery() noexcept -> bool {
    if(!IsInGameThreadRaw())return false; // pending, never initialize or consume the startup log off GT
    auto& store=Internal::WriterRecoveryStore::instance();
    const bool initialized=store.initialize();
    static std::atomic<bool> logged{false};
    // Initialization reports storage availability, not permission to write.
    // UNCERTAIN must retain the snapshot/checked-restoration channel and hooks.
    if(initialized){
        if(!logged.exchange(true)){
            if(store.allowed())Output::send(STR("WIR recovery: READY\n"));
            else Output::send(STR("WIR recovery: UNAVAILABLE {} (writes quarantined)\n"),ensure_str(store.reason()));
        }
        return true;
    }
    if(!logged.exchange(true))Output::send(STR("WIR recovery: UNAVAILABLE {}\n"),ensure_str(store.reason()));
    return false;
}
auto GetWitchItWriterRecoverySnapshot(WitchItRecoverySnapshot& output) noexcept -> bool {
    output={};if(!IsInGameThreadRaw())return false;
    try {auto s=Internal::WriterRecoveryStore::instance().snapshot();
        output.state=static_cast<WitchItRecoveryState>(s.state);output.session=s.session;output.incident=s.incident;
        output.target=s.target;output.source=s.source;output.resolved=s.resolved;output.proof=s.proof;
        output.revision=s.revision;output.serial=s.serial;output.generation=s.generation;return true;
    }catch(...){return false;}
}
auto ResolveWitchItRestoredOriginal(const std::string& incident,std::uint64_t revision) noexcept -> bool {
    if(!IsInGameThreadRaw())return false;
    try {auto& store=Internal::WriterRecoveryStore::instance();auto before=store.snapshot();
        const auto target=recovery_wide(before.target); // allocate before terminal append
        if(Internal::WriterFile::has_active(target) || !store.resolve_restored(incident,revision))return false;
        Internal::WriterFile::clear_resolved(target);return true;
    }catch(...){return false;}
}
auto VerifyWitchItWriterRecoveryResolution(const std::string& incident,const std::string& session,std::uint64_t revision,const std::string& proof) noexcept -> bool {
    if(!IsInGameThreadRaw())return false;
    try{return Internal::WriterRecoveryStore::instance().verify_resolution(incident,session,revision,proof);}catch(...){return false;}
}
auto VerifyWitchItWriterRecoveryReady(const std::string& session,std::uint64_t revision) noexcept -> bool {
    if(!IsInGameThreadRaw())return false;
    try{return Internal::WriterRecoveryStore::instance().verify_ready(session,revision);}catch(...){return false;}
}
auto GetWitchItUnresolvedWrite(StringType& target,StringType& backup) noexcept -> bool {
    if(!IsInGameThreadRaw())return false;
    try {auto s=Internal::WriterRecoveryStore::instance().snapshot();if(s.state==Internal::WriterRecoveryLedger::State::Ready)return Internal::WriterFile::unresolved(target,backup);
        target=recovery_wide(s.target);backup=recovery_wide(s.source);return true;
    }catch(...){return true;}
}
auto UnrealScriptFunctionCallableContext::UseWitchItWriterStatics(const void* expected,const void* owned) -> bool {
    auto* scope=Internal::NativeWriterTransactionScope::current_;
    if(!scope || !scope->pre_ || scope->context_!=this || scope->input_copy_ || scope->transaction_
        || !Internal::NativeWriterGate::accepts(this))return false;
    try {
        auto copy=std::make_unique<Internal::WitchItWriterInputCopy>(*this,expected,owned);
        scope->input_copy_=std::move(copy);return true;
    }catch(...){return false;}
}
auto UnrealScriptFunctionCallableContext::PrepareWitchItWriter(const void* expected,const void* owned,const WitchItWriterValidator& validator,std::uint64_t serial,std::uint64_t generation) -> bool {
    auto* scope=Internal::NativeWriterTransactionScope::current_;
    if(!scope || !scope->pre_ || scope->context_!=this || scope->completion_ || !Internal::NativeWriterGate::accepts(this,true))return false;
    try {
        auto slot=Hook::Internal::TCallbackSlot<WitchItWriterValidator>::CreateCopy(validator,UE4SS_WIR_WRITER_REGISTRATION_ORIGIN);
        if(!slot){(void)DenyNativeWriterCall();return false;}
        scope->completion_=slot;
        if(!expected || !owned || !Internal::NativeWriterGate::accepts(this)){(void)DenyNativeWriterCall();return false;}
        scope->transaction_=std::make_unique<Internal::WitchItWriterTransaction>(*this,expected,owned,std::move(slot),serial,generation);return true;
    } catch(...) {(void)DenyNativeWriterCall();return false;}
}
auto UnrealScriptFunctionCallableContext::CommitWitchItWriterFile() noexcept -> bool {
    auto* scope=Internal::NativeWriterTransactionScope::current_;
    return scope && !scope->pre_ && scope->context_==this && scope->transaction_ && scope->transaction_->commit();
}
auto UnrealScriptFunctionCallableContext::GetWitchItWriterOutcome(WitchItPublicationState& state,StringType& target,StringType& backup) const noexcept -> bool {
    auto* scope=Internal::NativeWriterTransactionScope::current_;
    if(!scope || scope->pre_ || scope->context_!=this)return false;
    if(scope->transaction_)return scope->transaction_->outcome(state,target,backup);
    try {
        auto* filename=CastField<FStrProperty>(TheStack.Node()->FindProperty(FName(STR("AbsoluteFileName"),FNAME_Find)));
        if(!filename)return false;
        auto* value=static_cast<FString*>(Internal::writer_argument(TheStack,filename));
        target=StringType(**value,value->Len());
        state=Internal::WriterFile::unresolved_for(target,backup)?WitchItPublicationState::Uncertain:WitchItPublicationState::NotPublished;
        return true;
    }catch(...){return false;}
}
}
