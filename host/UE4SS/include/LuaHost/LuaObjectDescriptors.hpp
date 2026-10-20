#pragma once
#include <LuaHost/LuaObjectBackend.hpp>
namespace RC::LuaHost {
struct LuaObjectDescriptor {
    LuaObjectKind kind;
    const char* name;
    std::size_t size,alignment;
    bool polymorphic;
    RC::Unreal::UObject* (*extract)(void*);
    const void* storage_type;
};
/* Definitions compile against the real SDK in LuaObjectDescriptors.cpp.
 * Separating declarations lets the pure native catalogue compile independently
 * without replacing SDK types, sizes, constructors or wrapper functions. */
const LuaObjectDescriptor* lua_object_host_descriptor(LuaObjectKind);
const void* lua_object_host_private_factory_key(LuaObjectKind) noexcept;
const LuaObjectDescriptor* lua_object_host_descriptor_for(const void* actual_storage_type);
const LuaObjectDescriptor* lua_object_host_metamethod_descriptor();
}
