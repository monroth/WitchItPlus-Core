#pragma once
#include <LuaMadeSimple/Common.hpp>
#include <cstddef>
#include <cstdint>
struct lua_State;
namespace RC::LuaMadeSimple {
class Lua;
// Exact compiler-created operations, never guessed userdata/shared_ptr layout.
// Execute authenticates BOTH code addresses before admitting Native emission.
struct HostOwnedUserdataConstruction {
    const Lua* lua{};
    const void* type{};
    std::size_t size{}, alignment{};
    void* payload{};
    void (*construct)(void*){};
    void (*destroy)(void*){};
    bool shared{}, custom_gc{};
    int (*automatic_gc)(lua_State*){};
};
enum class HostOwnedUserdataPhase : std::uint32_t { constructing=1, constructed=2, constructor_threw=3 };
struct HostOwnedUserdataHooks {
    std::uint32_t (*Execute)(const HostOwnedUserdataConstruction*,bool*) noexcept{};
    std::uint32_t (*Allocate)(lua_State*,const void*,std::size_t,int,void**,bool*) noexcept{};
    std::uint32_t (*Arm)(lua_State*,void*) noexcept{};
    std::uint32_t (*Finalizer)(lua_State*,const void*,int (*)(lua_State*),int (**)(lua_State*)) noexcept{};
    bool (*Readable)(lua_State*,void*,const void*) noexcept{};
    void (*Phase)(lua_State*,void*,HostOwnedUserdataPhase) noexcept{};
};
RC_LMS_API bool install_host_owned_userdata_hooks(const HostOwnedUserdataHooks&) noexcept;
RC_LMS_API std::uint32_t execute_host_owned_userdata(const HostOwnedUserdataConstruction&,bool*) noexcept;
RC_LMS_API void* allocate_host_owned_userdata(lua_State*,const void*,std::size_t,int,bool*);
RC_LMS_API int (*publish_host_owned_userdata_finalizer(lua_State*,const void*,int (*)(lua_State*)))(lua_State*);
RC_LMS_API bool host_owned_userdata_readable(lua_State*,void*,const void*) noexcept;
RC_LMS_API void arm_host_owned_userdata(lua_State*,void*);
RC_LMS_API void phase_host_owned_userdata(lua_State*,void*,HostOwnedUserdataPhase) noexcept;
}
