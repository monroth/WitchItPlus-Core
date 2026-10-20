#pragma once
#include <LuaHost/LuaObjectBackend.hpp>
#include <memory>

namespace RC::LuaMadeSimple {struct HostUserdataEmission;}
namespace RC::LuaHost {
/* Resolves ONLY actual SDK stacks to their cached, host-verified actual root.
 * No Lua API/state dereference, guessed owner, new scope or consumer input. */
using LuaObjectRootResolver=lua_State* (*)(lua_State* actual_sdk_stack,void* host_cookie) noexcept;
class LuaObjectAuthorityRuntime final:public LuaObjectAuthority {
    struct State;
    std::unique_ptr<State> state_;
public:
    LuaObjectAuthorityRuntime(Lifecycle&,LuaObjectRootResolver,void* host_cookie);
    ~LuaObjectAuthorityRuntime();
    LuaObjectAuthorityRuntime(const LuaObjectAuthorityRuntime&)=delete;
    LuaObjectAuthorityRuntime& operator=(const LuaObjectAuthorityRuntime&)=delete;
    // Stable host installation before any attached VM emits SDK wrappers.
    // Uninstall/destruction is allowed only after VM close and host quiescence.
    ue4ss_lua_host_status install_observer() noexcept;
    ue4ss_lua_host_status uninstall_observer() noexcept;
    ue4ss_lua_host_status attach_vm(VmHandle,lua_State* verified_actual_root) noexcept;
    // After claim_vm_finalization and all host VM entries are drained; runs
    // protected raw-ref release under global gate BEFORE actual lua_close.
    ue4ss_lua_host_status detach_vm(VmHandle,lua_State* verified_actual_root) noexcept;
    // Pure map removal AFTER actual lua_close + Lifecycle closed publication.
    ue4ss_lua_host_status forget_closed_vm(VmHandle,lua_State* formerly_verified_root) noexcept;
    ue4ss_lua_host_status factory(const ScopeLease&,LuaObjectKind,LuaObjectFactoryRecord&) const noexcept override;
    ue4ss_lua_host_status validate_factory(const ScopeLease&,LuaObjectKind,const LuaObjectFactoryRecord&) const noexcept override;
    ue4ss_lua_host_status authenticate(const ScopeLease&,const void*,LuaObjectEmissionProof&) const noexcept override;
    ue4ss_lua_host_status publish(const ScopeLease&,LuaObjectEmissionProof&) noexcept override;
    ue4ss_lua_host_status cache(const ScopeLease&,LuaObjectEmissionProof&,LuaObjectFactoryRecord&,bool&) noexcept override;
    void retract(const ScopeLease&,const LuaObjectEmissionProof&) noexcept override;
    // SDK observer entry points: pure native records/removal, NO Lua access.
    void completed(const LuaMadeSimple::HostUserdataEmission&) noexcept;
    void destroying(lua_State*,void*,const void*) noexcept;
    void finalizer(lua_State*,const void*,int (*)(lua_State*)) noexcept;
};
}
