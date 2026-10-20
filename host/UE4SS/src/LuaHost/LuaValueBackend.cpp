#include <LuaHost/LuaValueBackend.hpp>
#include "LuaValueFrame.h"
#include <limits>
#include <type_traits>

namespace RC::LuaHost {
namespace {
ue4ss_lua_host_status code(Status status) noexcept { return static_cast<ue4ss_lua_host_status>(status); }
struct Gate {
    lua_State* state;
    explicit Gate(lua_State* actual) : state(actual) { LuaLock(state); }
    ~Gate() { LuaUnlock(state); }
};
static_assert(std::is_trivially_destructible_v<uikit_lua_value_request>);
static_assert(sizeof(lua_Integer) == sizeof(std::int64_t));
static_assert(std::is_same_v<lua_Number, double>);
ue4ss_lua_host_status prefix(const ue4ss_lua_value_v1* value) noexcept {
    if (!value) return UE4SS_LUA_HOST_INVALID_HANDLE;
    return ue4ss_lua_host_validate_prefix(value->abi_version, value->struct_size, sizeof(*value));
}
ue4ss_lua_value_v1 empty_value() noexcept {
    ue4ss_lua_value_v1 value{};
    value.abi_version = UE4SS_UIKIT_LUA_HOST_ABI_VERSION;
    value.struct_size = sizeof(value);
    return value;
}
ue4ss_lua_host_status execute(lua_State* state, uikit_lua_value_request& request) noexcept {
    request.status = UE4SS_LUA_HOST_OK;
    const int result = uikit_lua_value_run(state, &request);
    return result == LUA_OK ? request.status : ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
}
}

LuaValueSerialization::LuaValueSerialization(Lifecycle& lifecycle, const ScopeLease& scope,
                                             lua_State* actual) noexcept
    : lifecycle_(lifecycle), scope_(scope), stack_(actual) {
    if (!actual) return;
    try {
        status_ = code(lifecycle_.validate_scope(scope_));
        if (status_ != UE4SS_LUA_HOST_OK) return;
        LuaLock(stack_);
        locked_ = true;
        status_ = code(lifecycle_.validate_scope(scope_));
        if (status_ == UE4SS_LUA_HOST_OK) frame_ = uikit_lua_value_frame_identity(stack_);
    } catch (...) { status_ = UE4SS_LUA_HOST_INTERNAL_ERROR; }
}
LuaValueSerialization::~LuaValueSerialization() { if (locked_) LuaUnlock(stack_); }

ue4ss_lua_host_status LuaValueBackend::admit(const LuaValueContext& view,
                                           ue4ss_lua_context_handle requested) const {
    // Pure lifetime/thread/owner admission happens before any Lua inspection.
    auto status = code(lifecycle_.validate_scope(view.scope));
    if (status != UE4SS_LUA_HOST_OK) return status;
    const auto actual = view.scope.handle();
    if (!requested.id || !requested.generation || requested.id != actual.id)
        return UE4SS_LUA_HOST_INVALID_HANDLE;
    if (requested.generation != actual.generation) return UE4SS_LUA_HOST_STALE_GENERATION;
    const auto& pin = view.serialization;
    if (&pin.lifecycle_ != &lifecycle_ || &pin.scope_ != &view.scope)
        return UE4SS_LUA_HOST_WRONG_OWNER;
    if (pin.status_ != UE4SS_LUA_HOST_OK) return pin.status_;
    if (!pin.locked_ || !pin.stack_ || !pin.frame_) return UE4SS_LUA_HOST_INVALID_HANDLE;
    // Inspection is serialized by the OUTER pin. A nested helper/hook frame
    // cannot silently reuse another entry's callback stack authority.
    if (uikit_lua_value_frame_identity(pin.stack_) != pin.frame_)
        return UE4SS_LUA_HOST_WRONG_OWNER;
    return UE4SS_LUA_HOST_OK;
}

ue4ss_lua_host_status LuaValueBackend::stack_top(const LuaValueContext& view,
    ue4ss_lua_context_handle context, std::int32_t* output) noexcept {
    if (!output) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        auto status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        Gate gate{view.serialization.stack_};
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        const auto top = uikit_lua_value_top(gate.state);
        status = admit(view, context);
        if (status == UE4SS_LUA_HOST_OK) *output = top;
        return status;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}

ue4ss_lua_host_status LuaValueBackend::stack_set_top(const LuaValueContext& view,
    ue4ss_lua_context_handle context, std::int32_t index) noexcept {
    lua_State* state{};
    int old_top = -1;
    try {
        auto status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        Gate gate{view.serialization.stack_};
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        state = gate.state;
        old_top = uikit_lua_value_top(state);
        const std::int64_t target = index >= 0 ? index : std::int64_t{old_top} + index + 1;
        if (target < 0 || target > uikit_lua_value_max_top()) return UE4SS_LUA_HOST_INVALID_HANDLE;
        if (target <= old_top) {
            if (!uikit_lua_value_can_pop(state, static_cast<int>(target)))
                return UE4SS_LUA_HOST_INVALID_HANDLE;
            // No allocation, metamethod, or GC occurs during this pop or pure
            // admission. Original TValue slots remain intact for rollback.
            uikit_lua_value_pop_to(state, static_cast<int>(target));
        } else {
            uikit_lua_value_request request{};
            request.operation = UIKIT_VALUE_GROW;
            request.count = static_cast<int>(target - old_top);
            status = execute(state, request);
            if (status != UE4SS_LUA_HOST_OK) return status;
        }
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) uikit_lua_value_pop_to(state, old_top);
        return status;
    } catch (...) {
        if (state && old_top >= 0) uikit_lua_value_pop_to(state, old_top);
        return UE4SS_LUA_HOST_INTERNAL_ERROR;
    }
}

