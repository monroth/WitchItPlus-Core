#include <LuaHost/LuaObjectDescriptors.hpp>
#include <array>
namespace RC::LuaHost {
const void* lua_object_host_private_factory_key(LuaObjectKind kind) noexcept {
    static const std::array<unsigned char,static_cast<std::size_t>(LuaObjectKind::meta_methods)> keys{};
    const auto n=static_cast<std::size_t>(kind);
    return n<keys.size()?&keys[n]:nullptr;
}
}
