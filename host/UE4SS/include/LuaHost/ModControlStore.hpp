#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace RC::LuaHost {
// Pure wrapper ownership only. No method, callback, Native manager, VM close,
// DLL unload or destructor runs while mutex_ is held. The event-thread owner
// alone drains retired records; a borrower never becomes the final owner.
template<class T, class Id>
class ModControlStore {
    mutable std::mutex mutex_;
    std::map<Id, std::shared_ptr<T>> active_;
    std::map<Id, std::shared_ptr<T>> retired_;
public:
    void publish(Id id, std::shared_ptr<T> object) {
        if (!object) return;
        std::lock_guard lock{mutex_};
        // IDs belong to an immutable host incarnation and are never reused.
        if (active_.contains(id) || retired_.contains(id)) return;
        active_.emplace(id, std::move(object));
    }
    std::shared_ptr<T> borrow(Id id) const {
        std::lock_guard lock{mutex_};
        auto found = active_.find(id);
        return found == active_.end() ? std::shared_ptr<T>{} : found->second;
    }
    std::vector<std::shared_ptr<T>> snapshot() const {
        std::vector<std::shared_ptr<T>> result;
        std::lock_guard lock{mutex_};
        result.reserve(active_.size());
        for (const auto& [id, object] : active_) result.push_back(object);
        return result;
    }
    Id id_of(const T* object) const {
        // Pointer comparison only: legacy pointer entrypoints must not read a
        // potentially deleted object to obtain its ID.
        std::lock_guard lock{mutex_};
        for (const auto& [id, owned] : active_) if (owned.get() == object) return id;
        return {};
    }
    void retire(Id id) {
        std::lock_guard lock{mutex_};
        auto found = active_.find(id);
        if (found == active_.end()) return;
        // Allocate the retained slot before removing its original owner.
        retired_.emplace(id, found->second);
        active_.erase(found);
    }
    void retire_all() {
        std::lock_guard lock{mutex_};
        // Allocation may fail, but no ownership is dropped on that path.
        for (const auto& [id, object] : active_) retired_.emplace(id, object);
        active_.clear();
    }
    std::vector<std::shared_ptr<T>> take_released() {
        std::vector<std::shared_ptr<T>> result;
        std::lock_guard lock{mutex_};
        result.reserve(retired_.size());
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (it->second.use_count() != 1) { ++it; continue; }
            result.push_back(std::move(it->second));
            it = retired_.erase(it);
        }
        return result; // Event-thread caller destroys these OUTSIDE mutex_.
    }
    bool empty() const {
        std::lock_guard lock{mutex_};
        return active_.empty() && retired_.empty();
    }
};
}