ue4ss_lua_host_status LuaValueBackend::value_read(const LuaValueContext& view,
    ue4ss_lua_context_handle context, std::int32_t index, ue4ss_lua_value_v1* output) noexcept {
    const auto version = prefix(output);
    if (version != UE4SS_LUA_HOST_OK) return version;
    try {
        auto status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        Gate gate{view.serialization.stack_};
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        const int normalized = uikit_lua_value_normalize_index(uikit_lua_value_top(gate.state), index, 1);
        if (!normalized) return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto value = empty_value();
        if (normalized > 0) {
            uikit_lua_value_request request{};
            request.operation = UIKIT_VALUE_READ;
            request.index = normalized;
            request.output = &value;
            status = execute(gate.state, request);
            if (status != UE4SS_LUA_HOST_OK) return status;
        }
        status = admit(view, context);
        if (status == UE4SS_LUA_HOST_OK) *output = value;
        return status;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}

ue4ss_lua_host_status LuaValueBackend::upvalue_read(const LuaValueContext& view,
    ue4ss_lua_context_handle context, std::uint32_t ordinal, ue4ss_lua_value_v1* output) noexcept {
    const auto version = prefix(output);
    if (version != UE4SS_LUA_HOST_OK) return version;
    try {
        auto status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        Gate gate{view.serialization.stack_};
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        if (!ordinal || ordinal > view.consumer_upvalue_count || !view.first_consumer_upvalue ||
            !view.verified_trampoline) return UE4SS_LUA_HOST_INVALID_HANDLE;
        lua_CFunction function{};
        unsigned int physical_count{};
        if (!uikit_lua_value_current_closure(gate.state, &function, &physical_count) ||
            function != view.verified_trampoline) return UE4SS_LUA_HOST_WRONG_OWNER;
        const std::uint64_t end = std::uint64_t{view.first_consumer_upvalue} + view.consumer_upvalue_count - 1;
        if (end > physical_count) return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto value = empty_value();
        uikit_lua_value_request request{};
        request.operation = UIKIT_VALUE_UPVALUE_READ;
        request.index = static_cast<int>(std::uint64_t{view.first_consumer_upvalue} + ordinal - 1);
        request.output = &value;
        status = execute(gate.state, request);
        if (status != UE4SS_LUA_HOST_OK) return status;
        status = admit(view, context);
        if (status == UE4SS_LUA_HOST_OK) *output = value;
        return status;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}

ue4ss_lua_host_status LuaValueBackend::value_push(const LuaValueContext& view,
    ue4ss_lua_context_handle context, const ue4ss_lua_value_v1* input) noexcept {
    const auto version = prefix(input);
    if (version != UE4SS_LUA_HOST_OK) return version;
    lua_State* state{};
    int old_top = -1;
    try {
        auto status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        if (input->kind > UE4SS_LUA_VALUE_TABLE || input->kind == UE4SS_LUA_VALUE_FUNCTION)
            return UE4SS_LUA_HOST_INVALID_HANDLE;
        if (input->kind == UE4SS_LUA_VALUE_STRING &&
            (input->string_size > std::numeric_limits<std::size_t>::max() ||
             (!input->string_data && input->string_size))) return UE4SS_LUA_HOST_INVALID_HANDLE;
        // Freeze descriptor fields before GC can invoke a nested host operation.
        const auto value = *input;
        Gate gate{view.serialization.stack_};
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        state = gate.state;
        old_top = uikit_lua_value_top(state);
        if (old_top >= uikit_lua_value_max_top()) return UE4SS_LUA_HOST_INVALID_HANDLE;
        uikit_lua_value_request request{};
        request.operation = UIKIT_VALUE_PUSH;
        request.input = &value;
        status = execute(state, request);
        if (status != UE4SS_LUA_HOST_OK) return status;
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) uikit_lua_value_pop_to(state, old_top);
        return status;
    } catch (...) {
        if (state && old_top >= 0) uikit_lua_value_pop_to(state, old_top);
        return UE4SS_LUA_HOST_INTERNAL_ERROR;
    }
}

ue4ss_lua_host_status LuaValueBackend::table_set_field(const LuaValueContext& view,
    ue4ss_lua_context_handle context, std::int32_t index, const char* key, std::uint64_t size) noexcept {
    if ((!key && size) || size > std::numeric_limits<std::size_t>::max()) return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        auto status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        Gate gate{view.serialization.stack_};
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        const int old_top = uikit_lua_value_top(gate.state);
        const int normalized = uikit_lua_value_normalize_index(old_top, index, 0);
        if (old_top < 1 || !uikit_lua_value_is_table(gate.state, normalized) ||
            !uikit_lua_value_can_pop(gate.state, old_top - 1)) return UE4SS_LUA_HOST_INVALID_HANDLE;
        uikit_lua_value_request request{};
        request.operation = UIKIT_VALUE_TABLE_SET;
        request.index = normalized;
        request.key = key;
        request.key_size = static_cast<std::size_t>(size);
        status = execute(gate.state, request);
        if (status != UE4SS_LUA_HOST_OK) return status;
        status = admit(view, context);
        if (status == UE4SS_LUA_HOST_OK) uikit_lua_value_pop_to(gate.state, old_top - 1);
        return status;
    } catch (...) { return UE4SS_LUA_HOST_INTERNAL_ERROR; }
}

