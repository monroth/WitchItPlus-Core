#include <LuaMadeSimple/HostTablePlan.hpp>
#include <stdexcept>
namespace RC::LuaMadeSimple {
thread_local HostTablePlan* HostTablePlan::current_{};
HostTablePlan::HostTablePlan(const Lua& lua):lua_(lua),previous_(current_){current_=this;}
HostTablePlan::~HostTablePlan(){current_=previous_;}
void HostTablePlan::store(HostTablePair pair){
    for(auto& existing:pairs)if(existing.key==pair.key){existing=std::move(pair);return;}
    pairs.push_back(std::move(pair));
}
auto host_table_plan_active(const Lua& lua) noexcept -> bool {
    return HostTablePlan::current_ && &HostTablePlan::current_->lua_==&lua;
}
auto record_host_table_pair(const Lua& lua,std::string_view key,Lua::LuaFunction fn) -> void {
    if(!host_table_plan_active(lua))throw std::runtime_error("No matching host table plan");
    HostTablePlan::current_->store({std::string{key},HostTablePair::Kind::function,fn});
}
auto record_host_table_pair(const Lua& lua,std::string_view key,Lua::LuaFunctionInternal fn) -> void {
    if(!host_table_plan_active(lua))throw std::runtime_error("No matching host table plan");
    HostTablePair pair;pair.key=key;pair.kind=HostTablePair::Kind::raw_function;pair.raw_function=fn;
    HostTablePlan::current_->store(std::move(pair));
}
auto record_host_table_pair(const Lua& lua,std::string_view key,bool value) -> void {
    if(!host_table_plan_active(lua))throw std::runtime_error("No matching host table plan");
    HostTablePair pair;pair.key=key;pair.kind=HostTablePair::Kind::boolean;pair.boolean=value;
    HostTablePlan::current_->store(std::move(pair));
}
}
