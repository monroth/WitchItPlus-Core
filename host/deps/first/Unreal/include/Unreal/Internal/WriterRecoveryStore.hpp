#pragma once
#include <Unreal/Internal/WriterRecoveryLedger.hpp>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace RC::Unreal::Internal {
// One append-only, SDK-owned intent log. Existing missing/foreign/partial storage is
// UNAVAILABLE. Only an absent namespace may be provisioned; existing evidence
// is never reset, replaced or migrated. No arbitrary map-folder discovery.
class WriterRecoveryStore final {
    struct Handle {
        HANDLE value{INVALID_HANDLE_VALUE};
        ~Handle(){reset();}
        void reset() noexcept {if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
    };
    struct Cursor {
        const std::vector<std::uint8_t>& bytes;std::size_t at{};
        template<class T>T number(){if(sizeof(T)>bytes.size()-at)throw std::runtime_error("truncated recovery record");T v;std::memcpy(&v,bytes.data()+at,sizeof(v));at+=sizeof(v);return v;}
        std::string text(){auto n=number<std::uint32_t>();if(n>256 || n>bytes.size()-at)throw std::runtime_error("invalid recovery identity");std::string v(reinterpret_cast<const char*>(bytes.data()+at),n);at+=n;if(v.empty() || v.find('\0')!=std::string::npos)throw std::runtime_error("empty recovery identity");return v;}
        std::wstring path(){auto n=number<std::uint32_t>();if(!n || n>4096 || n>(bytes.size()-at)/sizeof(wchar_t))throw std::runtime_error("invalid recovery path");std::wstring v(n,L'\0');std::memcpy(v.data(),bytes.data()+at,n*sizeof(wchar_t));at+=n*sizeof(wchar_t);if(v.find(L'\0')!=std::wstring::npos)throw std::runtime_error("invalid recovery path");return v;}
        std::vector<std::uint8_t> data(){auto n=number<std::uint64_t>();if(n>MaxData || n>bytes.size()-at)throw std::runtime_error("invalid recovery bytes");std::vector<std::uint8_t> v(bytes.begin()+at,bytes.begin()+at+std::size_t(n));at+=std::size_t(n);return v;}
    };
    struct Record {
        std::string incident;std::wstring target,backup;
        std::uint64_t serial{},generation{};bool existed{};
        std::uint32_t volume{},high{},low{};
        std::vector<std::uint8_t> old;
    };
    static constexpr std::uint64_t MaxData=256ULL*1024*1024,MaxLog=512ULL*1024*1024;
    static constexpr char Magic[8]={'W','I','R','R','C','V','2','\0'};
    std::recursive_mutex mutex_;
    WriterRecoveryLedger ledger_;
    Handle log_,directory_;
    bool initialized_{};
    std::uint64_t size_{},counter_{};
    std::string session_;
    std::string failure_reason_{"not initialized"};
    std::wstring root_,logPath_;
    std::map<std::string,std::shared_ptr<Record>> pending_;
    std::string resolved_,proof_;
    template<class T>static void number(std::vector<std::uint8_t>& b,T v){const auto* p=reinterpret_cast<const std::uint8_t*>(&v);b.insert(b.end(),p,p+sizeof(v));}
    static void text(std::vector<std::uint8_t>& b,const std::string& s){if(s.empty() || s.size()>256)throw std::runtime_error("invalid incident/proof");number(b,std::uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
    static void path(std::vector<std::uint8_t>& b,const std::wstring& s){if(s.empty() || s.size()>4096)throw std::runtime_error("invalid intent path");number(b,std::uint32_t(s.size()));const auto* p=reinterpret_cast<const std::uint8_t*>(s.data());b.insert(b.end(),p,p+s.size()*sizeof(wchar_t));}
    static std::uint64_t checksum(const std::vector<std::uint8_t>& b){std::uint64_t h=14695981039346656037ULL;for(auto c:b){h^=c;h*=1099511628211ULL;}return h;}
    static std::vector<std::uint8_t> read(HANDLE h,std::uint64_t bound) {
        LARGE_INTEGER n{},zero{};if(!GetFileSizeEx(h,&n) || n.QuadPart<0 || std::uint64_t(n.QuadPart)>bound || !SetFilePointerEx(h,zero,nullptr,FILE_BEGIN))throw std::runtime_error("recovery file unavailable");
        std::vector<std::uint8_t> b(std::size_t(n.QuadPart));std::size_t at=0;
        while(at<b.size()){DWORD got{};auto count=DWORD((std::min)(b.size()-at,std::size_t(1048576)));if(!ReadFile(h,b.data()+at,count,&got,nullptr) || !got || got>count)throw std::runtime_error("recovery read refused");at+=got;}return b;
    }
    void append(const std::vector<std::uint8_t>& payload) {
        if(log_.value==INVALID_HANDLE_VALUE || payload.empty() || payload.size()>MaxData+65536 || size_>MaxLog-payload.size()-12)throw std::runtime_error("recovery log unavailable/full");
        LARGE_INTEGER actual{},end{};end.QuadPart=size_;
        if(!GetFileSizeEx(log_.value,&actual) || actual.QuadPart!=std::int64_t(size_) || !SetFilePointerEx(log_.value,end,nullptr,FILE_BEGIN))throw std::runtime_error("recovery log ownership changed");
        std::vector<std::uint8_t> frame;frame.reserve(payload.size()+12);number(frame,std::uint32_t(payload.size()));number(frame,checksum(payload));frame.insert(frame.end(),payload.begin(),payload.end());
        std::size_t at=0;while(at<frame.size()){DWORD wrote{};auto n=DWORD((std::min)(frame.size()-at,std::size_t(1048576)));if(!WriteFile(log_.value,frame.data()+at,n,&wrote,nullptr) || !wrote || wrote>n)throw std::runtime_error("intent append refused");at+=wrote;}
        if(!FlushFileBuffers(log_.value))throw std::runtime_error("intent flush refused");size_+=frame.size();
    }
    static std::string utf8(const std::wstring& s) {
        if(s.empty())return {};
        auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
        if(n<=0 || n>32768)throw std::runtime_error("recovery path encoding refused");std::string v(n,'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),v.data(),n,nullptr,nullptr)!=n)throw std::runtime_error("recovery path encoding refused");return v;
    }
    void replay(const std::vector<std::uint8_t>& bytes) {
        if(bytes.size()<8 || std::memcmp(bytes.data(),Magic,8)!=0)throw std::runtime_error("foreign recovery root");
        Cursor log{bytes,8};std::map<std::string,bool> seen;
        while(log.at<bytes.size()) {
            auto n=log.number<std::uint32_t>();auto hash=log.number<std::uint64_t>();
            if(!n || n>MaxData+65536 || n>bytes.size()-log.at)throw std::runtime_error("partial recovery frame");
            std::vector<std::uint8_t> p(bytes.begin()+log.at,bytes.begin()+log.at+n);log.at+=n;if(checksum(p)!=hash)throw std::runtime_error("damaged recovery frame");
            Cursor c{p};auto kind=c.number<std::uint8_t>();auto id=c.text();
            if(kind==1) {
                if(seen.contains(id) || pending_.size()>=128)throw std::runtime_error("duplicate/unbounded recovery intent");
                auto r=std::make_shared<Record>();r->incident=id;r->serial=c.number<std::uint64_t>();r->generation=c.number<std::uint64_t>();auto exists=c.number<std::uint8_t>();
                if(!r->serial || !r->generation || r->serial>INT64_MAX || r->generation>INT64_MAX || exists>1)throw std::runtime_error("invalid writer tuple");r->existed=exists==1;
                r->volume=c.number<std::uint32_t>();r->high=c.number<std::uint32_t>();r->low=c.number<std::uint32_t>();r->target=c.path();r->backup=c.path();r->old=c.data();
                if(!r->existed && (!r->old.empty() || r->volume || r->high || r->low))throw std::runtime_error("invalid original absence");pending_.emplace(id,r);seen[id]=true;
            } else if(kind==2 || kind==3) {
                auto outcome=c.number<std::uint8_t>();auto proof=c.text();
                if(outcome>2 || !pending_.contains(id))throw std::runtime_error("unbound recovery terminal");
                pending_.erase(id);if(kind==3){resolved_=id;proof_=proof;}
            }else throw std::runtime_error("unknown recovery record");
            if(c.at!=p.size())throw std::runtime_error("trailing recovery data");if(++counter_==INT64_MAX)throw std::runtime_error("recovery revision exhausted");
        }
    }
    static bool expected_file(HANDLE h,const Record& r) {
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(h,&info) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
            && info.dwVolumeSerialNumber==r.volume && info.nFileIndexHigh==r.high && info.nFileIndexLow==r.low && read(h,MaxData)==r.old;
    }
    static bool absent(const std::wstring& path) {
        if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
        return GetLastError()==ERROR_FILE_NOT_FOUND; // inaccessible/missing parent is unknown
    }
    bool terminal(const std::string& id,std::uint8_t kind,std::uint8_t outcome,const std::string& proof) {
        std::vector<std::uint8_t> b;number(b,kind);text(b,id);number(b,outcome);text(b,proof);append(b);return true;
    }
    void unavailable() noexcept {ledger_.unavailable();}
public:
    static WriterRecoveryStore& instance(){static WriterRecoveryStore store;return store;}
    std::string reason(){std::lock_guard lock(mutex_);return failure_reason_;}
    bool initialize() noexcept {
        std::lock_guard lock(mutex_);
        if(initialized_){try{return ledger_.snapshot().state!=WriterRecoveryLedger::State::Unavailable;}catch(...){unavailable();return false;}}
        initialized_=true;
        try {
            static_assert(sizeof(wchar_t)==2,"Win32 recovery wire uses UTF16");
            std::wstring module(32768,L'\0');auto owner=GetModuleHandleW(L"UE4SS.dll");auto n=owner?GetModuleFileNameW(owner,module.data(),DWORD(module.size())):0;
            if(!n || n>=module.size())throw std::runtime_error("host module root unavailable");module.resize(n);auto slash=module.find_last_of(L"\\/");if(slash==std::wstring::npos)throw std::runtime_error("host root unavailable");
            root_=module.substr(0,slash)+L"\\WitchItRecovery";logPath_=root_+L"\\ledger.wir";
            auto attributes=GetFileAttributesW(root_.c_str());bool provision=false;
            if(attributes==INVALID_FILE_ATTRIBUTES){
                if(GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("recovery namespace inaccessible");
                // Only the process that exclusively creates a previously absent
                // namespace may provision it. A race/existing empty directory
                // is evidence to preserve, never authority to reset storage.
                if(!CreateDirectoryW(root_.c_str(),nullptr))throw std::runtime_error("recovery namespace creation refused");
                provision=true;attributes=GetFileAttributesW(root_.c_str());
            }
            if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY) || attributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("foreign/reparse recovery root");
            directory_.value=CreateFileW(root_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            BY_HANDLE_FILE_INFORMATION directory_info{};
            if(directory_.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory_.value,&directory_info)
                || !(directory_info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || directory_info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("recovery namespace pin refused");
            if(provision){
                const auto stagePath=root_+L"\\ledger.wir.new";Handle stage;
                stage.value=CreateFileW(stagePath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(stage.value==INVALID_HANDLE_VALUE)throw std::runtime_error("recovery ledger creation refused");
                DWORD wrote{};
                if(!WriteFile(stage.value,Magic,sizeof(Magic),&wrote,nullptr) || wrote!=sizeof(Magic) || !FlushFileBuffers(stage.value))throw std::runtime_error("recovery header flush refused");
                stage.reset();
                // Atomic namespace publication, WITHOUT MOVEFILE_REPLACE_EXISTING.
                // A failed publish retains the staging evidence for inspection.
                if(!MoveFileExW(stagePath.c_str(),logPath_.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("recovery ledger publication refused");
            }
            log_.value=CreateFileW(logPath_.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            if(log_.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(log_.value,&info) || info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT || info.nNumberOfLinks!=1)throw std::runtime_error("recovery log ownership unavailable");
            auto bytes=read(log_.value,MaxLog);replay(bytes);size_=bytes.size();
            LARGE_INTEGER stamp{};if(!QueryPerformanceCounter(&stamp) || stamp.QuadPart<=0)throw std::runtime_error("recovery session stamp unavailable");
            session_=std::to_string(GetCurrentProcessId())+"-"+std::to_string(stamp.QuadPart);
            if(pending_.size()>1)throw std::runtime_error("multiple pending intents require checked repair");
            if(pending_.empty()){failure_reason_.clear();return ledger_.initialize(session_,true);}
            failure_reason_="unresolved recovery intent";
            auto r=pending_.begin()->second;return ledger_.initialize(session_,false,r->incident,utf8(r->target),utf8(r->backup));
        }catch(const std::exception& e){failure_reason_=e.what();log_.reset();directory_.reset();unavailable();return false;}
        catch(...){failure_reason_="recovery initialization failed";log_.reset();directory_.reset();unavailable();return false;}
    }
    WriterRecoveryLedger::Snapshot snapshot() {
        std::lock_guard lock(mutex_);auto value=ledger_.snapshot();
        if(value.state==WriterRecoveryLedger::State::Ready){value.resolved=resolved_;value.proof=proof_;}return value;
    }
    bool allowed() noexcept {std::lock_guard lock(mutex_);return ledger_.allowed();}
    std::string begin(const void* writer,std::uint64_t serial,std::uint64_t generation,const std::wstring& target,const std::wstring& backup,bool existed,const BY_HANDLE_FILE_INFORMATION& old_info,const std::vector<std::uint8_t>& old) {
        std::lock_guard lock(mutex_);
        if(!ledger_.allowed() || !writer || !serial || !generation || serial>INT64_MAX || generation>INT64_MAX || !pending_.empty() || old.size()>MaxData)throw std::runtime_error("durable writer admission refused");
        try {
            auto r=std::make_shared<Record>();r->incident=session_+"-"+std::to_string(++counter_);r->serial=serial;r->generation=generation;r->target=target;r->backup=backup;r->existed=existed;r->old=old;
            if(existed){r->volume=old_info.dwVolumeSerialNumber;r->high=old_info.nFileIndexHigh;r->low=old_info.nFileIndexLow;}
            std::vector<std::uint8_t> b;number(b,std::uint8_t(1));text(b,r->incident);number(b,serial);number(b,generation);number(b,std::uint8_t(existed));number(b,r->volume);number(b,r->high);number(b,r->low);path(b,target);path(b,backup);number(b,std::uint64_t(old.size()));b.insert(b.end(),old.begin(),old.end());
            pending_.emplace(r->incident,r);append(b); // durable BEFORE native serialization
            if(!ledger_.begin(writer,serial,generation,r->incident,utf8(target),utf8(backup)))throw std::runtime_error("cached writer admission refused");return r->incident;
        }catch(...){unavailable();throw;}
    }
    bool complete(const void* writer,const std::string& incident,bool verified,std::uint8_t outcome) noexcept {
        std::lock_guard lock(mutex_);
        try {
            auto snapshot=ledger_.snapshot();if(!verified || outcome>2 || snapshot.state!=WriterRecoveryLedger::State::Writing || snapshot.incident!=incident || !pending_.contains(incident) || !ledger_.allowed(writer,snapshot.serial,snapshot.generation))return false;
            auto proof=session_+"-terminal-"+std::to_string(++counter_);terminal(incident,2,outcome,proof);
            if(!ledger_.complete(writer,true,true)){unavailable();return false;}pending_.erase(incident);return true;
        }catch(...){unavailable();return false;}
    }
    void uncertain(const void* writer) noexcept {std::lock_guard lock(mutex_);(void)ledger_.complete(writer,false,false);}
    bool resolve_restored(const std::string& incident,std::uint64_t revision) noexcept {
        std::lock_guard lock(mutex_);
        try {
            auto s=ledger_.snapshot();if(s.state!=WriterRecoveryLedger::State::Uncertain || s.incident!=incident || s.revision!=revision || pending_.size()!=1)return false;
            auto r=pending_.begin()->second;
            // Resolution certifies an already restored ORIGINAL. It never moves,
            // deletes or writes a target/backup, and cannot certify original absence.
            if(!r->existed)return false;
            Handle target;target.value=CreateFileW(r->target.c_str(),GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(target.value==INVALID_HANDLE_VALUE || !expected_file(target.value,*r))return false;
            Handle backup;backup.value=CreateFileW(r->backup.c_str(),GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(backup.value==INVALID_HANDLE_VALUE){if(!absent(r->backup))return false;}else if(!expected_file(backup.value,*r))return false;
            auto proof=session_+"-restored-"+std::to_string(++counter_);resolved_=incident;proof_=proof; // allocate before durable record
            terminal(incident,3,1,proof); // pinned verified files remain held through flush
            if(!ledger_.resolve(incident,true,true,proof)){unavailable();return false;}pending_.erase(incident);return true;
        }catch(...){unavailable();return false;}
    }
    bool verify_resolution(const std::string& incident,const std::string& session,std::uint64_t revision,const std::string& proof) {
        std::lock_guard lock(mutex_);auto s=snapshot();return s.state==WriterRecoveryLedger::State::Ready && s.session==session && s.revision==revision && !proof.empty() && resolved_==incident && proof_==proof;
    }
    bool verify_ready(const std::string& session,std::uint64_t revision) {
        std::lock_guard lock(mutex_);auto s=ledger_.snapshot();return !session.empty() && s.state==WriterRecoveryLedger::State::Ready && s.session==session && s.revision==revision;
    }
    std::wstring publication_path(const std::string& incident) {
        std::lock_guard lock(mutex_);auto s=ledger_.snapshot();
        if(s.state!=WriterRecoveryLedger::State::Writing || s.incident!=incident || !pending_.contains(incident))throw std::runtime_error("unowned publication journal");
        for(auto c:incident)if((c<'0' || c>'9') && c!='-')throw std::runtime_error("invalid publication identity");
        return root_+L"\\publish-"+std::wstring(incident.begin(),incident.end())+L".wir";
    }
};
}
