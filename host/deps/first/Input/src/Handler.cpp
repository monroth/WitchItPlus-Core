#include <array>
#include <climits>
#include <cstring>
#include <unordered_set>
#include <limits>
#include <Unreal/Hooks/Internal/CallbackOwnerBridge.hpp>
#if defined(_MSC_VER)
#include <intrin.h>
#define RC_INPUT_ORIGIN _ReturnAddress()
#elif defined(__GNUC__) || defined(__clang__)
#define RC_INPUT_ORIGIN __builtin_extract_return_addr(__builtin_return_address(0))
#else
#define RC_INPUT_ORIGIN nullptr
#endif

#include <Input/Handler.hpp>
#include <Input/PlatformInputSource.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

namespace RC::Input
{
    using CallbackSlot = Unreal::Hook::Internal::TCallbackSlot<EventCallbackCallable>;
    struct OwnedEventCallback::Impl
    {
        std::shared_ptr<CallbackSlot> slot;
        explicit Impl(std::shared_ptr<CallbackSlot> value) : slot(std::move(value)) {}
    };
    auto OwnedEventCallback::capture(const EventCallbackCallable& callback, const void* origin) -> OwnedEventCallback
    {
        OwnedEventCallback result;
        if (!callback) return result;
        auto slot = CallbackSlot::CreateCopy(callback, origin);
        if (slot) result.m_impl = std::make_shared<Impl>(std::move(slot));
        return result;
    }
    OwnedEventCallback::OwnedEventCallback(const EventCallbackCallable& callback)
        : OwnedEventCallback(capture(callback, RC_INPUT_ORIGIN)) {}
    auto OwnedEventCallback::operator=(const EventCallbackCallable& callback) -> OwnedEventCallback&
    {
        auto next = capture(callback, RC_INPUT_ORIGIN);
        m_impl.swap(next.m_impl); // previous manager disposal happens outside Input locks
        return *this;
    }
    auto OwnedEventCallback::operator()() const -> void
    {
        auto value = m_impl;
        if (value) value->slot->Invoke();
    }

    std::unordered_map<std::string, std::shared_ptr<PlatformInputSource>> Handler::m_input_sources_store;

    auto Handler::process_event() -> void
    {
        if (m_platform_handler == nullptr)
        {
            return;
        }

        std::vector<OwnedEventCallback> callbacks_to_call{};

        auto events = m_platform_handler->process_event(this);

        {
            // Lock the event mutex to access the key_set
            auto event_update_lock = std::lock_guard(m_event_mutex);
            for (auto& event : events)
            {
                const auto found = m_key_set.key_data.find(event.key);
                if (found == m_key_set.key_data.end()) continue;
                for (const auto& key_data : found->second)
                {
                    if (key_data.required_modifier_keys == event.modifier_keys)
                    {
                        callbacks_to_call.emplace_back(key_data.callback);
                    }
                }
            }
        }

        // No need to lock the event mutex to call the callbacks
        // this avoids key registration inside the callback
        for (const auto& callback : callbacks_to_call)
        {
            callback();
        }
    }

    auto Handler::register_keydown_event(Input::Key key, EventCallbackCallable callback, uint8_t custom_data, void* custom_data2) -> void
    {
        // Capture at this actual public export. Caller-owned by-value input is
        // moved into an owned slot before entering the Input registry mutex.
        auto slot = CallbackSlot::Create(std::move(callback), RC_INPUT_ORIGIN);
        if (!slot || static_cast<std::size_t>(key) >= m_subscribed_keys.size()) return;
        OwnedEventCallback owned;
        owned.m_impl = std::make_shared<OwnedEventCallback::Impl>(std::move(slot));
        KeyData entry;
        entry.callback = std::move(owned); entry.custom_data = custom_data; entry.custom_data2 = custom_data2;
        {
            auto lock = std::lock_guard(m_event_mutex);
            if (m_next_event_id == std::numeric_limits<std::uint64_t>::max()) return;
            entry.event_id = m_next_event_id++; entry.event_id_owner = this;
            m_key_set.key_data[key].emplace_back(std::move(entry));
            m_subscribed_keys[key] = true;
        }
    }

    auto Handler::register_keydown_event(Input::Key key, const ModifierKeyArray& modifier_keys,
            const EventCallbackCallable& callback, uint8_t custom_data, void* custom_data2) -> void
    {
        register_keydown_event_from_host(key, &modifier_keys, callback, custom_data, custom_data2, RC_INPUT_ORIGIN);
    }

    auto Handler::register_keydown_event_from_host(Input::Key key, const ModifierKeyArray* modifiers,
            const EventCallbackCallable& callback, uint8_t custom_data, void* custom_data2, const void* origin) -> bool
    {
        if (static_cast<std::size_t>(key) >= m_subscribed_keys.size()) return false;
        KeyData entry;
        entry.callback = OwnedEventCallback::capture(callback, origin);
        if (!entry.callback) return false;
        entry.custom_data = custom_data; entry.custom_data2 = custom_data2;
        if (modifiers) { entry.requires_modifier_keys = true; entry.required_modifier_keys = *modifiers; }
        {
            auto lock = std::lock_guard(m_event_mutex);
            if (m_next_event_id == std::numeric_limits<std::uint64_t>::max()) return false;
            entry.event_id = m_next_event_id++; entry.event_id_owner = this;
            m_key_set.key_data[key].emplace_back(std::move(entry));
            m_subscribed_keys[key] = true;
        }
        return true;
    }

    auto Handler::is_keydown_event_registered(Input::Key key) -> bool
    {
        auto event_update_lock = std::lock_guard(m_event_mutex);
        auto key_data = m_key_set.key_data.find(key);
        if (key_data == m_key_set.key_data.end())
        {
            return false;
        }
        for (const auto& key_data_container : key_data->second)
        {
            if (key_data_container.required_modifier_keys.empty())
            {
                return true;
            }
        }
        return false;
    }

