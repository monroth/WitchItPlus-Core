#pragma once
#include <LuaMadeSimple/Common.hpp>
#include <cstdint>
#include <string>
struct lua_State;
struct lua_Debug;
namespace RC::LuaMadeSimple {
class Lua;
enum class HostAuxiliaryKind : std::uint32_t { post = 1, error = 2, internal = 3, debug = 4 };
struct HostAuxiliaryInvocation {
    const Lua* lua{};
    lua_State* actual{};
    const std::string* message{};
    const std::string* traceback{};
    int results{};
    lua_Debug* debug{};
};
struct HostAuxiliaryHooks {
    std::uint32_t (*Register)(const Lua*, HostAuxiliaryKind, const void*, std::uint64_t*) noexcept{};
    std::uint32_t (*Dispatch)(std::uint64_t, HostAuxiliaryInvocation*) noexcept{};
    std::uint32_t (*DispatchHost)(HostAuxiliaryKind,const void*,HostAuxiliaryInvocation*) noexcept{};
};
RC_LMS_API bool install_host_auxiliary_hooks(const HostAuxiliaryHooks&) noexcept;
}
