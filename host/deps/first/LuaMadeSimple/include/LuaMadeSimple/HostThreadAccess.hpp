#pragma once
#include <LuaMadeSimple/Common.hpp>
#include <cstdint>
struct lua_State;
namespace RC::LuaMadeSimple {
// Trusted host only. Creation is already protected by the actual VM scope +
// LuaRaw gate. The host verified thread/root G identity before calling this.
RC_LMS_API bool adopt_host_thread(lua_State* actual, lua_State* root) noexcept;
RC_LMS_API bool adopt_host_thread(lua_State* actual, lua_State* root, std::uint64_t* receipt) noexcept;
// Pure bookkeeping after the actual root lua_close. No Lua inspection.
RC_LMS_API void forget_closed_host_thread(lua_State* actual) noexcept;
RC_LMS_API void forget_closed_host_thread(lua_State* actual, std::uint64_t expected_epoch) noexcept;
}

namespace RC::LuaMadeSimple {
// After actual root lua_close; invalidates all cached SDK stack generations.
RC_LMS_API void forget_closed_host_vm(lua_State* root) noexcept;
}