    auto Handler::is_keydown_event_registered(Input::Key key, const ModifierKeyArray& modifier_keys_array) -> bool
    {
        auto event_update_lock = std::lock_guard(m_event_mutex);
        auto key_data = m_key_set.key_data.find(key);
        auto modifier_keys = ModifierKeys(modifier_keys_array);
        if (key_data == m_key_set.key_data.end())
        {
            return false;
        }
        for (const auto& key_data_container : key_data->second)
        {
            if (key_data_container.required_modifier_keys == modifier_keys)
            {
                return true;
            }
        }
        return false;
    }

    auto Handler::has_event_on_key(Input::Key key) -> bool
    {
        auto lock = std::lock_guard(m_event_mutex);
        return static_cast<std::size_t>(key) < m_subscribed_keys.size() && m_subscribed_keys[key];
    }

    auto Handler::get_events_safe(std::function<void(KeySet&)> callback) -> void
    {
        get_events_safe_from_host(std::move(callback), RC_INPUT_ORIGIN);
    }

    auto Handler::get_events_safe_from_host(std::function<void(KeySet&)> callback, const void* origin) -> void
    {
        using VisitorSlot = Unreal::Hook::Internal::TCallbackSlot<std::function<void(KeySet&)>>;
        auto visitor = VisitorSlot::Create(std::move(callback), origin);
        if (!visitor) return;
        KeySet next;
        std::unordered_set<std::uint64_t> original_ids;
        {
            auto lock = std::lock_guard(m_event_mutex);
            next = m_key_set; // copies only stable host callback control blocks
            for (const auto& [key, entries] : next.key_data)
                for (const auto& entry : entries) original_ids.insert(entry.event_id);
        }
        // Erase/edit, arbitrary visitor code, and Native manager disposal are
        // all outside the Input mutex. Unknown custom_data2 remains caller-owned.
        if (!visitor->Invoke(next)) return;
        std::unordered_set<std::uint64_t> published_ids;
        {
            auto lock = std::lock_guard(m_event_mutex);
            // Registrations added while the visitor ran remain intact. A later
            // edit of the same original event wins in publication order.
            for (const auto& [key, entries] : m_key_set.key_data)
                for (const auto& entry : entries)
                    if (!original_ids.contains(entry.event_id)) next.key_data[key].push_back(entry);
            for (auto& [key, entries] : next.key_data)
                for (auto& entry : entries) {
                    if (entry.event_id_owner != this || !entry.event_id || !published_ids.insert(entry.event_id).second) {
                        if (m_next_event_id == std::numeric_limits<std::uint64_t>::max()) return;
                        entry.event_id = m_next_event_id++; entry.event_id_owner = this;
                        published_ids.insert(entry.event_id);
                    }
                }
            m_key_set.key_data.swap(next.key_data);
            m_subscribed_keys.fill(false);
            for (const auto& [key, entries] : m_key_set.key_data)
                if (!entries.empty() && static_cast<std::size_t>(key) < m_subscribed_keys.size()) m_subscribed_keys[key] = true;
        }
        // next now owns displaced storage; last callback references drop here.
    }

    auto Handler::clear_subscribed_keys() -> void
    {
        auto lock = std::lock_guard(m_event_mutex);
        m_subscribed_keys.fill(false);
    }

    auto Handler::clear_subscribed_key(Key k) -> void
    {
        auto lock = std::lock_guard(m_event_mutex);
        if (static_cast<std::size_t>(k) < m_subscribed_keys.size()) m_subscribed_keys[k] = false;
    }

    auto Handler::get_allow_input() -> bool
    {
        return m_allow_input;
    }

    auto Handler::set_allow_input(bool new_value) -> void
    {
        m_allow_input = new_value;
    }

    /// Set the input source to the given source
    /// SAFETY: Only call this function from the main thread
    auto Handler::set_input_source(std::string source) -> bool
    {
        auto event_update_lock = std::lock_guard(m_event_mutex);
        std::shared_ptr<PlatformInputSource> next_input_source = nullptr;
        if (source == "Default")
        {
            // find the highest priority input source
            int highest_priority = INT_MAX;
            for (auto& input_source : m_input_sources_store)
            {
                if (!input_source.second->is_available())
                {
                    continue;
                }
                auto priority = input_source.second->source_priority();
                if (priority < highest_priority)
                {
                    next_input_source = input_source.second;
                    highest_priority = priority;
                }
            }
            if (highest_priority == INT_MAX)
            {
                Output::send<LogLevel::Error>(STR("Input source: Source not found\n"));
                return false;
            }
        }
        else
        {
            auto input_source = m_input_sources_store.find(source);
            if (input_source == m_input_sources_store.end())
            {
                return false;
            }
            next_input_source = input_source->second;
            if (!next_input_source->is_available())
            {
                return false;
            }
        }
        if (next_input_source != m_platform_handler)
        {
            if (m_platform_handler != nullptr)
            {
                m_platform_handler->deactivate();
            }
            next_input_source->activate();
            m_platform_handler = next_input_source;
            return true;
        }
        return true;
    }

    // register the input source to the input source store
    auto Handler::register_input_source(std::shared_ptr<PlatformInputSource> input_source) -> void
    {
        std::string name = input_source->get_name();
        if (m_input_sources_store.find(name) == m_input_sources_store.end())
        {
            m_input_sources_store[name] = input_source;
        }
    }

    auto Handler::get_current_input_source() -> std::string
    {
        if (m_platform_handler == nullptr)
        {
            return "None";
        }
        return m_platform_handler->get_name();
    }
} // namespace RC::Input
