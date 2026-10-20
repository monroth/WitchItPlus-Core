#include "LuaObjectColdFactory.hpp"
#include "LuaValueFrame.h"
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <climits>
#include <new>
#include <vector>
#include <type_traits>
static_assert(std::is_nothrow_copy_constructible_v<RC::LuaMadeSimple::Type::MetaMethodContainer>);
namespace RC::LuaHost {
namespace {
ue4ss_lua_host_status execute(lua_State* state,uikit_lua_object_request& r) noexcept {
    return uikit_lua_object_run(state,&r)==LUA_OK?r.status:ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
}
LuaObjectFactoryRecord record(const ScopeLease& scope,LuaObjectKind kind,const uikit_lua_object_request& r) {
    return {scope.vm(),kind,0,lua_object_descriptor(kind)->storage_type,r.metatable_ref,r.members_ref,r.metamethods_ref,
        r.metatable_identity,r.members_identity,r.metamethods_identity,r.destructor,r.metamethods_metatable_identity,r.metamethods_destructor,r.private_registry_key};
}
bool same_methods(const LuaMadeSimple::Lua::MetaMethods& a,const LuaMadeSimple::Lua::MetaMethods& b) noexcept {
    return a.gc==b.gc && a.index==b.index && a.new_index==b.new_index && a.call==b.call && a.equal==b.equal && a.length==b.length;
}
void pairs(const LuaMadeSimple::Lua& lua,const std::vector<LuaMadeSimple::HostTablePair>& plan,
    std::vector<uikit_lua_object_pair>& output,std::vector<LuaMadeSimple::HostTableFunction>& functions) {
    using Pair=LuaMadeSimple::HostTablePair;
    output.reserve(plan.size());functions.reserve(functions.size()+plan.size());
    for(const auto& entry:plan) {
        uikit_lua_object_pair value{};value.key=entry.key.data();value.key_size=entry.key.size();
        if(entry.kind==Pair::Kind::function) {
            const auto fn=LuaMadeSimple::prepare_host_table_function(lua,entry.function);functions.push_back(fn);
            value.kind=UIKIT_OBJECT_PAIR_FUNCTION;value.id=fn.id;value.function_kind=fn.kind;value.function=fn.trampoline;
        } else if(entry.kind==Pair::Kind::raw_function){value.kind=UIKIT_OBJECT_PAIR_RAW_FUNCTION;value.function=entry.raw_function;}
        else {value.kind=UIKIT_OBJECT_PAIR_BOOLEAN;value.boolean=entry.boolean;}
        output.push_back(value);
    }
}
}
ue4ss_lua_host_status prepare_cold_object_factory(const LuaValueContext& view,LuaObjectAuthority& authority,lua_State* state,
    LuaObjectKind kind,LuaPreparedObject& prepared,uikit_lua_object_request& captured,LuaObjectFactoryRecord& output) noexcept {
    const int top=uikit_lua_value_top(state);bool auxiliary_constructed=false,keep_functions=false;
    std::vector<LuaMadeSimple::HostTableFunction> functions;
    auto cleanup=[&]() noexcept {
        if(auxiliary_constructed && !captured.auxiliary_published)
            static_cast<LuaMadeSimple::Type::MetaMethodContainer*>(captured.auxiliary_storage)->~MetaMethodContainer();
        if(!keep_functions)for(auto fn:functions)LuaMadeSimple::discard_host_table_function(fn);
        uikit_lua_value_pop_to(state,top);
    };
    try {
        if(!LuaMadeSimple::has_host_execution_scope(LuaMadeSimple::cached_root_lua_state(state)))return UE4SS_LUA_HOST_INVALID_HANDLE;
        LuaMadeSimple::Lua lua{state}; // actual known SDK stack; pure constructor, no G/root substitution
        LuaMadeSimple::HostTablePlan members{lua};LuaObjectTypeAccess::describe(kind,lua,prepared.source());
        LuaMadeSimple::HostTablePlan metatable{lua};LuaObjectTypeAccess::describe_metamethods(lua,prepared.source());
        const auto* descriptor=lua_object_descriptor(kind);const auto* auxiliary=lua_object_host_metamethod_descriptor();
        const auto* private_key=lua_object_host_private_factory_key(kind);
        if(!private_key){cleanup();return UE4SS_LUA_HOST_INVALID_HANDLE;}
        auto initialize=[&](const void* key) {
            captured={};captured.factory_name=descriptor->name;captured.private_registry_key=key;
            captured.storage_tag=LuaObjectTypeAccess::owned_stack_tag();
            captured.metamethods_size=auxiliary->size;captured.metamethods_alignment=auxiliary->alignment;
        };
        auto validate=[&]() {
            output=record(view.scope,kind,captured);
            auto status=authority.validate_factory(view.scope,kind,output);
            if(status==UE4SS_LUA_HOST_OK && !same_methods(prepared.source().get_metamethods(),
                static_cast<LuaMadeSimple::Type::MetaMethodContainer*>(const_cast<void*>(captured.metamethods_identity))->metamethods))
                status=UE4SS_LUA_HOST_INVALID_HANDLE;
            return status;
        };
        initialize(private_key);captured.operation=UIKIT_OBJECT_FACTORY_LOOKUP;
        auto status=execute(state,captured);if(status!=UE4SS_LUA_HOST_OK){cleanup();return status;}
        if(captured.factory_found){
            status=validate();
            if(status==UE4SS_LUA_HOST_OK){captured.operation=UIKIT_OBJECT_FACTORY_NAMED_ALIAS;status=execute(state,captured);}
            cleanup();return status;
        }
        // Named SDK entries are optional adoption candidates. An incompatible
        // entry stays intact while this exact kind gets private host metadata.
        initialize(nullptr);captured.operation=UIKIT_OBJECT_FACTORY_LOOKUP;
        status=execute(state,captured);
        if(status==UE4SS_LUA_HOST_OK && captured.factory_found)status=validate();
        if(status==UE4SS_LUA_HOST_OK && captured.factory_found) {
            captured.private_registry_key=private_key;captured.operation=UIKIT_OBJECT_FACTORY_ALIAS;
            status=execute(state,captured);
            if(status==UE4SS_LUA_HOST_OK)output=record(view.scope,kind,captured);
            cleanup();return status;
        }
        if(status!=UE4SS_LUA_HOST_OK && status!=UE4SS_LUA_HOST_PENDING && status!=UE4SS_LUA_HOST_INVALID_HANDLE){cleanup();return status;}
        captured.operation=UIKIT_OBJECT_RELEASE;
        status=execute(state,captured);if(status!=UE4SS_LUA_HOST_OK){cleanup();return status;}
        initialize(private_key);
        captured.destructor=LuaObjectTypeAccess::finalizer(kind);
        captured.metamethods_destructor=LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaMadeSimple::Type::MetaMethodContainer>();
        metatable.pairs.push_back({"__gc",LuaMadeSimple::HostTablePair::Kind::raw_function,nullptr,captured.destructor});
        std::vector<uikit_lua_object_pair> member_pairs,metatable_pairs;
        pairs(lua,members.pairs,member_pairs,functions);pairs(lua,metatable.pairs,metatable_pairs,functions);
        if(member_pairs.size()>INT_MAX || metatable_pairs.size()>INT_MAX-1){cleanup();return UE4SS_LUA_HOST_INTERNAL_ERROR;}
        captured.member_pairs=member_pairs.data();captured.member_count=member_pairs.size();
        captured.metatable_pairs=metatable_pairs.data();captured.metatable_count=metatable_pairs.size();
        captured.operation=UIKIT_OBJECT_FACTORY_BUILD;status=execute(state,captured);
        if(status!=UE4SS_LUA_HOST_OK){cleanup();return status;}
        new(captured.auxiliary_storage) LuaMadeSimple::Type::MetaMethodContainer{prepared.source().get_metamethods()};
        auxiliary_constructed=true;
        LuaMadeSimple::notify_host_userdata_finalizer(state,descriptor->storage_type,captured.destructor);
        LuaMadeSimple::notify_host_userdata_finalizer(state,auxiliary->storage_type,captured.metamethods_destructor);
        captured.operation=UIKIT_OBJECT_FACTORY_ATTACH;captured.index=top+1;status=execute(state,captured);
        if(status!=UE4SS_LUA_HOST_OK){cleanup();return status;}
        LuaMadeSimple::notify_host_userdata_completed({state,captured.auxiliary_storage,auxiliary->storage_type,
            auxiliary->size,auxiliary->alignment,top+1,"MetaMethodContainer"});
        output=record(view.scope,kind,captured);status=authority.validate_factory(view.scope,kind,output);
        if(status!=UE4SS_LUA_HOST_OK){cleanup();return status;}
        captured.operation=UIKIT_OBJECT_FACTORY_COMMIT;status=execute(state,captured);
        keep_functions=captured.factory_published || captured.named_factory_published; // callbacks retained only after real registry publication
        cleanup();return status;
    }catch(...){cleanup();return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
}