ue4ss_lua_host_status LuaValueBackend::protected_call(const LuaValueContext& view,
    ue4ss_lua_context_handle context, std::int32_t nargs, std::int32_t nresults) noexcept {
    lua_State* state{};
    int base = -1;
    try {
        auto status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        if (nargs < 0 || nresults < 0 || nresults > uikit_lua_value_max_top())
            return UE4SS_LUA_HOST_INVALID_HANDLE;
        Gate gate{view.serialization.stack_};
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) return status;
        state = gate.state;
        const int top = uikit_lua_value_top(state);
        if (nargs >= top) return UE4SS_LUA_HOST_INVALID_HANDLE;
        base = top - nargs - 1;
        if (!uikit_lua_value_can_pop(state, base) || base > uikit_lua_value_max_top() - nresults)
            return UE4SS_LUA_HOST_INVALID_HANDLE;
        uikit_lua_value_request reserve{};
        reserve.operation = UIKIT_VALUE_RESERVE;
        reserve.count = nresults;
        status = execute(state, reserve);
        if (status == UE4SS_LUA_HOST_OK) status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) {
            uikit_lua_value_pop_to(state, base);
            return status;
        }
        const int call_status = uikit_lua_value_call(state, nargs, nresults);
        if (call_status != LUA_OK) {
            uikit_lua_value_pop_to(state, base);
            return call_status == LUA_YIELD ? ue4ss_lua_host_status{UE4SS_LUA_HOST_YIELD_FORBIDDEN}
                                          : ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
        }
        status = admit(view, context);
        if (status != UE4SS_LUA_HOST_OK) uikit_lua_value_pop_to(state, base);
        return status;
    } catch (...) {
        if (state && base >= 0) uikit_lua_value_pop_to(state, base);
        return UE4SS_LUA_HOST_INTERNAL_ERROR;
    }
}
}
