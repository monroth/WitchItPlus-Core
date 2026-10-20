#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaMadeSimple/LuaObject.hpp>
#include <LuaMadeSimple/HostFunctionHooks.hpp>
#include <LuaMadeSimple/HostAuxiliaryHooks.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <LuaMadeSimple/HostThreadAccess.hpp>
#include <LuaMadeSimple/HostTablePlan.hpp>
#include "HostFunctionFrame.h"
#include "HostExecutionFrame.h"
#include <atomic>
#include <lstate.h>

#include <Helpers/String.hpp>

namespace RC::LuaMadeSimple
{
    // All lua instances, lua_State* are stored in the Lua class
    static std::unordered_map<lua_State*, std::shared_ptr<Lua>> lua_instances;
    static std::mutex lua_instances_mutex;
    // C++ Lua references are borrowed by host containers. Retain overwritten
    // wrappers as inert host tombstones; their immutable epoch rejects a reused
    // lua_State address without changing the public Lua class layout.
    static std::vector<std::shared_ptr<Lua>> retired_lua_wrappers;
    struct RootIdentity { lua_State* root; std::uint64_t epoch; };
    static auto& lua_root_identities = *new std::unordered_map<lua_State*, RootIdentity>;
    static auto& lua_object_identities = *new std::unordered_map<const Lua*, RootIdentity>;
    static std::uint64_t next_root_epoch{1};
    static auto& lua_root_identities_mutex = *new std::mutex;
    static auto store_wrapper(lua_State* actual, std::shared_ptr<Lua> wrapper) -> Lua& {
        std::lock_guard lock{lua_instances_mutex};
        const auto previous = lua_instances.find(actual);
        if (previous != lua_instances.end()) retired_lua_wrappers.push_back(previous->second);
        return *lua_instances.insert_or_assign(actual, std::move(wrapper)).first->second;
    }
    auto adopt_host_thread(lua_State* actual, lua_State* root, std::uint64_t* receipt) noexcept -> bool {
        if (receipt) *receipt = 0;
        if (!actual || !root) return false;
        try {
            {
                std::lock_guard lock{lua_root_identities_mutex};
                const auto parent = lua_root_identities.find(root);
                if (parent == lua_root_identities.end() || parent->second.root != root) return false;
                const RootIdentity proven = parent->second;
                // Trusted host already proved G(actual)==G(root) under its real
                // scope/gate. Record that fact BEFORE constructing the wrapper.
                lua_root_identities.insert_or_assign(actual, proven);
                if (receipt) *receipt = proven.epoch; // survives later C++ allocation failure
            }
            {
                std::lock_guard lock{lua_instances_mutex};
                const auto existing = lua_instances.find(actual);
                if (existing != lua_instances.end() && existing->second->get_root_lua_state() == root) {
                    if (receipt) { std::lock_guard identities{lua_root_identities_mutex}; *receipt = lua_root_identities.at(actual).epoch; }
                    return true;
                }
            }
            auto wrapper = std::make_shared<Lua>(actual);
            store_wrapper(actual, std::move(wrapper));
            if (receipt) { std::lock_guard lock{lua_root_identities_mutex}; *receipt = lua_root_identities.at(actual).epoch; }
            return true;
        } catch (...) { return false; }
    }

    auto adopt_host_thread(lua_State* actual, lua_State* root) noexcept -> bool {
        return adopt_host_thread(actual, root, nullptr);
    }

    struct FunctionSlot {
        std::optional<Lua::LuaFunction> function;
        std::uint64_t ticket{};
    };
    static std::vector<FunctionSlot> lua_functions;
    static std::mutex lua_functions_mutex;
    static HostFunctionHooks host_function_hooks;
    static std::atomic<bool> host_function_hooks_installed{};
    auto install_host_function_hooks(const HostFunctionHooks& hooks) noexcept -> bool {
        if (!hooks.Register || !hooks.Dispatch || !hooks.DispatchHost) return false;
        std::lock_guard lock{lua_functions_mutex};
        if (host_function_hooks_installed.load(std::memory_order_relaxed)) return false;
        host_function_hooks = hooks;
        host_function_hooks_installed.store(true, std::memory_order_release);
        return true;
    }
    static auto register_function_slot(const Lua& lua, Lua::LuaFunction function) -> std::size_t {
        std::uint64_t ticket{};
        if (host_function_hooks_installed.load(std::memory_order_acquire) &&
            host_function_hooks.Register(lua, function, &ticket) != 0)
            throw std::runtime_error("Host refused LuaMadeSimple function ownership");
        std::lock_guard lock{lua_functions_mutex};
        const auto id = lua_functions.size();
        lua_functions.push_back({function, ticket});
        return id;
    }
    auto prepare_host_table_function(const Lua& lua, Lua::LuaFunction function) -> HostTableFunction {
        const auto id = register_function_slot(lua, function);
        return {static_cast<lua_Integer>(id), static_cast<lua_Integer>(Lua::LuaFunctionType::Local), process_lua_function};
    }
    auto discard_host_table_function(HostTableFunction value) noexcept -> void {
        std::lock_guard lock{lua_functions_mutex};
        if (value.id >= 0 && static_cast<std::size_t>(value.id) < lua_functions.size())
            lua_functions[static_cast<std::size_t>(value.id)].function.reset();
    }
    static auto publish_function(const Lua& lua, std::size_t id, Lua::LuaFunctionType type,
                                 const char* name = nullptr) -> void {
        const lms_host_function_request request{static_cast<lua_Integer>(id), static_cast<lua_Integer>(type),
                                                 name, process_lua_function};
        if (lms_host_function_publish(lua.get_lua_state(), &request) != LUA_OK) {
            std::lock_guard lock{lua_functions_mutex};
            lua_functions[id].function.reset();
            throw std::runtime_error("Protected LuaMadeSimple function publication failed");
        }
    }

    struct InternalFunctionRecord { std::uint64_t ticket; int anchor; };
    static std::mutex internal_functions_mutex;
    static std::unordered_map<lua_State*,std::unordered_map<const void*,InternalFunctionRecord>> internal_functions;
    struct DebugHookRecord { lua_State* root; std::uint64_t ticket; };
    static std::mutex debug_hooks_mutex;
    static std::unordered_map<lua_State*,DebugHookRecord> debug_hooks;
    static int debug_thread_notice(lua_State* parent,lua_State* child,lua_Hook inherited,int born) noexcept {
        try {
            std::lock_guard lock{debug_hooks_mutex};
            if(!born || inherited!=lms_managed_debug_hook) {debug_hooks.erase(child);return 0;}
            const auto record=debug_hooks.find(parent);
            if(record==debug_hooks.end()) return 1;
            debug_hooks.insert_or_assign(child,record->second);
            return 0;
        } catch (...) {return 1;}
    }


    // A Native __index delegate remains in the legacy MetaMethods structure,
    // but its callable identity resolves only to a host-managed slot.
    static std::mutex meta_delegate_mutex;
    static std::unordered_map<lua_State*,std::unordered_map<std::uintptr_t,std::size_t>> meta_delegate_slots;
    static auto register_meta_delegate(const Lua& lua,Lua::LuaFunction function) -> void {
        const auto id=register_function_slot(lua,function);
        std::lock_guard lock{meta_delegate_mutex};
        meta_delegate_slots[lua.get_root_lua_state()].insert_or_assign(reinterpret_cast<std::uintptr_t>(function),id);
    }
    static auto dispatch_meta_delegate(const Lua& lua,Lua::LuaFunction identity) -> int {
        std::size_t id;
        { std::lock_guard lock{meta_delegate_mutex};
          const auto root=meta_delegate_slots.find(lua.get_root_lua_state());
          if(root==meta_delegate_slots.end()) throw std::runtime_error("Unregistered __index delegate");
          const auto found=root->second.find(reinterpret_cast<std::uintptr_t>(identity));
          if(found==root->second.end()) throw std::runtime_error("Unregistered __index delegate");
          id=found->second; }
        std::uint64_t ticket;Lua::LuaFunction verified_host{};
        {std::lock_guard lock{lua_functions_mutex};
         if(id>=lua_functions.size() || !lua_functions[id].function) throw std::runtime_error("Expired __index delegate");
         ticket=lua_functions[id].ticket;
         if(!ticket) verified_host=lua_functions[id].function.value();}
        int result{};
        if(ticket) {
            if(host_function_hooks.Dispatch(ticket,lua,&result)!=0) throw std::runtime_error("Host refused __index delegate");
            return result;
        }
        if(host_function_hooks.DispatchHost(lua,verified_host,&result)!=0) throw std::runtime_error("Host refused __index helper");
        return result;
    }

