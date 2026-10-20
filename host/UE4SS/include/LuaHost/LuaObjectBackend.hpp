#pragma once
#include <LuaHost/LuaValueBackend.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace RC::Unreal { class UObject; }
namespace RC::LuaHost {
enum class LuaObjectKind : std::uint32_t {
    object, function, klass, script_struct, data_table, structure, enumeration, world, actor, meta_methods
};

/* These records are HOST INTERNAL. Registry indexes are not consumer handles.
 * They must pin genuine SDK factory metadata for this VM generation. Name-only
 * lookup is insufficient: the pinned SDK names both UStruct and UClass UClass.
 * The resolver/catalogue must establish this record from actual factory output,
 * including the exact storage type and original host __gc function. */
struct LuaObjectFactoryRecord {
    VmHandle vm{};
    LuaObjectKind kind{};
    std::uint64_t factory_generation{};
    const void* storage_type{};
    int metatable_ref{};
    int members_ref{};
    int metamethods_ref{};
    const void* metatable_identity{};
    const void* members_identity{};
    const void* metamethods_identity{};
    int (*destructor)(lua_State*){};
    const void* metamethods_metatable_identity{};
    int (*metamethods_destructor)(lua_State*){};
    const void* private_registry_key{};
};
struct LuaObjectEmissionProof {
    VmHandle vm{};
    LuaObjectKind kind{};
    const void* storage{};
    std::size_t size{};
    std::uint64_t factory_generation{};
    const void* storage_type{};
    std::uint64_t emission_generation{};
};

/* Implemented by LuaObjectAuthorityRuntime. The host's authentic SDK observer
 * tracks exact userdata identity/type/lifetime. A consumer,
 * Lua metatable string, userdata's uservalues or raw pointer cannot mint proof.
 * Calls below perform native bookkeeping only, never Lua or consumer callbacks.
 * Cache refs remain pinned until the VM's authorized finalization boundary.
 * No default/dummy-success implementation is provided. */
class LuaObjectAuthority {
public:
    virtual ~LuaObjectAuthority() = default;
    virtual ue4ss_lua_host_status factory(const ScopeLease&,LuaObjectKind,LuaObjectFactoryRecord&) const noexcept = 0;
    virtual ue4ss_lua_host_status validate_factory(const ScopeLease&,LuaObjectKind,const LuaObjectFactoryRecord&) const noexcept = 0;
    virtual ue4ss_lua_host_status authenticate(const ScopeLease&,const void*,LuaObjectEmissionProof&) const noexcept = 0;
    virtual ue4ss_lua_host_status publish(const ScopeLease&,LuaObjectEmissionProof&) noexcept = 0;
    // Adopts captured VM-owned refs only when adopted=true. The caller retains
    // its POD C capture request and releases its original refs on reuse/conflict;
    // incoming may return the already-pinned factory record on reuse.
    virtual ue4ss_lua_host_status cache(const ScopeLease&,LuaObjectEmissionProof&,LuaObjectFactoryRecord& incoming,
                                      bool& adopted) noexcept = 0;
    virtual void retract(const ScopeLease&,const LuaObjectEmissionProof&) noexcept = 0;
};

class LuaObjectBackend final {
    LuaValueBackend& values_;
    LuaObjectAuthority& authority_;
public:
    LuaObjectBackend(LuaValueBackend& values,LuaObjectAuthority& authority):values_(values),authority_(authority){}
    ue4ss_lua_host_status object_read(const LuaValueContext&,ue4ss_lua_context_handle,std::int32_t,void**) noexcept;
    ue4ss_lua_host_status object_push(const LuaValueContext&,ue4ss_lua_context_handle,void*) noexcept;
};
}
