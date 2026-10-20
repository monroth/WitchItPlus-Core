#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace witchit {
using FrameKey = std::uintptr_t;
struct SaveEntry {
    std::uint64_t serial{};
    std::uint64_t bridge_generation{};
    std::string owner_key;
    std::string requested_name;
    std::string actual_name; // native SafeLayoutName copied from the same invocation
    bool compressed{};
    bool autosave{};
    bool captured{};
    bool writer_seen{};
    bool writer_admitted{};
    bool writer_done{};
    bool native_success{};
    std::string actual_target;
};
// The key identifies an active invocation, never a last-save/FIFO/path guess.
// Runtime access is game-thread-only; detached entries contain scalar data.
class OperationLedger {
    std::uint64_t serial_{};
    std::unordered_map<FrameKey,SaveEntry> saves_;
public:
    std::optional<SaveEntry> enter(FrameKey frame,std::uint64_t generation,
                                   std::string owner,std::string name,
                                   bool compressed,bool autosave) {
        if (!frame || saves_.contains(frame) || serial_==INT64_MAX) return {};
        SaveEntry e;
        e.serial=++serial_;e.bridge_generation=generation;e.owner_key=std::move(owner);
        e.requested_name=std::move(name);e.compressed=compressed;e.autosave=autosave;
        saves_.emplace(frame,e);
        return e;
    }
    SaveEntry* find(FrameKey frame) {
        auto it=saves_.find(frame);return it==saves_.end()?nullptr:&it->second;
    }
    bool captured(FrameKey frame,std::uint64_t serial,bool accepted) {
        auto e=find(frame);if(!e || e->serial!=serial)return false;
        e->captured=accepted;return true;
    }
    bool writer(FrameKey frame,std::uint64_t serial,std::string target,bool compressed) {
        auto e=find(frame);
        if(!e || e->serial!=serial || e->writer_seen || e->compressed!=compressed || target.empty())return false;
        e->writer_seen=true;e->actual_target=std::move(target);return true;
    }
    bool admitted(FrameKey frame,std::uint64_t serial,bool value) {
        auto e=find(frame);if(!e || e->serial!=serial || !e->writer_seen)return false;
        e->writer_admitted=e->captured && value;return true;
    }
    bool written(FrameKey frame,std::uint64_t serial,const std::string& target,bool success) {
        auto e=find(frame);
        if(!e || e->serial!=serial || !e->writer_seen || e->writer_done || e->actual_target!=target)return false;
        e->writer_done=true;e->native_success=success;return true;
    }
    std::optional<SaveEntry> finish(FrameKey frame,std::uint64_t generation,
                                    const std::string& actual_target,bool actual_success,
                                    const std::string& actual_name) {
        auto it=saves_.find(frame);if(it==saves_.end())return {};
        SaveEntry e=std::move(it->second);saves_.erase(it); // detach before outward callbacks
        if(e.bridge_generation!=generation || !e.writer_done || e.actual_target!=actual_target ||
            e.native_success!=actual_success)return {};
        e.actual_name=actual_name;
        return e;
    }
    void abandon(FrameKey frame) {saves_.erase(frame);}
    void retire() {saves_.clear();} // no access to saved UObject/frame pointers
    std::size_t active() const {return saves_.size();}
};
}
