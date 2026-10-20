#include <LuaHost/LuaObjectBackend.hpp>
#include "LuaObjectFrame.h"
#include "LuaObjectTypes.hpp"
#include "LuaObjectColdFactory.hpp"
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include "LuaValueFrame.h"
#include <Unreal/UnrealInitializer.hpp>
#include <type_traits>

namespace RC::LuaHost {
namespace {
struct Gate {lua_State* state;explicit Gate(lua_State* s):state(s){LuaLock(state);}~Gate(){LuaUnlock(state);}};
static_assert(std::is_trivially_destructible_v<uikit_lua_object_request>);
ue4ss_lua_host_status execute(lua_State* state,uikit_lua_object_request& r) noexcept {
    return uikit_lua_object_run(state,&r)==LUA_OK?r.status:ue4ss_lua_host_status{UE4SS_LUA_HOST_INTERNAL_ERROR};
}
bool usable(const ScopeLease& scope,const LuaObjectFactoryRecord& f,const LuaObjectDescriptor& d) noexcept {
    return f.vm==scope.vm() && f.kind==d.kind && f.factory_generation && f.storage_type==d.storage_type && f.metatable_ref>0 &&
        f.members_ref>0 && f.metamethods_ref>0 && f.metatable_identity &&
        f.members_identity && f.metamethods_identity && f.destructor && f.metamethods_metatable_identity && f.metamethods_destructor &&
        (!f.private_registry_key || f.private_registry_key==lua_object_host_private_factory_key(d.kind));
}
uikit_lua_object_request request(const LuaObjectFactoryRecord& f,const LuaObjectDescriptor& d) noexcept {
    uikit_lua_object_request r{};r.size=d.size;r.alignment=d.alignment;
    r.storage_tag=LuaObjectTypeAccess::owned_stack_tag();r.polymorphic=d.polymorphic;
    r.metatable_ref=f.metatable_ref;r.members_ref=f.members_ref;r.metamethods_ref=f.metamethods_ref;
    r.metatable_identity=f.metatable_identity;r.members_identity=f.members_identity;
    r.metamethods_identity=f.metamethods_identity;r.destructor=f.destructor;
    const auto* auxiliary=lua_object_host_metamethod_descriptor();
    r.metamethods_size=auxiliary->size;r.metamethods_alignment=auxiliary->alignment;
    r.private_registry_key=f.private_registry_key;
    r.metamethods_metatable_identity=f.metamethods_metatable_identity;r.metamethods_destructor=f.metamethods_destructor;
    return r;
}
}
ue4ss_lua_host_status LuaObjectBackend::object_read(const LuaValueContext& view,
    ue4ss_lua_context_handle context,std::int32_t index,void** output) noexcept {
    if(!output)return UE4SS_LUA_HOST_INVALID_HANDLE;
    lua_State* state{};uikit_lua_object_request captured{};bool owns_capture=false;
    auto release_capture=[&]() noexcept {
        if(owns_capture && state){captured.operation=UIKIT_OBJECT_RELEASE;(void)execute(state,captured);owns_capture=false;}
    };
    try {
        auto status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK)return status;
        Gate gate{view.serialization.stack_};state=gate.state;
        status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK)return status;
        const int normalized=uikit_lua_value_normalize_index(uikit_lua_value_top(gate.state),index,0);
        if(!normalized)return UE4SS_LUA_HOST_INVALID_HANDLE;
        uikit_lua_object_request observed{};observed.operation=UIKIT_OBJECT_INSPECT;observed.index=normalized;
        status=execute(gate.state,observed);if(status!=UE4SS_LUA_HOST_OK)return status;
        status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK)return status;
        LuaObjectEmissionProof proof{};
        status=authority_.authenticate(view.scope,observed.storage,proof);
        if(status!=UE4SS_LUA_HOST_OK)return status;
        const auto* descriptor=lua_object_descriptor(proof.kind);
        if(!descriptor || proof.vm!=view.scope.vm() || proof.storage!=observed.storage ||
           proof.size!=observed.size || proof.size!=descriptor->size || !proof.emission_generation || proof.storage_type!=descriptor->storage_type)
            return UE4SS_LUA_HOST_INVALID_HANDLE;
        LuaObjectFactoryRecord factory{};status=authority_.factory(view.scope,proof.kind,factory);
        if(status==UE4SS_LUA_HOST_PENDING) {
            captured.operation=UIKIT_OBJECT_CAPTURE;captured.index=normalized;
            captured.storage=observed.storage;captured.size=descriptor->size;captured.alignment=descriptor->alignment;
            captured.storage_tag=LuaObjectTypeAccess::owned_stack_tag();owns_capture=true;
            const auto* auxiliary=lua_object_host_metamethod_descriptor();
            captured.metamethods_size=auxiliary->size;captured.metamethods_alignment=auxiliary->alignment;
            status=execute(state,captured);if(status!=UE4SS_LUA_HOST_OK){release_capture();return status;}
            status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK){release_capture();return status;}
            factory={view.scope.vm(),proof.kind,0,proof.storage_type,captured.metatable_ref,captured.members_ref,captured.metamethods_ref,
                captured.metatable_identity,captured.members_identity,captured.metamethods_identity,captured.destructor,
                captured.metamethods_metatable_identity,captured.metamethods_destructor};
            bool adopted=false;status=authority_.cache(view.scope,proof,factory,adopted);
            if(adopted)owns_capture=false;else release_capture();
            if(status!=UE4SS_LUA_HOST_OK)return status;
        }
        if(status!=UE4SS_LUA_HOST_OK)return status;
        if(!usable(view.scope,factory,*descriptor) || (proof.factory_generation && factory.factory_generation!=proof.factory_generation))
            return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto checked=request(factory,*descriptor);checked.operation=UIKIT_OBJECT_VERIFY;
        checked.index=normalized;checked.storage=observed.storage;
        status=execute(gate.state,checked);if(status!=UE4SS_LUA_HOST_OK)return status;
        status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK)return status;
        LuaObjectEmissionProof current{};status=authority_.authenticate(view.scope,observed.storage,current);
        if(status!=UE4SS_LUA_HOST_OK || current.emission_generation!=proof.emission_generation ||
           current.storage_type!=proof.storage_type || current.kind!=proof.kind ||
           current.factory_generation!=factory.factory_generation)return UE4SS_LUA_HOST_INVALID_HANDLE;
        // Only NOW is there a typed SDK access. No wrapper vtable/type cast is
        // attempted on unproved external userdata or a lightuserdata pointer.
        auto* object=descriptor->extract(observed.storage);
        if(object==LuaMadeSimple::Type::special_invalid_ptr())object=nullptr;
        status=values_.admit(view,context);if(status==UE4SS_LUA_HOST_OK)*output=object;
        return status;
    }catch(...){release_capture();return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
ue4ss_lua_host_status LuaObjectBackend::object_push(const LuaValueContext& view,
    ue4ss_lua_context_handle context,void* address) noexcept {
    lua_State* state{};int old_top=-1;std::unique_ptr<LuaPreparedObject> prepared;
    uikit_lua_object_request allocated{},captured{};bool constructed=false,recorded=false,cold=false,owns_capture=false;
    LuaObjectEmissionProof proof{};
    auto release_capture=[&]() noexcept {
        if(owns_capture && state){captured.operation=UIKIT_OBJECT_RELEASE;(void)execute(state,captured);owns_capture=false;}
    };
    auto rollback=[&]() noexcept {
        if(recorded){authority_.retract(view.scope,proof);recorded=false;}
        if(constructed && !allocated.published && prepared){prepared->destroy_at(allocated.storage);constructed=false;}
        release_capture();
        if(state && old_top>=0)uikit_lua_value_pop_to(state,old_top);
    };
    try {
        auto status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK)return status;
        if(!Unreal::IsInGameThreadRaw())return UE4SS_LUA_HOST_WRONG_THREAD;
        auto* actual=view.serialization.stack_;
        LuaMadeSimple::LuaExecutionScope execution{actual,LuaMadeSimple::cached_root_lua_state(actual)};
        Gate gate{actual};state=gate.state;old_top=uikit_lua_value_top(state);
        status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK)return status;
        auto* object=static_cast<Unreal::UObject*>(address);
        if(object==LuaMadeSimple::Type::special_invalid_ptr())return UE4SS_LUA_HOST_INVALID_HANDLE;
        const auto kind=LuaObjectTypeAccess::select(object);const auto* descriptor=lua_object_descriptor(kind);
        if(!descriptor)return UE4SS_LUA_HOST_INVALID_HANDLE;
        LuaObjectFactoryRecord factory{};status=authority_.factory(view.scope,kind,factory);
        if(status!=UE4SS_LUA_HOST_OK && status!=UE4SS_LUA_HOST_PENDING)return status;
        cold=status==UE4SS_LUA_HOST_PENDING;
        if(!cold && !usable(view.scope,factory,*descriptor))return UE4SS_LUA_HOST_PENDING;
        prepared=LuaObjectTypeAccess::prepare(kind,object);if(!prepared)return UE4SS_LUA_HOST_INTERNAL_ERROR;
        if(cold) {
            owns_capture=true;
            status=prepare_cold_object_factory(view,authority_,state,kind,*prepared,captured,factory);
            if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}
        }
        if(object!=LuaMadeSimple::Type::special_invalid_ptr())LuaType::add_to_global_unreal_objects_map(object);
        allocated=request(factory,*descriptor);allocated.operation=UIKIT_OBJECT_ALLOCATE;
        status=execute(state,allocated);if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}
        status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}
        prepared->construct_at(allocated.storage);constructed=true; // genuine SDK construction; no Lua in C++ frame
        if(!cold) {
            proof={view.scope.vm(),kind,allocated.storage,descriptor->size,factory.factory_generation,descriptor->storage_type};
            status=authority_.publish(view.scope,proof);if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}recorded=true;
        }
        allocated.operation=UIKIT_OBJECT_COMMIT;allocated.index=old_top+1;
        status=execute(state,allocated);if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}
        if(cold) {
            // Emission is observed only after real typed construction and genuine
            // metatable attachment. No consumer or witness mints provenance.
            LuaMadeSimple::notify_host_userdata_completed({state,allocated.storage,descriptor->storage_type,
                descriptor->size,descriptor->alignment,old_top+1,descriptor->name});
            status=authority_.authenticate(view.scope,allocated.storage,proof);
            if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}recorded=true;
            bool adopted=false;status=authority_.cache(view.scope,proof,factory,adopted);
            if(adopted){captured.metatable_ref=0;captured.members_ref=0;captured.metamethods_ref=0;}
            if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}
            release_capture();
        }
        status=values_.admit(view,context);if(status!=UE4SS_LUA_HOST_OK){rollback();return status;}
        return UE4SS_LUA_HOST_OK;
    }catch(...){rollback();return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
}