    // Current errors for all lua states
    static std::unordered_map<lua_State*, std::string> lua_state_errors;
    static std::mutex lua_state_errors_mutex;
    auto forget_closed_host_thread(lua_State* actual, std::uint64_t expected_epoch) noexcept -> void {
        if (!actual || !expected_epoch) return;
        try {
            std::scoped_lock lock{lua_instances_mutex, lua_root_identities_mutex};
            const auto identity = lua_root_identities.find(actual);
            if (identity == lua_root_identities.end() || identity->second.epoch != expected_epoch) return;
            const auto found = lua_instances.find(actual);
            if (found != lua_instances.end()) {
                const auto object = lua_object_identities.find(found->second.get());
                if (object == lua_object_identities.end() || object->second.epoch != expected_epoch) return;
                retired_lua_wrappers.push_back(found->second);
                lua_instances.erase(found);
            }
            { std::lock_guard errors{lua_state_errors_mutex}; lua_state_errors.erase(actual); }
            lua_root_identities.erase(identity);
        } catch (...) { std::terminate(); }
    }
    auto forget_closed_host_thread(lua_State*) noexcept -> void {
        // Legacy callers have no receipt to distinguish a reused raw address.
        // Refuse blind erasure; the VM finalizer invalidates the whole old root.
    }
    static auto forget_auxiliary_root(lua_State*,std::uint64_t) -> void;
    auto forget_closed_host_vm(lua_State* root) noexcept -> void {
        // Called only after actual lua_close. No root/stack memory is touched.
        try {
            std::scoped_lock lock{lua_instances_mutex, lua_root_identities_mutex, lua_state_errors_mutex};
            const auto root_identity = lua_root_identities.find(root);
            if (root_identity == lua_root_identities.end() || root_identity->second.root != root) return;
            const auto epoch = root_identity->second.epoch;
            forget_auxiliary_root(root,epoch);
            for (auto it = lua_root_identities.begin(); it != lua_root_identities.end();) {
                if (it->second.root != root || it->second.epoch != epoch) { ++it; continue; }
                const auto actual = it->first;
                const auto wrapper = lua_instances.find(actual);
                if (wrapper != lua_instances.end()) {
                    const auto identity = lua_object_identities.find(wrapper->second.get());
                    if (identity != lua_object_identities.end() && identity->second.epoch == epoch) {
                        retired_lua_wrappers.push_back(wrapper->second);
                        lua_instances.erase(wrapper);
                    }
                }
                lua_state_errors.erase(actual);
                it = lua_root_identities.erase(it);
            }
        } catch (...) { std::terminate(); }
    }

    static HostAuxiliaryHooks host_auxiliary_hooks;
    static std::atomic<bool> host_auxiliary_hooks_installed{};
    static std::mutex host_auxiliary_mutex;
    auto install_host_auxiliary_hooks(const HostAuxiliaryHooks& hooks) noexcept -> bool {
        if (!hooks.Register || !hooks.Dispatch || !hooks.DispatchHost) return false;
        try {
            if(!LuaHostInstallThreadNotice(debug_thread_notice)) return false;
            std::lock_guard lock{host_auxiliary_mutex};
            if (host_auxiliary_hooks_installed.load(std::memory_order_relaxed)) return false;
            host_auxiliary_hooks=hooks;
            host_auxiliary_hooks_installed.store(true,std::memory_order_release);
            return true;
        } catch (...) {return false;}
    }
    static auto auxiliary_ticket(const Lua* lua,HostAuxiliaryKind kind,const void* function) -> std::uint64_t {
        std::uint64_t ticket{};
        if (!host_auxiliary_hooks_installed.load(std::memory_order_acquire) ||
            host_auxiliary_hooks.Register(lua,kind,function,&ticket)!=0)
            throw std::runtime_error("Host refused auxiliary callback ownership");
        return ticket;
    }
    struct ErrorCallbackRecord {std::uintptr_t identity;LuaErrorCallback host_function;std::uint64_t ticket;};
    static std::vector<ErrorCallbackRecord> lua_error_callbacks;
    static std::unordered_map<const Lua*,std::vector<std::uint64_t>> native_post_callbacks;
    static std::mutex native_post_callbacks_mutex;
    static auto forget_auxiliary_root(lua_State* root,std::uint64_t epoch) -> void {
        { std::lock_guard lock{meta_delegate_mutex};meta_delegate_slots.erase(root); }
            { std::lock_guard internal{internal_functions_mutex}; internal_functions.erase(root); }
        { std::lock_guard hooks{debug_hooks_mutex};std::erase_if(debug_hooks,[root](const auto& row){return row.second.root==root;}); }
        std::lock_guard lock{native_post_callbacks_mutex};
        std::erase_if(native_post_callbacks,[&](const auto& row) {
            const auto identity=lua_object_identities.find(row.first);
            return identity!=lua_object_identities.end() && identity->second.root==root && identity->second.epoch==epoch;
        });
    }
    static std::mutex lua_error_callbacks_mutex;

    auto register_error_callback(LuaErrorCallback callback) -> void
    {
        if (!callback) return;
        const auto ticket=auxiliary_ticket(nullptr,HostAuxiliaryKind::error,reinterpret_cast<const void*>(callback));
        std::lock_guard<std::mutex> lock(lua_error_callbacks_mutex);
        lua_error_callbacks.push_back({reinterpret_cast<std::uintptr_t>(callback),ticket?nullptr:callback,ticket});
    }

    auto unregister_error_callback(LuaErrorCallback callback) -> void
    {
        std::lock_guard<std::mutex> lock(lua_error_callbacks_mutex);
        std::erase_if(lua_error_callbacks,[callback](const auto& row){return row.identity==reinterpret_cast<std::uintptr_t>(callback);});
    }

    static auto notify_error_callbacks(lua_State* L, const std::string& error_message, const std::string& traceback) -> void
    {
        std::vector<ErrorCallbackRecord> snapshot;
        { std::lock_guard lock{lua_error_callbacks_mutex}; snapshot=lua_error_callbacks; }
        // Native callbacks and their managers never run under the registry lock.
        for (const auto& callback : snapshot) {
            if (callback.ticket) {
                HostAuxiliaryInvocation invocation{nullptr,L,&error_message,&traceback,0};
                (void)host_auxiliary_hooks.Dispatch(callback.ticket,&invocation);
            } else if (callback.host_function) {
                HostAuxiliaryInvocation invocation{nullptr,L,&error_message,&traceback,0};
                (void)host_auxiliary_hooks.DispatchHost(HostAuxiliaryKind::error,reinterpret_cast<const void*>(callback.host_function),&invocation);
            }
        }
    }

    // Message handler for lua_pcall that captures the stack while it's still intact
    // This is called BEFORE the stack unwinds, so we can get a proper traceback
    static int pcall_error_handler(lua_State* L)
    {
        // Get the error message from the top of the stack
        const char* msg = lua_tostring(L, 1);
        std::string error_message = msg ? msg : "unknown error";

        // Generate traceback while stack is intact (starting at level 1 to skip this handler)
        luaL_traceback(L, L, error_message.c_str(), 1);
        std::string traceback = lua_tostring(L, -1);

        // Notify callbacks while stack is still intact
        notify_error_callbacks(L, error_message, traceback);

        // Return the traceback as the error message
        return 1;
    }

