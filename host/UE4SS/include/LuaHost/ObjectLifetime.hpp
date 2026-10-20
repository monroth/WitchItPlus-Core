#pragma once
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace RC::LuaHost {
// The existing host UObject map, with an incarnation per live address. The
// deletion listener revokes old wrappers even if Unreal later reuses an address.
// No engine calls, allocation of Unreal serials, Lua tokens, owners or worlds.
class ObjectLifetime final {
    mutable std::mutex mutex_;
    std::unordered_map<const void*,std::uint64_t> objects_;
    std::uint64_t sequence_{};
public:
    std::uint64_t observe(const void* object) {
        if(!object)return 0;
        std::lock_guard lock(mutex_);
        if(auto i=objects_.find(object);i!=objects_.end())return i->second;
        if(sequence_==UINT64_MAX)throw std::runtime_error("Native object lifetime exhausted");
        auto stamp=++sequence_;objects_.emplace(object,stamp);return stamp;
    }
    void erase(const void* object) {std::lock_guard lock(mutex_);objects_.erase(object);}
    bool current(const void* object,std::uint64_t stamp) const {
        std::lock_guard lock(mutex_);auto i=objects_.find(object);
        return stamp && i!=objects_.end() && i->second==stamp;
    }
    void clear() {std::lock_guard lock(mutex_);objects_.clear();}
    bool contains(const void* object) const {std::lock_guard lock(mutex_);return objects_.contains(object);}
};
}
