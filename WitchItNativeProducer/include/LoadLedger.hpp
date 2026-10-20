#pragma once
#include "OperationLedger.hpp"
#include <unordered_set>

namespace witchit {
struct LoadEntry {
    std::uint64_t serial{},bridge_generation{};
    std::uintptr_t owner_address{}; // copied scalar identity; never dereferenced
    std::string owner_key,actual_target;
    std::string layout_name,layout_world,layout_path; // detached exact input Layout fields
    bool compressed{},recovery{},recovery_intent_known{},captured{},reader_seen{},reader_done{},reader_success{};
    bool application_seen{},application_done{};
    std::unordered_set<FrameKey> application_frames;
};
class LoadLedger {
    std::uint64_t serial_{};
    std::unordered_map<FrameKey,LoadEntry> loads_;
public:
    std::optional<LoadEntry> enter(FrameKey frame,std::uint64_t generation,
                                   std::string owner,std::uintptr_t address,bool compressed,bool recovery) {
        if(!frame || !address || loads_.contains(frame) || serial_==INT64_MAX)return {};
        LoadEntry e;
        e.serial=++serial_;e.bridge_generation=generation;e.owner_key=std::move(owner);
        e.owner_address=address;
        e.compressed=compressed;e.recovery=recovery;
        loads_.emplace(frame,e);return e;
    }
    LoadEntry* find(FrameKey frame) {
        auto it=loads_.find(frame);return it==loads_.end()?nullptr:&it->second;
    }
    bool reader(FrameKey frame,std::string path,bool compressed) {
        auto e=find(frame);
        if(!e || e->reader_seen || e->compressed!=compressed || path.empty())return false;
        e->reader_seen=true;e->actual_target=std::move(path);return true;
    }
    bool read(FrameKey frame,const std::string& path,bool success) {
        auto e=find(frame);
        if(!e || !e->reader_seen || e->reader_done || path!=e->actual_target)return false;
        e->reader_done=true;e->reader_success=success;return true;
    }
    bool apply_enter(FrameKey frame,FrameKey application,std::uintptr_t address) {
        auto e=find(frame);
        if(!e || !application || !address || address!=e->owner_address ||
           !e->reader_done || !e->reader_success || e->application_done)return false;
        if(!e->application_frames.insert(application).second)return false;
        e->application_seen=true;return true;
    }
    bool apply_return(FrameKey frame,FrameKey application) {
        auto e=find(frame);
        if(!e || !e->application_frames.erase(application))return false;
        e->application_done=e->application_frames.empty();return true;
    }
    std::optional<LoadEntry> finish(FrameKey frame,std::uint64_t generation) {
        auto it=loads_.find(frame);if(it==loads_.end())return {};
        auto e=std::move(it->second);loads_.erase(it);
        if(e.bridge_generation!=generation)return {};
        // Reports facts, not aggregate actor validity or destination-world ready.
        return e;
    }
    void abandon(FrameKey frame){loads_.erase(frame);}
    void retire(){loads_.clear();}
};
// Every exact LoadPropLayout invocation is a scope barrier, even when capture
// is rejected or later abandoned. An inner invocation cannot borrow an outer
// invocation's successful reader. Frame addresses are live-stack identities.
class LoadBarriers {
    std::unordered_set<FrameKey> frames_;
public:
    void enter(FrameKey frame){if(frame)frames_.insert(frame);}
    void leave(FrameKey frame){frames_.erase(frame);}
    void retire(){frames_.clear();}
    template<class Parent,class Admitted>
    FrameKey nearest(FrameKey frame,Parent parent,Admitted admitted) const {
        for(int depth=0;frame && depth<64;++depth,frame=parent(frame))
            if(frames_.contains(frame))return admitted(frame)?frame:0;
        return 0;
    }
};
}
