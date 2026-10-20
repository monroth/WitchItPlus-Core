#pragma once
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <string>
#include <vector>
namespace RC::LuaMadeSimple {
struct HostTablePair {
    enum class Kind {function, raw_function, boolean};
    std::string key;
    Kind kind{};
    Lua::LuaFunction function{};
    Lua::LuaFunctionInternal raw_function{};
    bool boolean{};
};
struct HostTableFunction {lua_Integer id{},kind{};lua_CFunction trampoline{};};
// These register actual SDK callbacks through the existing HostFunctionHooks.
// They perform native bookkeeping only and are called above protected C frames.
auto prepare_host_table_function(const Lua&,Lua::LuaFunction) -> HostTableFunction;
auto discard_host_table_function(HostTableFunction) noexcept -> void;
class HostTablePlan final {
    friend auto host_table_plan_active(const Lua&) noexcept -> bool;
    friend auto record_host_table_pair(const Lua&,std::string_view,Lua::LuaFunction) -> void;
    friend auto record_host_table_pair(const Lua&,std::string_view,Lua::LuaFunctionInternal) -> void;
    friend auto record_host_table_pair(const Lua&,std::string_view,bool) -> void;
    const Lua& lua_;
    HostTablePlan* previous_{};
    static thread_local HostTablePlan* current_;
    void store(HostTablePair);
public:
    explicit HostTablePlan(const Lua&);
    ~HostTablePlan();
    HostTablePlan(const HostTablePlan&)=delete;
    HostTablePlan& operator=(const HostTablePlan&)=delete;
    std::vector<HostTablePair> pairs;
};
}
