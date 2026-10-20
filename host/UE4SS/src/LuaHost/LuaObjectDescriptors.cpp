#include "LuaObjectTypes.hpp"
namespace RC::LuaHost {
const LuaObjectDescriptor* lua_object_host_descriptor(LuaObjectKind kind){return lua_object_descriptor(kind);}
const LuaObjectDescriptor* lua_object_host_descriptor_for(const void* type){
    for(const auto& descriptor:lua_object_descriptors())if(descriptor.storage_type==type)return &descriptor;
    auto* auxiliary=lua_object_host_metamethod_descriptor();if(auxiliary->storage_type==type)return auxiliary;
    return nullptr;
}
const LuaObjectDescriptor* lua_object_host_metamethod_descriptor(){
    using T=LuaMadeSimple::Type::MetaMethodContainer;
    static const LuaObjectDescriptor descriptor{LuaObjectKind::meta_methods,"MetaMethodContainer",sizeof(T),alignof(T),
        std::is_polymorphic_v<T>,nullptr,LuaMadeSimple::host_userdata_type<T>()};
    return &descriptor;
}
}