    // Helper to push the error handler and return its stack index
    static int push_error_handler(lua_State* L)
    {
        lua_pushcfunction(L, pcall_error_handler);
        return lua_gettop(L);
    }

    template <>
    auto LuaTableData<-1>::is_nil() -> bool
    {
        return lua_isnil(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::is_string() const -> bool
    {
        return lua_type(lua->checked_lua_state(), -1) == LUA_TSTRING;
    }
    template <>
    auto LuaTableData<-1>::get_string() const -> std::string_view
    {
        return lua_tostring(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::is_number() const -> bool
    {
        return lua_isnumber(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::get_number() const -> double
    {
        return lua_tonumber(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::get_float(int32_t force_index) -> float // Safe to use after a call to is_number
    {
        return static_cast<float>(lua_tonumber(lua->checked_lua_state(), force_index));
    }
    template <>
    auto LuaTableData<-1>::is_integer() const -> bool
    {
        return lua_isinteger(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::get_integer() const -> int64_t
    {
        return lua_tointeger(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::is_bool() const -> bool
    {
        return lua_isboolean(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::get_bool() const -> bool
    {
        return lua_toboolean(lua->checked_lua_state(), -1);
    }
    template <>
    auto LuaTableData<-1>::is_table() const -> bool
    {
        return lua_istable(lua->checked_lua_state(), -1);
    }

    template <>
    auto LuaTableData<-2>::is_nil() -> bool
    {
        return lua_isnil(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::is_string() const -> bool
    {
        return lua_type(lua->checked_lua_state(), -2) == LUA_TSTRING;
    }
    template <>
    auto LuaTableData<-2>::get_string() const -> std::string_view
    {
        return lua_tostring(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::is_number() const -> bool
    {
        return lua_isnumber(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::get_number() const -> double
    {
        return lua_tonumber(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::get_float(int32_t force_index) -> float // Safe to use after a call to is_number
    {
        return static_cast<float>(lua_tonumber(lua->checked_lua_state(), force_index));
    }
    template <>
    auto LuaTableData<-2>::is_integer() const -> bool
    {
        return lua_isinteger(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::get_integer() const -> int64_t
    {
        return lua_tointeger(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::is_bool() const -> bool
    {
        return lua_isboolean(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::get_bool() const -> bool
    {
        return lua_toboolean(lua->checked_lua_state(), -2);
    }
    template <>
    auto LuaTableData<-2>::is_table() const -> bool
    {
        return lua_istable(lua->checked_lua_state(), -2);
    }

    Lua::Lua(lua_State* lua_state) : m_lua_state(lua_state), m_registry(*this)
    {
        // State factories / trusted host adoption cache the proven root first.
        // Constructors never inspect G(actual) or any unpinned VM memory.
        std::lock_guard lock{lua_root_identities_mutex};
        const auto found = lua_root_identities.find(lua_state);
        if (found == lua_root_identities.end())
            throw std::runtime_error("Lua wrapper has no proven cached root identity");
        lua_object_identities.insert_or_assign(this, found->second);
    }

    Lua::~Lua()
    {
        std::lock_guard lock{lua_root_identities_mutex};
        lua_object_identities.erase(this); // pure bookkeeping; never lua_close
    }

    Lua::Registry::Registry(const Lua& lua) : m_lua_instance(lua)
    {
    }

    auto Lua::Registry::get_lua_instance() const -> const Lua&
    {
        return m_lua_instance;
    }

    auto Lua::Registry::make_ref() const -> int32_t
    {
        return luaL_ref(get_lua_instance().checked_lua_state(), LUA_REGISTRYINDEX);
    }

    auto Lua::Registry::get_string_ref(int32_t registry_index) const -> void
    {
        if (get_lua_instance().rawgeti(LUA_REGISTRYINDEX, registry_index) != LUA_TSTRING)
        {
            get_lua_instance().discard_value();
            get_lua_instance().throw_error("[Lua::Registry::get_string_ref] Ref was not string");
        }
    }

    auto Lua::Registry::get_number_ref(int32_t registry_index) const -> void
    {
        get_lua_instance().rawgeti(LUA_REGISTRYINDEX, registry_index);
        if (!get_lua_instance().is_number(-1))
        {
            get_lua_instance().discard_value();
            get_lua_instance().throw_error("[Lua::Registry::get_number_ref] Ref was not number");
        }
    }

    auto Lua::Registry::get_integer_ref(int32_t registry_index) const -> void
    {
        get_lua_instance().rawgeti(LUA_REGISTRYINDEX, registry_index);
        if (!get_lua_instance().is_integer(-1))
        {
            get_lua_instance().discard_value();
            get_lua_instance().throw_error("[Lua::Registry::get_integer_ref] Ref was not integer");
        }
    }

    auto Lua::Registry::get_bool_ref(int32_t registry_index) const -> void
    {
        get_lua_instance().rawgeti(LUA_REGISTRYINDEX, registry_index);
        if (!get_lua_instance().is_bool(-1))
        {
            get_lua_instance().discard_value();
            get_lua_instance().throw_error("[Lua::Registry::get_bool_ref] Ref was not bool");
        }
    }

    auto Lua::Registry::get_function_ref(int32_t registry_index) const -> void
    {
        if (get_lua_instance().rawgeti(LUA_REGISTRYINDEX, registry_index) != LUA_TFUNCTION)
        {
            get_lua_instance().discard_value();
            get_lua_instance().throw_error("[Lua::Registry::get_function_ref] Ref was not function");
        }
    }

    auto Lua::Registry::get_table_ref(int32_t registry_index) const -> void
    {
        if (get_lua_instance().rawgeti(LUA_REGISTRYINDEX, registry_index) != LUA_TTABLE)
        {
            get_lua_instance().discard_value();
            get_lua_instance().throw_error("[Lua::Registry::get_table_ref] Ref was not table");
        }
    }

    Lua::Table::Table(const Lua& lua_instance) : m_lua_instance(lua_instance)
    {
        // printf_s("Constructed Table\n");
    }

    auto Lua::Table::add_function_value_internal(Lua::LuaFunction function) const -> void
    {
        const auto id = register_function_slot(get_lua_instance(), function);
        publish_function(get_lua_instance(), id, m_has_userdata ? LuaFunctionType::Local : LuaFunctionType::Table);
    }

    auto Lua::Table::get_lua_instance() const -> const Lua&
    {
        return m_lua_instance;
    }

    auto Lua::Table::get_metamethods() -> Lua::MetaMethods&
    {
        return m_metamethods;
    }

    auto Lua::Table::fuse_pair() -> void
    {
        lua_rawset(get_lua_instance().checked_lua_state(), -3);
    }

    // Nop, this function is just to clarify when you make a local table
    auto Lua::Table::make_local() -> void
    {
    }

    auto Lua::Table::make_global(std::string_view table_name) const -> void
    {
        lua_setglobal(get_lua_instance().checked_lua_state(), table_name.data());
    }

    auto Lua::Table::find_value() -> void
    {
        lua_rawget(get_lua_instance().checked_lua_state(), -2);

        // Push the bottom of the stack to the top (table)
        lua_pushvalue(get_lua_instance().checked_lua_state(), 1);

        // Remove the bottom of the stack, this puts the table value at the bottom
        lua_remove(get_lua_instance().checked_lua_state(), 1);
    }

    auto Lua::Table::has_userdata() const -> bool
    {
        return m_has_userdata;
    }

    auto Lua::Table::set_has_userdata(bool new_value) -> void
    {
        m_has_userdata = new_value;
    }

    auto Lua::Table::push_field_name_and_value(std::string_view field_name) -> int32_t
    {
        // Push the field name to the top of the stack
        lua_pushstring(get_lua_instance().checked_lua_state(), field_name.data());

        // Push the value of the field to the top of the stack
        // Table is at -2 (idx param)
        // Field name is at -1
        int32_t table_value_type = lua_rawget(get_lua_instance().checked_lua_state(), -2);

        // Table is now at -3 (if: has_table_marked_for_pop == true, otherwise nothing/unrelated)
        // Table is now at -2
        // Value is now at -1

        if (m_has_table_marked_for_pop)
        {
            lua_remove(get_lua_instance().checked_lua_state(), -2);
            m_has_table_marked_for_pop = false;
        }

        return table_value_type;
    }

    auto Lua::Table::does_field_exist(std::string_view field_name) -> bool
    {
        // Push the field name to the top of the stack
        lua_pushstring(get_lua_instance().checked_lua_state(), field_name.data());

        // Push the value of the field to the top of the stack
        int32_t table_value_type = lua_rawget(get_lua_instance().checked_lua_state(), -2);

        // Pop the value of the stack as we don't need it here, we're just checking if it exists
        lua_pop(get_lua_instance().checked_lua_state(), 1);

        return table_value_type != LUA_TNIL;
    }

    auto Lua::Table::get_string_field(std::string_view field_name, bool* has_error) -> std::string_view
    {
        int pushed_type = push_field_name_and_value(field_name);
        if (pushed_type != LUA_TSTRING)
        {
            if (has_error)
            {
                *has_error = true;
            }
            else
            {
                get_lua_instance().throw_error(fmt::format("[get_string_field] Attempted to get field: '{}' but the type was '{}'",
                                                           field_name,
                                                           lua_typename(get_lua_instance().checked_lua_state(), pushed_type)));
            }
        }

        // Pop the value of the field from the stack
        std::string_view str = lua_tostring(get_lua_instance().checked_lua_state(), -1);
        lua_pop(get_lua_instance().checked_lua_state(), 1);
        return str;
    }

    auto Lua::Table::get_int_field(std::string_view field_name, bool* has_error) -> int64_t
    {
        push_field_name_and_value(field_name);
        if (!get_lua_instance().is_integer(-1))
        {
            if (has_error)
            {
                *has_error = true;
            }
            else
            {
                get_lua_instance().throw_error(fmt::format("[get_int_field] Attempted to get field: '{}' but the type was '{}'",
                                                           field_name,
                                                           lua_typename(get_lua_instance().checked_lua_state(), lua_type(get_lua_instance().checked_lua_state(), -1))));
            }
        }

        // Pop the value of the field from the stack
        int64_t integer = lua_tointeger(get_lua_instance().checked_lua_state(), -1);
        lua_pop(get_lua_instance().checked_lua_state(), 1);
        return integer;
    }

    auto Lua::Table::get_float_field(std::string_view field_name, bool* has_error) -> float
    {
        push_field_name_and_value(field_name);
        if (!get_lua_instance().is_number(-1))
        {
            if (has_error)
            {
                *has_error = true;
            }
            else
            {
                get_lua_instance().throw_error(fmt::format("[get_float_field] Attempted to get field: '{}' but the type was '{}'",
                                                           field_name,
                                                           lua_typename(get_lua_instance().checked_lua_state(), lua_type(get_lua_instance().checked_lua_state(), -1))));
            }
        }

        // Pop the value of the field from the stack
        float number = static_cast<float>(lua_tonumber(get_lua_instance().checked_lua_state(), -1));
        lua_pop(get_lua_instance().checked_lua_state(), 1);
        return number;
    }

    auto Lua::Table::get_table_field(std::string_view field_name, bool* has_error) -> Lua::Table&
    {
        int pushed_type = push_field_name_and_value(field_name);
        if (pushed_type != LUA_TTABLE)
        {
            if (has_error)
            {
                *has_error = true;
            }
            else
            {
                get_lua_instance().throw_error(fmt::format("[get_table_field] Attempted to get field: '{}' but the type was '{}'",
                                                           field_name,
                                                           lua_typename(get_lua_instance().checked_lua_state(), pushed_type)));
            }
        }

        m_has_table_marked_for_pop = true;

        // There's now another table on the top of the stack
        // Return any Lua::Table object to enable chaining of a function
        // The Lua::Table object doesn't store any table specific stuff, so we can reuse the existing one
        return *this;
    }

    auto Lua::dump_stack(const char* message) const -> void
    {
        ::RC::LuaMadeSimple::dump_stack(checked_lua_state(), message);
    }

    auto Lua::get_stack_dump(const char* message) const -> std::string
    {
        return ::RC::LuaMadeSimple::get_stack_dump(checked_lua_state(), message);
    }

    auto Lua::handle_error(const std::string& error_message) const -> const std::string
    {
        return ::RC::LuaMadeSimple::handle_error(checked_lua_state(), error_message);
    }

    auto Lua::throw_error(const std::string& error_message) const -> void
    {
        ::RC::LuaMadeSimple::throw_error(checked_lua_state(), error_message);
    }

    auto Lua::status_to_string(int status) const -> std::string_view
    {
        switch (status)
        {
        case LUA_YIELD:
            return std::string_view("LUA_YIELD");
        case LUA_ERRRUN:
            return std::string_view("LUA_ERRRUN");
        case LUA_ERRSYNTAX:
            return std::string_view("LUA_ERRSYNTAX");
        case LUA_ERRMEM:
            return std::string_view("LUA_ERRMEM");
        case LUA_ERRERR:
            return std::string_view("LUA_ERRERR");
        case LUA_ERRFILE:
            return std::string_view("LUA_ERRFILE");
        }

        return std::string_view("Unknown error");
    }

    auto Lua::resolve_status_message(int status, bool from_lua_only) const -> std::string
    {
        if (!from_lua_only) {
            std::lock_guard lock{lua_state_errors_mutex};
            const auto found = lua_state_errors.find(get_lua_state());
            if (found != lua_state_errors.end()) return found->second;
        }
        if (lua_isstring(checked_lua_state(), -1))
        {
            const char* status_message = lua_tostring(checked_lua_state(), -1);
            lua_pop(checked_lua_state(), 1);
            return fmt::format("{} => {}", status_to_string(status), status_message);
        }
        else
        {
            return fmt::format("{} => No message", status_to_string(status));
        }
    }

    auto Lua::add_metamethods(const MetaMethods& metamethods, const Table& metatable) const -> bool
    {
        auto create = [&](const char* method_name, std::optional<LuaFunction> user_callable) {
            if (user_callable.has_value())
            {
                metatable.add_pair(method_name, user_callable.value());
            }
        };

        if (metamethods.index) register_meta_delegate(*this,metamethods.index.value());
        // Hide metatable from lua scripts
        metatable.add_pair("__metatable", false);

        // Create the '__index' metamethod
        // This one will always exist but the user supplied callable might not
        // This is necessary otherwise attached variables and functions won't be found
        metatable.add_pair("__index", [](const LuaMadeSimple::Lua& lua) -> int {
            if (lua.is_userdata(-2))
            {
                // Get the global table that corresponds to this metatable
                if (lua_getiuservalue(lua.checked_lua_state(), -2, 3) == LUA_TNIL)
                {
                    lua_pop(lua.checked_lua_state(), 1);
                    luaL_traceback(lua.checked_lua_state(), lua.get_lua_state(), nullptr, 0);
                    throw std::runtime_error{"System: Global for __index doesn't exist."};
                }

                // Push the member_name to the top of the stack for lua_raw_get
                lua_pushvalue(lua.checked_lua_state(), -2);
                if (lua_rawget(lua.checked_lua_state(), -2) != LUA_TNIL)
                {
                    return 1;
                }
                else
                {
                    // Pop the table from lua_getglobal and the nil from lua_rawget
                    lua_pop(lua.checked_lua_state(), 2);

                    // Duplicate the userdata so that we can work with it without deleting the original from the stack
                    lua_pushvalue(lua.checked_lua_state(), -2);

                    // Push onto the stack, the userdata corresponding to the MetaMethodContainer for this userdata
                    lua_getiuservalue(lua.checked_lua_state(), -1, 4);
                    const auto& lua_object = lua.get_userdata<Type::MetaMethodContainer>(-1);

                    const MetaMethods& user_callables = lua_object.metamethods;

                    if (user_callables.index.has_value())
                    {
                        // Let user callback push the final value.
                        dispatch_meta_delegate(lua,user_callables.index.value());
                    }
                    else
                    {
                        // No user callback means value wasn't found.
                        // Let's return nil to stay consistent with how Lua normally works.
                        lua.set_nil();
                    }
                }
            }
            return 1;
        });

        create("__newindex", metamethods.new_index);

        bool custom_gc_method{};
        if (metamethods.gc.has_value())
        {
            create("__gc", metamethods.gc);
            custom_gc_method = true;
        }

        create("__call", metamethods.call);
        create("__eq", metamethods.equal);
        create("__len", metamethods.length);

        return custom_gc_method;
    };

    auto Lua::register_post_function_process_callback(PostFunctionProcessCallback callback) -> void
    {
        if (!callback) return;
        LuaExecutionScope execution{get_lua_state(),get_root_lua_state()};
        const auto ticket=auxiliary_ticket(this,HostAuxiliaryKind::post,reinterpret_cast<const void*>(callback));
        if (!ticket) {m_post_function_process_callbacks.emplace_back(callback);return;}
        std::lock_guard lock{native_post_callbacks_mutex};
        native_post_callbacks[this].push_back(ticket);
    }

    auto Lua::execute_file(std::string_view file_name_and_path) const -> void
    {
        LuaExecutionScope execution{get_lua_state(), get_root_lua_state()};
        // Push error handler first
        int err_handler_idx = push_error_handler(get_lua_state());

        if (int status = luaL_loadfile(checked_lua_state(), file_name_and_path.data()); status != LUA_OK)
        {
            lua_remove(checked_lua_state(), err_handler_idx); // Clean up error handler
            throw std::runtime_error{fmt::format("[Lua::execute_file] luaL_loadfile returned {}", resolve_status_message(status))};
        }

        // Use our error handler (err_handler_idx) for pcall
        if (int status = lua_pcall(checked_lua_state(), 0, LUA_MULTRET, err_handler_idx); status != LUA_OK)
        {
            // Error message already includes traceback from our handler
            std::string error_msg = lua_tostring(checked_lua_state(), -1);
            lua_pop(checked_lua_state(), 1); // Pop error message
            lua_remove(checked_lua_state(), err_handler_idx); // Clean up error handler
            throw std::runtime_error{fmt::format("[Lua::execute_file] lua_pcall returned {}: {}", resolve_status_message(status), error_msg)};
        }

        lua_remove(checked_lua_state(), err_handler_idx); // Clean up error handler
    }

    auto Lua::execute_file(std::wstring_view file_name_and_path) const -> void
    {
        std::string file_name_and_path_utf8 = to_string(file_name_and_path);
        execute_file(file_name_and_path_utf8);
    }

    auto Lua::execute_file(std::u16string_view file_name_and_path) const -> void
    {
        std::string file_name_and_path_utf8 = to_string(file_name_and_path);
        execute_file(file_name_and_path_utf8);
    }

    auto Lua::execute_string(std::string_view code) const -> void
    {
        LuaExecutionScope execution{get_lua_state(), get_root_lua_state()};
        // Push error handler first
        int err_handler_idx = push_error_handler(get_lua_state());

        if (int status = luaL_loadstring(checked_lua_state(), code.data()); status != LUA_OK)
        {
            const std::string message=lua_type(checked_lua_state(),-1)==LUA_TSTRING ? lua_tostring(checked_lua_state(),-1) : "Lua compilation failed";
            lua_settop(checked_lua_state(),err_handler_idx-1);
            throw std::runtime_error{fmt::format("[Lua::execute_string] {}: {}",status_to_string(status),message)};
        }

        // Use our error handler for pcall
        if (int status = lua_pcall(checked_lua_state(), 0, LUA_MULTRET, err_handler_idx); status != LUA_OK)
        {
            // The protected Lua return is complete. Report through ordinary C++
            // unwinding; the member throw_error path clears the caller's entire
            // prefix and can nonlocally rethrow across this scope a second time.
            const std::string message=lua_type(checked_lua_state(),-1)==LUA_TSTRING ? lua_tostring(checked_lua_state(),-1) : "Lua execution failed";
            lua_settop(checked_lua_state(),err_handler_idx-1);
            throw std::runtime_error{fmt::format("[Lua::execute_string] {}: {}",status_to_string(status),message)};
        }

        lua_remove(checked_lua_state(), err_handler_idx); // Clean up error handler
    }

    auto Lua::execute_string(std::wstring_view code) const -> void
    {
        std::string code_utf8 = to_string(code);
        execute_string(code_utf8);
    }

    auto Lua::execute_string(std::u16string_view code) const -> void
    {
        std::string code_utf8 = to_string(code);
        execute_string(code_utf8);
    }

    auto Lua::open_all_libs() const -> void
    {
        LuaExecutionScope execution{get_lua_state(), get_root_lua_state()};
        luaL_openlibs(checked_lua_state());
    }

    auto Lua::set_hook(lua_Hook func, int32_t mask, int32_t count) const -> void
    {
        auto* actual=checked_lua_state();
        const auto ticket=func ? auxiliary_ticket(this,HostAuxiliaryKind::debug,reinterpret_cast<const void*>(func)) : 0;
        // All metadata allocation precedes nonallocating raw hook publication.
        {std::lock_guard lock{debug_hooks_mutex};
         if(ticket) debug_hooks.insert_or_assign(actual,DebugHookRecord{get_root_lua_state(),ticket});
         else debug_hooks.erase(actual);}
        lua_sethook(actual,ticket ? lms_managed_debug_hook : func,mask,count);
    }

    auto Lua::registry() const -> const Registry&
    {
        return m_registry;
    }

    auto Lua::rawgeti(int32_t idx, lua_Integer n) const -> int32_t
    {
        return lua_rawgeti(checked_lua_state(), idx, n);
    }

    auto Lua::register_function(const std::string& name, const LuaFunction& function) const -> void
    {
        LuaExecutionScope execution{get_lua_state(),get_root_lua_state()};
        const auto id = register_function_slot(*this, function);
        publish_function(*this, id, LuaFunctionType::Global, name.c_str());
    }

    auto Lua::get_lua_state() const -> lua_State*
    {
        return m_lua_state;
    }

    auto Lua::checked_lua_state() const -> lua_State* {
        auto* root=get_root_lua_state();
        if(!root || !host_actual_execution_allowed(m_lua_state,root))
            throw std::runtime_error("SDK Lua access lacks a protected actual VM execution scope");
        return m_lua_state;
    }

    auto cached_root_lua_state(lua_State* actual) noexcept -> lua_State*
    {
        try {
            std::lock_guard lock{lua_root_identities_mutex};
            const auto found = lua_root_identities.find(actual);
            if (found == lua_root_identities.end()) return nullptr;
            const auto root = lua_root_identities.find(found->second.root);
            return root != lua_root_identities.end() && root->second.epoch == found->second.epoch
                ? found->second.root : nullptr;
        } catch (...) { return nullptr; }
    }
    auto Lua::get_root_lua_state() const -> lua_State*
    {
        try {
            std::lock_guard lock{lua_root_identities_mutex};
            const auto object = lua_object_identities.find(this);
            const auto actual = lua_root_identities.find(m_lua_state);
            if (object == lua_object_identities.end() || actual == lua_root_identities.end() ||
                object->second.epoch != actual->second.epoch || object->second.root != actual->second.root) return nullptr;
            const auto root = lua_root_identities.find(actual->second.root);
            return root != lua_root_identities.end() && root->second.epoch == actual->second.epoch
                ? actual->second.root : nullptr;
        } catch (...) { return nullptr; }
    }

    auto Lua::get_stack_size() const -> int32_t
    {
        return lua_gettop(checked_lua_state());
    }

    auto Lua::get_type_string() const -> std::string_view
    {
        return lua_typename(checked_lua_state(), lua_type(checked_lua_state(), 1));
    }

    auto Lua::discard_value(int32_t force_index) const -> void
    {
        lua_remove(checked_lua_state(), force_index);
    }

    auto Lua::insert_value(int32_t from_index, int32_t force_index) const -> void
    {
        lua_pushvalue(checked_lua_state(), from_index);
        lua_insert(checked_lua_state(), force_index);
    }

    auto Lua::is_nil(int32_t force_index) const -> bool
    {
        return lua_isnil(checked_lua_state(), force_index);
    }

    auto Lua::set_nil() const -> void
    {
        lua_pushnil(checked_lua_state());
    }

    auto Lua::is_string(int32_t force_index) const -> bool
    {
        // Not using lua_isstring because I don't want to return true if the type is non-string but convertible to string (example: number)
        return lua_type(checked_lua_state(), force_index) == LUA_TSTRING;
    }

    auto Lua::get_string(int32_t force_index) const -> std::string_view
    {
        std::string_view string = lua_tostring(checked_lua_state(), force_index);
        lua_remove(checked_lua_state(), force_index);
        return string;
    }

    auto Lua::set_string(const char* str, size_t len) const -> void
    {
        lua_pushlstring(checked_lua_state(), str, len);
    }

    auto Lua::set_string(const uint8_t* str, size_t len) const -> void
    {
        lua_pushlstring(checked_lua_state(), reinterpret_cast<const char*>(str), len);
    }

    auto Lua::set_string(std::string_view str) const -> void
    {
        lua_pushlstring(checked_lua_state(), str.data(), str.length());
    }

    auto Lua::is_number(int32_t force_index) const -> bool
    {
        return lua_isnumber(checked_lua_state(), force_index);
    }

    auto Lua::get_number(int32_t force_index) const -> double
    {
        double number = lua_tonumber(checked_lua_state(), force_index);
        lua_remove(checked_lua_state(), force_index);
        return number;
    }

    auto Lua::set_number(double number) const -> void
    {
        lua_pushnumber(checked_lua_state(), number);
    }

    auto Lua::get_float(int32_t force_index) const -> float
    {
        float number = static_cast<float>(lua_tonumber(checked_lua_state(), force_index));
        lua_remove(checked_lua_state(), force_index);
        return number;
    }

    auto Lua::set_float(float number) const -> void
    {
        lua_pushnumber(checked_lua_state(), static_cast<double>(number));
    }

    auto Lua::is_integer(int32_t force_index) const -> bool
    {
        return lua_isinteger(checked_lua_state(), force_index);
    }

    auto Lua::get_integer(int32_t force_index) const -> int64_t
    {
        int64_t integer = lua_tointeger(checked_lua_state(), force_index);
        lua_remove(checked_lua_state(), force_index);
        return integer;
    }

    auto Lua::set_integer(int64_t integer) const -> void
    {
        lua_pushinteger(checked_lua_state(), integer);
    }

    auto Lua::is_bool(int32_t force_index) const -> bool
    {
        return lua_isboolean(checked_lua_state(), force_index);
    }

    auto Lua::get_bool(int32_t force_index) const -> bool
    {
        int64_t integer = lua_toboolean(checked_lua_state(), force_index);
        lua_remove(checked_lua_state(), force_index);
        return integer;
    }

    auto Lua::set_bool(bool bool_value) const -> void
    {
        lua_pushboolean(checked_lua_state(), bool_value);
    }

    auto Lua::is_table(int32_t force_index) const -> bool
    {
        return lua_istable(checked_lua_state(), force_index);
    }

    auto Lua::get_table() const -> Table
    {
        return Table{*this};
    }

    auto Lua::for_each_in_table(const Lua::ForEachInTableCallable& callable) const -> void
    {
        ++m_num_tables_being_iterated;

        // If there are other values on the stack after the table then everything break
        // So lets make sure the table is at the top of the stack
        // 1: table (original table)

        // If we're already iterating at able then the table will be at index -2 instead of at index 1
        lua_pushvalue(checked_lua_state(), m_num_tables_being_iterated > 1 ? -2 : 1);
        // 1: table (original table)
        // 2: table  (original table, dupe)

        // Put nil on the top of the stack for lua_next
        lua_pushnil(checked_lua_state());
        // 1: table (original table)
        // 2: table (original table, dupe)
        // 3: nil (key)

        // Stack index -2 = table
        while (lua_next(checked_lua_state(), -2) != 0)
        {
            // 1: table (original table)
            // 2: table  (original table, dupe)
            // 3: number (key)
            // 4: table (value)

            // Put the table key at the top of the stack
            // This is to prevent lua_next from getting confused if the key is a number
            lua_pushvalue(checked_lua_state(), -2);
            // 1: table (original table)
            // 2: table  (original table, dupe)
            // 3: number (key)
            // 4: table (value)
            // 5: number (key)

            // No we have access to the table key at index -1 and the table value at -2
            // It's up to the 'callable' to fetch the key/value from the stack
            // This makes dealing with different types easier
            // TODO: Make it so you don't need to pass the 'this' pointer to both the key & value struct
            LuaTableReference table{.key = {this}, .value = {this}};
            if (callable(table))
            {
                lua_pop(checked_lua_state(), 3);
                // 1: table (original table)
                // 2: table  (original table, dupe)
                break;
            }
            else
            {
                lua_pop(checked_lua_state(), 2);
                // 1: table (original table)
                // 2: table  (original table, dupe)
                // 3: number (key)
            }
        }

        lua_pop(checked_lua_state(), 1);
        // 1: table (original table)
        if (m_num_tables_being_iterated <= 1)
        {
            lua_remove(checked_lua_state(), 1);
        }

        // 1: stack empty

        --m_num_tables_being_iterated;
    }

    auto Lua::is_function(int32_t force_index) const -> bool
    {
        return lua_isfunction(checked_lua_state(), force_index);
    }

    auto Lua::is_global_function(std::string_view global_function_name) const -> bool
    {
        // Push on to the stack the value of global_function_name
        // lua_getglobal also returns the type
        int type_of_global = lua_getglobal(checked_lua_state(), global_function_name.data());

        // Clean the stack by popping the global of the stack
        lua_pop(checked_lua_state(), 1);

        return type_of_global == LUA_TFUNCTION;
    }

    auto Lua::prepare_function_call(std::string_view global_function_name) const -> void
    {
        // Put global on the stack & verify that it's a function and if it's not then simply do some stack cleanup
        if (int status = lua_getglobal(checked_lua_state(), global_function_name.data()); status != LUA_TFUNCTION)
        {
            // Intentionally only cleaning the stack if the type wasn't TFUNCTION
            // This is because the TFUNCTION is needed on the stack later
            lua_pop(checked_lua_state(), 1);
            throw std::runtime_error{fmt::format("[Lua::prepare_function_call] lua_getglobal returned !LUA_TFUNCTION, type returned: {}",
                                                 lua_typename(checked_lua_state(), status))};
        }
    }

    auto Lua::call_function(int32_t num_params, int32_t num_return_values) const -> void
    {
        LuaExecutionScope execution{get_lua_state(), get_root_lua_state()};
        lua_State* L = get_lua_state();

        // Insert error handler below the function and its arguments
        // Stack before: [... func arg1 arg2 ... argN] (top = N items)
        // We need: [... err_handler func arg1 arg2 ... argN]

        // Get absolute index of where the function currently is
        int top_before = lua_gettop(L);
        int func_abs_idx = top_before - num_params;  // func is at this absolute position

        // Push error handler, then insert it below the function
        lua_pushcfunction(L, pcall_error_handler);
        // Now stack is: [... func arg1 ... argN err_handler]
        // Insert at func position to shift everything up
        lua_insert(L, func_abs_idx);
        // Now stack is: [... err_handler func arg1 ... argN]

        // Error handler is at func_abs_idx (absolute)
        int err_handler_abs = func_abs_idx;

        if (int status = lua_pcall(L, num_params, num_return_values, err_handler_abs); status != LUA_OK)
        {
            // Error message (with traceback) is on stack, error handler already notified callbacks
            const char* msg = lua_tostring(L, -1);
            std::string error_msg = msg ? msg : "(no error message)";
            lua_pop(L, 1);  // Pop error message
            lua_remove(L, err_handler_abs);  // Remove error handler
            // Don't use resolve_status_message here - it would pop the stack again
            throw std::runtime_error{fmt::format("[Lua::call_function] lua_pcall returned {} => {}", status_to_string(status), error_msg)};
        }

        // Remove error handler (it's below any return values)
        // Stack after pcall success: [... err_handler ret1 ret2 ... retN]
        lua_remove(L, err_handler_abs);
    }

    auto Lua::call_function(std::string_view global_function_name, int32_t num_params, int32_t num_return_values) const -> void
    {
        LuaExecutionScope execution{get_lua_state(), get_root_lua_state()};
        prepare_function_call(global_function_name);
        call_function(num_params, num_return_values);
    }

    auto Lua::prepare_new_table(int32_t preallocate_sequential_elements, int32_t preallocate_other_elements) const -> Table
    {
        if (!host_table_plan_active(*this))
            lua_createtable(checked_lua_state(), preallocate_sequential_elements, preallocate_other_elements);
        Table table{*this};
        return table;
    }

    auto Lua::prepare_new_metatable(const char* metatable_name) const -> Table
    {
        luaL_newmetatable(checked_lua_state(), metatable_name);
        Table table{*this};
        return table;
    }

    auto Lua::get_metatable(const char* metatable_name) const -> Table
    {
        luaL_getmetatable(checked_lua_state(), metatable_name);
        Table table{*this};
        return table;
    }

    auto Lua::new_thread() const -> Lua&
    {
        LuaExecutionScope execution{get_lua_state(), get_root_lua_state()};
        lua_State* new_lua_thread{};
        if (lms_execution_new_thread(get_lua_state(), &new_lua_thread) != LUA_OK ||
            !adopt_host_thread(new_lua_thread, get_root_lua_state()))
            throw std::runtime_error("Protected LuaMadeSimple coroutine creation failed");
        std::lock_guard lock{lua_instances_mutex};
        return *lua_instances.at(new_lua_thread);
    }

    auto Lua::construct_metamethods_object(const OptionalMetaMethods& metamethods, std::optional<std::string_view> metatable_name) const -> void
    {
        if (metamethods.has_value())
        {
            Type::MetaMethodContainer c{
                    metamethods->gc,
                    metamethods->index,
                    metamethods->new_index,
                    metamethods->call,
                    metamethods->equal,
                    metamethods->length,
            };
            new_metatable<Type::MetaMethodContainer>(metatable_name, std::nullopt);
            transfer_stack_object(std::move(c), metatable_name, std::nullopt, true);
        }
        else
        {
            Type::MetaMethodContainer c{};
            new_metatable<Type::MetaMethodContainer>(metatable_name, std::nullopt);
            transfer_stack_object(std::move(c), metatable_name, std::nullopt, true);
        }
    }

    auto Lua::is_userdata(int32_t force_index) const -> bool
    {
        return lua_isuserdata(checked_lua_state(), force_index);
    }

    auto RC_LMS_API handle_error(lua_State* lua_state, const std::string& error_message) -> const std::string
    {
        luaL_traceback(lua_state, lua_state, error_message.c_str(), 0);

        std::string final_message{};
        if (lua_isstring(lua_state, -1))
        {
            final_message = lua_tostring(lua_state, -1);
        }

        if (final_message.empty() || final_message == error_message + "\nstack traceback:")
        {
            final_message = fmt::format("{}\nNo traceback", error_message);
        }

        // Notify error callbacks before clearing the stack so they can inspect state
        notify_error_callbacks(lua_state, error_message, final_message);

        // Clearing the Lua stack because we're treating errors as non-fatal, but we still need to clean everything up
        lua_settop(lua_state, 0);
        return final_message;
    }

    // MSVC longjmp unwinds the caller's C++ frame. Keep this guard in a
    // completed, non-inlined frame so an error cannot run its unlock twice.
#if defined(_MSC_VER)
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    static void record_lua_error(lua_State* state, const std::string& message)
    {
        std::lock_guard lock{lua_state_errors_mutex};
        lua_state_errors.emplace(state, message);
    }

    auto throw_error(lua_State* lua_state, const std::string& error_message) -> void
    {
        auto final_message = handle_error(lua_state, error_message);

        record_lua_error(lua_state, final_message);
        // Do C++ throw if there aren't any Lua handlers, it means this function may have been called outside a protected Lua function.
        // This normally happens while setting up a callback into Lua, often via get_function_ref.
        if (!lua_state->errorJmp && !lua_state->l_G->mainthread->errorJmp)
        {
            throw std::runtime_error{final_message};
        }
        else
        {
            luaL_error(lua_state, final_message.c_str());
        }
    }

    auto new_state() -> Lua&
    {
        struct FFactoryGate {
            FFactoryGate() { LuaLock(nullptr); }
            ~FFactoryGate() { LuaUnlock(nullptr); }
        } gate; // includes root allocation, wrapper/cache publication and rollback
        auto new_lua_state = luaL_newstate();
        if (!new_lua_state) throw std::bad_alloc{};
        try {
            {
                std::lock_guard lock{lua_root_identities_mutex};
                if (next_root_epoch == std::numeric_limits<std::uint64_t>::max()) throw std::bad_alloc{};
                lua_root_identities.insert_or_assign(new_lua_state, RootIdentity{new_lua_state, next_root_epoch++});
            }
            return store_wrapper(new_lua_state, std::make_shared<Lua>(new_lua_state));
        } catch (...) {
            lua_close(new_lua_state); // unpublished factory root: host owns it exclusively
            forget_closed_host_vm(new_lua_state);
            throw;
        }
    }

    auto RC_LMS_API push_pcall_error_handler(lua_State* L) -> int
    {
        return push_error_handler(L);
    }

    // dumpstack function from: https://stackoverflow.com/questions/59091462/from-c-how-can-i-print-the-contents-of-the-lua-stack/59097940#59097940
    // it's been modified slightly
    auto dump_stack(lua_State* lua_state, const char* message) -> void
    {
        printf_s("\n\nLUA Stack dump -> START------------------------------\n%s\n", message);
        int top = lua_gettop(lua_state);
        for (int i = 1; i <= top; i++)
        {
            printf_s("%d\t%s\t", i, luaL_typename(lua_state, i));
            switch (lua_type(lua_state, i))
            {
            case LUA_TNUMBER:
                printf_s("%g", lua_tonumber(lua_state, i));
                break;
            case LUA_TSTRING:
                printf_s("%s", lua_tostring(lua_state, i));
                break;
            case LUA_TBOOLEAN:
                printf_s("%s", (lua_toboolean(lua_state, i) ? "true" : "false"));
                break;
            case LUA_TNIL:
                printf_s("nil");
                break;
            case LUA_TFUNCTION:
                printf_s("function");
                break;
            default:
                printf_s("%p", lua_topointer(lua_state, i));
                break;
            }
            printf_s("\n");
        }
        printf_s("\nLUA Stack dump -> END----------------------------\n\n");
    }

    // https://stackoverflow.com/questions/59091462/from-c-how-can-i-print-the-contents-of-the-lua-stack/59097940#59097940
    auto get_stack_dump(lua_State* lua_state, const char* message) -> std::string
    {
        auto out_message = fmt::format("\n\nLUA Stack dump -> START------------------------------\n{}\n", message);
        int top = lua_gettop(lua_state);
        for (int i = 1; i <= top; i++)
        {
            out_message.append(fmt::format("{}\t{}\t", i, luaL_typename(lua_state, i)));
            switch (lua_type(lua_state, i))
            {
            case LUA_TNUMBER:
                out_message.append(fmt::format("{}", lua_tonumber(lua_state, i)));
                break;
            case LUA_TSTRING:
                out_message.append(fmt::format("{}", lua_tostring(lua_state, i)));
                break;
            case LUA_TBOOLEAN:
                out_message.append(fmt::format("{}", (lua_toboolean(lua_state, i) ? "true" : "false")));
                break;
            case LUA_TNIL:
                out_message.append("nil");
                break;
            case LUA_TFUNCTION:
                out_message.append("function");
                break;
            default:
                out_message.append(fmt::format("{}", lua_topointer(lua_state, i)));
                break;
            }
            out_message.append("\n");
        }
        out_message.append("\nLUA Stack dump -> END----------------------------\n\n");
        return out_message;
    }

    auto Lua::Table::add_raw_function_value_internal(LuaFunctionInternal function) const -> void {
        if(!function) throw std::runtime_error("Null internal callback");
        const auto& lua=get_lua_instance();
        LuaExecutionScope execution{lua.get_lua_state(),lua.get_root_lua_state()};
        std::uint64_t ticket{};
        if(!host_auxiliary_hooks_installed.load(std::memory_order_acquire) ||
           host_auxiliary_hooks.Register(&lua,HostAuxiliaryKind::internal,
               reinterpret_cast<const void*>(function),&ticket)!=0)
            throw std::runtime_error("Host refused internal callback ownership");
        lms_internal_function_request request{ticket ? nullptr : function,nullptr,LUA_NOREF};
        if(lms_internal_function_publish(lua.get_lua_state(),&request)!=LUA_OK)
            throw std::runtime_error("Protected internal callback publication failed");
        if(!ticket) return; // exact original verified HOST function and zero upvalues
        try {
            std::lock_guard lock{internal_functions_mutex};
            internal_functions[lua.get_root_lua_state()].insert_or_assign(request.identity,InternalFunctionRecord{ticket,request.anchor});
        } catch (...) {
            lms_internal_function_discard(lua.get_lua_state(),request.anchor);
            throw;
        }
    }

    extern "C" int lms_dispatch_debug_hook(lua_State* actual,lua_Debug* debug) noexcept {
        if(!actual || !debug) return 1;
        try {
            DebugHookRecord record{};
            {std::lock_guard lock{debug_hooks_mutex};
             const auto found=debug_hooks.find(actual);if(found==debug_hooks.end()) return 1;
             record=found->second;}
            if(!record.root || cached_root_lua_state(record.root)!=record.root || !has_host_execution_scope(record.root)) return 1;
            if(!cached_root_lua_state(actual) &&
               (lms_execution_anchor_thread(actual,record.root)!=LUA_OK || !adopt_host_thread(actual,record.root))) return 1;
            if(cached_root_lua_state(actual)!=record.root) return 1;
            const Lua* wrapper{};
            {std::lock_guard lock{lua_instances_mutex};const auto found=lua_instances.find(actual);
             if(found!=lua_instances.end()) wrapper=found->second.get();}
            if(!wrapper || wrapper->get_root_lua_state()!=record.root) return 1;
            HostAuxiliaryInvocation invocation{wrapper,actual,nullptr,nullptr,0,debug};
            return host_auxiliary_hooks.Dispatch(record.ticket,&invocation)==0 ? 0 : 1;
        } catch (...) {return 1;}
    }

    extern "C" int lms_dispatch_internal_function(lua_State* actual,int* output) noexcept {
        if(!actual || !output) return 1;
        try {
            auto* root=cached_root_lua_state(actual);
            if(!root) {
                root=LuaExecutionScope::ActiveRoot();
                if(!root || lms_execution_anchor_thread(actual,root)!=LUA_OK || !adopt_host_thread(actual,root)) return 1;
            }
            if(!has_host_execution_scope(root)) return 1;
            const auto identity=lms_internal_function_identity(actual);
            std::uint64_t ticket{};int anchor=LUA_NOREF;
            {std::lock_guard lock{internal_functions_mutex};
             const auto vm=internal_functions.find(root);
             if(vm==internal_functions.end()) return 1;
             const auto found=vm->second.find(identity);
             if(found==vm->second.end()) return 1;
             ticket=found->second.ticket;anchor=found->second.anchor;}
            if(!lms_internal_function_validate(actual,anchor)) return 1;
            const Lua* wrapper{};
            {std::lock_guard lock{lua_instances_mutex};
             const auto found=lua_instances.find(actual);
             if(found!=lua_instances.end()) wrapper=found->second.get();}
            if(!wrapper || wrapper->get_root_lua_state()!=root) return 1;
            HostAuxiliaryInvocation invocation{wrapper,actual,nullptr,nullptr,0};
            const auto status=host_auxiliary_hooks.Dispatch(ticket,&invocation);
            if(status!=0) return 1;
            *output=invocation.results; return 0;
        } catch (...) {return 1;}
    }

    auto process_lua_function(lua_State* lua_state) -> int
    {
        auto* root=cached_root_lua_state(lua_state);
        if (!root) {
            root=LuaExecutionScope::ActiveRoot();
            // Lua-created coroutine: inherit the outer real VM owner/gate,
            // prove shared global identity in protected C, and anchor it before
            // publishing the SDK wrapper. No new longjmp-skippable scope here.
            if(!root || lms_execution_anchor_thread(lua_state,root)!=LUA_OK ||
                !adopt_host_thread(lua_state,root)) return 0;
        }
        if (!has_host_execution_scope(root)) return 0;
        const size_t func_id = lua_tointeger(lua_state, lua_upvalueindex(1));
        const Lua::LuaFunctionType func_type = static_cast<Lua::LuaFunctionType>(lua_tointeger(lua_state, lua_upvalueindex(2)));

        if (func_type == Lua::LuaFunctionType::Local && !lua_isuserdata(lua_state, 1))
        {
            throw_error(lua_state, fmt::format("[process_lua_function] A function requiring userdata as param #1 was called without userdata at param #1"));
        }

        Lua* wrapper{};
        {
            std::lock_guard lock{lua_instances_mutex};
            const auto found = lua_instances.find(lua_state);
            if (found != lua_instances.end()) wrapper = found->second.get();
        }
        if (!wrapper || wrapper->get_root_lua_state() != cached_root_lua_state(lua_state)) return 0;
        Lua& data_owner = *wrapper;

        std::uint64_t ticket{};
        Lua::LuaFunction host_function{};
        {
            std::lock_guard lock{lua_functions_mutex};
            if (func_id >= lua_functions.size() || !lua_functions[func_id].function.has_value())
                throw std::runtime_error("LuaMadeSimple function slot no longer exists");
            ticket = lua_functions[func_id].ticket;
            // Managed consumer pointers are read only in the host after pins.
            if (!ticket) host_function = lua_functions[func_id].function.value();
        }
        return TRY(lua_state, [&] {
            int return_value{};
            if (ticket) {
                const auto status = host_function_hooks.Dispatch(ticket, data_owner, &return_value);
                if (status != 0) throw std::runtime_error("Host refused retired/invalid LuaMadeSimple callback");
            } else if(host_function_hooks.DispatchHost(data_owner,host_function,&return_value)!=0)
                throw std::runtime_error("Host refused SDK helper");
            for (const auto& post_process_callback : data_owner.m_post_function_process_callbacks)
            {
                HostAuxiliaryInvocation invocation{&data_owner,lua_state,nullptr,nullptr,0};
                if(host_auxiliary_hooks.DispatchHost(HostAuxiliaryKind::post,reinterpret_cast<const void*>(post_process_callback),&invocation)!=0)
                    throw std::runtime_error("Host refused SDK post helper");
            }
            std::vector<std::uint64_t> post_tickets;
            {std::lock_guard lock{native_post_callbacks_mutex};
             const auto found=native_post_callbacks.find(&data_owner);
             if(found!=native_post_callbacks.end()) post_tickets=found->second;}
            for (const auto post_ticket : post_tickets) {
                HostAuxiliaryInvocation invocation{&data_owner,lua_state,nullptr,nullptr,0};
                const auto status=host_auxiliary_hooks.Dispatch(post_ticket,&invocation);
                if(status!=0) throw std::runtime_error("Host refused auxiliary post callback");
            }
            return return_value;
        });
    }
} // namespace RC::LuaMadeSimple
