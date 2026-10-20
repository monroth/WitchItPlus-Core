#pragma once
#include <cstdint>
#include <string>
#include <mutex>

namespace RC::Unreal::Internal {
// Pure process-wide state. Storage initialization/publication evidence belongs
// to the SDK backend; no Lua flag or guessed path can set READY.
class WriterRecoveryLedger final {
public:
    enum class State {Unavailable,Ready,Writing,Uncertain};
    struct Snapshot {
        std::string session,incident,target,source,resolved,proof;
        std::uint64_t revision{},serial{},generation{};
        State state{State::Unavailable};
    };
private:
    mutable std::mutex mutex_;
    Snapshot value_;
    const void* writer_{};
    bool initialized_{};
    bool advance() noexcept {
        if(value_.revision==INT64_MAX){value_.state=State::Unavailable;writer_=nullptr;return false;}
        ++value_.revision;return true;
    }
public:
    Snapshot snapshot() const {std::lock_guard lock(mutex_);return value_;}
    bool initialize(std::string session,bool storage_known_clean,std::string incident={},std::string target={},std::string source={}) {
        std::lock_guard lock(mutex_);if(initialized_ || session.empty())return false;
        initialized_=true;value_.session=std::move(session);value_.incident=std::move(incident);
        value_.target=std::move(target);value_.source=std::move(source);
        value_.state=storage_known_clean?State::Ready:value_.incident.empty()?State::Unavailable:State::Uncertain;
        return advance();
    }
    bool begin(const void* writer,std::uint64_t serial,std::uint64_t generation,std::string incident,std::string target,std::string source) {
        std::lock_guard lock(mutex_);
        if(!writer || !serial || !generation || incident.empty() || target.empty() || value_.state!=State::Ready)return false;
        value_.incident=std::move(incident);value_.target=std::move(target);value_.source=std::move(source);
        value_.serial=serial;value_.generation=generation;writer_=writer;value_.state=State::Writing;return advance();
    }
    bool allowed(const void* writer=nullptr,std::uint64_t serial=0,std::uint64_t generation=0) const noexcept {
        std::lock_guard lock(mutex_);return value_.state==State::Ready ||
            (value_.state==State::Writing && writer && writer==writer_ && serial==value_.serial && generation==value_.generation);
    }
    bool complete(const void* writer,bool verified_terminal,bool durable_terminal) noexcept {
        std::lock_guard lock(mutex_);
        if(!writer || writer!=writer_ || value_.state!=State::Writing)return false;
        value_.state=verified_terminal && durable_terminal?State::Ready:State::Uncertain;writer_=nullptr;
        if(value_.state==State::Ready){value_.incident.clear();value_.target.clear();value_.source.clear();value_.serial=0;value_.generation=0;}
        return advance();
    }
    bool resolve(const std::string& incident,bool verified,bool durable,std::string proof) {
        std::lock_guard lock(mutex_);
        if(value_.state!=State::Uncertain || incident.empty() || incident!=value_.incident || !verified || !durable || proof.empty())return false;
        value_.resolved=incident;value_.proof=std::move(proof);value_.state=State::Ready;
        value_.incident.clear();value_.target.clear();value_.source.clear();value_.serial=0;value_.generation=0;
        return advance();
    }
    void unavailable() noexcept {std::lock_guard lock(mutex_);value_.state=State::Unavailable;writer_=nullptr;(void)advance();}
};
}
