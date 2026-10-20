#pragma once
#include <algorithm>
#include <functional>
#include <mutex>
#include <vector>

namespace RC::LuaHost {
using HostEvent = std::function<void()>;
// Move payloads under the queue lock; execute and destroy them after releasing
// it. Newly queued events are left for a later batch, including reentrant work.
inline std::vector<HostEvent> take_event_batch(std::vector<HostEvent>& events,
                                             std::mutex& mutex, std::size_t limit) {
    std::vector<HostEvent> batch;
    batch.reserve(limit);
    std::lock_guard lock{mutex};
    const auto count = std::min(events.size(), limit);
    for (std::size_t i = 0; i < count; ++i) batch.push_back(std::move(events[i]));
    events.erase(events.begin(), events.begin() + count);
    return batch;
}
inline std::vector<HostEvent> detach_events(std::vector<HostEvent>& events, std::mutex& mutex) {
    std::vector<HostEvent> detached;
    std::lock_guard lock{mutex};
    detached.swap(events);
    return detached;
}
}
