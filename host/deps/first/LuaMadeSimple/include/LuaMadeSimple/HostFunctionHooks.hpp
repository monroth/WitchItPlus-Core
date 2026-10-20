#pragma once
#include <cstdint>
#include <LuaMadeSimple/Common.hpp>
namespace RC::LuaMadeSimple {
class Lua;
// Installed once by the owning host before loading consumer DLLs. A zero
// ticket explicitly denotes a verified host function; consumers cannot select
// that classification. Nonzero tickets dispatch through host-owned metadata.
struct HostFunctionHooks {
    std::uint32_t (*Register)(const Lua&, int (*)(const Lua&), std::uint64_t*){};
    std::uint32_t (*Dispatch)(std::uint64_t, const Lua&, int*){};
    std::uint32_t (*DispatchHost)(const Lua&,int (*)(const Lua&),int*){};
};
RC_LMS_API bool install_host_function_hooks(const HostFunctionHooks&) noexcept;
}
