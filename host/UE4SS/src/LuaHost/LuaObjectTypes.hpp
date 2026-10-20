#pragma once
#include <LuaHost/LuaObjectBackend.hpp>
#include <LuaHost/LuaObjectDescriptors.hpp>
#include <LuaMadeSimple/HostUserdataObserver.hpp>
#include <LuaMadeSimple/HostTablePlan.hpp>
#include <LuaType/LuaUObject.hpp>
#include <LuaType/LuaUFunction.hpp>
#include <LuaType/LuaUClass.hpp>
#include <LuaType/LuaUScriptStruct.hpp>
#include <LuaType/LuaUDataTable.hpp>
#include <LuaType/LuaUStruct.hpp>
#include <LuaType/LuaUEnum.hpp>
#include <LuaType/LuaUWorld.hpp>
#include <LuaType/LuaAActor.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/World.hpp>
#include <Unreal/AActor.hpp>
#include <array>
#include <new>
#include <type_traits>
#include <utility>

namespace RC::LuaHost {
/* Match UObjectBase::construct's actual SelfType=Super storage. The public
 * UObject alias adds a different derived type, which that factory does not
 * placement-construct. No byte offsets or private layout casts are invented. */
using StoredUObject=LuaType::RemoteObjectBase<Unreal::UObject,LuaType::UObjectName>;
template<class T,class Name> inline LuaObjectDescriptor remote_descriptor(LuaObjectKind kind) {
    return {kind,Name::ToString(),sizeof(T),alignof(T),std::is_polymorphic_v<T>,[](void* storage)->Unreal::UObject* {
        return static_cast<T*>(storage)->get_remote_cpp_object();
    },LuaMadeSimple::host_userdata_type<T>()};
}
inline const auto& lua_object_descriptors() {
    static const std::array<LuaObjectDescriptor,9> descriptors{{
        remote_descriptor<StoredUObject,LuaType::UObjectName>(LuaObjectKind::object),
        remote_descriptor<LuaType::UFunction,LuaType::UFunctionName>(LuaObjectKind::function),
        remote_descriptor<LuaType::UClass,LuaType::UClassName>(LuaObjectKind::klass),
        {LuaObjectKind::script_struct,LuaType::UScriptStructName::ToString(),sizeof(LuaType::UScriptStruct),alignof(LuaType::UScriptStruct),
         std::is_polymorphic_v<LuaType::UScriptStruct>,[](void* storage)->Unreal::UObject* {
             return static_cast<LuaType::UScriptStruct*>(storage)->get_local_cpp_object().script_struct;
         },LuaMadeSimple::host_userdata_type<LuaType::UScriptStruct>()},
        remote_descriptor<LuaType::UDataTable,LuaType::UDataTableName>(LuaObjectKind::data_table),
        remote_descriptor<LuaType::UStruct,LuaType::UStructName>(LuaObjectKind::structure),
        remote_descriptor<LuaType::UEnum,LuaType::UEnumName>(LuaObjectKind::enumeration),
        remote_descriptor<LuaType::UWorld,LuaType::UWorldName>(LuaObjectKind::world),
        remote_descriptor<LuaType::AActor,LuaType::AActorName>(LuaObjectKind::actor)
    }};
    return descriptors;
}
inline const LuaObjectDescriptor* lua_object_descriptor(LuaObjectKind kind) {
    auto n=static_cast<std::size_t>(kind);const auto& all=lua_object_descriptors();
    return n<all.size()?&all[n]:nullptr;
}
class LuaPreparedObject {
public:
    virtual ~LuaPreparedObject()=default;
    virtual LuaMadeSimple::Type::BaseObject& source() noexcept=0;
    virtual void construct_at(void*)=0; // C++ only, never Lua, errors unwind normally
    virtual void destroy_at(void*) noexcept=0;
};
template<class T> class LuaPreparedTyped final:public LuaPreparedObject {
    std::unique_ptr<T> source_;
public:
    explicit LuaPreparedTyped(std::unique_ptr<T> value):source_(std::move(value)){}
    LuaMadeSimple::Type::BaseObject& source() noexcept override {return *source_;}
    void construct_at(void* storage) override {new(storage) T(std::move(*source_));}
    void destroy_at(void* storage) noexcept override {static_cast<T*>(storage)->~T();}
};

/* Only this narrowly scoped host struct is a friend of the real SDK wrappers
 * and Lua. Its friendship adds no data member, virtual function or public ABI.
 * See sdk-access.patch. It obtains the REAL private enum rather than learning
 * or guessing a numeric tag through an unprotected witness allocation. */
struct LuaObjectTypeAccess {
    static lua_Integer owned_stack_tag() noexcept {
        return static_cast<lua_Integer>(LuaMadeSimple::Lua::UserdataInternalType::LuaOwnedStackObject);
    }
    template<class T,class... Args> static std::unique_ptr<LuaPreparedObject> make(Args&&... args) {
        return std::make_unique<LuaPreparedTyped<T>>(std::unique_ptr<T>(new T(std::forward<Args>(args)...)));
    }
    template<class T> static void describe_typed(const LuaMadeSimple::Lua& lua,LuaMadeSimple::Type::BaseObject& object) {
        const auto table=T::construct(lua,object);
        T::template setup_member_functions<LuaMadeSimple::Type::IsFinal::Yes>(table);
    }
    static void describe(LuaObjectKind kind,const LuaMadeSimple::Lua& lua,LuaMadeSimple::Type::BaseObject& object) {
        switch(kind) {
        case LuaObjectKind::object:describe_typed<LuaType::UObject>(lua,object);break;
        case LuaObjectKind::function:describe_typed<LuaType::UFunction>(lua,object);break;
        case LuaObjectKind::klass:describe_typed<LuaType::UClass>(lua,object);break;
        case LuaObjectKind::script_struct:describe_typed<LuaType::UScriptStruct>(lua,object);break;
        case LuaObjectKind::data_table:describe_typed<LuaType::UDataTable>(lua,object);break;
        case LuaObjectKind::structure:describe_typed<LuaType::UStruct>(lua,object);break;
        case LuaObjectKind::enumeration:describe_typed<LuaType::UEnum>(lua,object);break;
        case LuaObjectKind::world:describe_typed<LuaType::UWorld>(lua,object);break;
        case LuaObjectKind::actor:describe_typed<LuaType::AActor>(lua,object);break;
        default:throw std::runtime_error("Unsupported cold host object kind");
        }
    }
    static void describe_metamethods(const LuaMadeSimple::Lua& lua,LuaMadeSimple::Type::BaseObject& object) {
        LuaMadeSimple::Lua::Table table{lua};
        if(lua.add_metamethods(object.get_metamethods(),table))
            throw std::runtime_error("Cold host object custom GC is unsupported");
    }
    static auto finalizer(LuaObjectKind kind) noexcept -> lua_CFunction {
        switch(kind) {
        case LuaObjectKind::object:return LuaMadeSimple::Lua::automatic_userdata_finalizer<StoredUObject>();
        case LuaObjectKind::function:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::UFunction>();
        case LuaObjectKind::klass:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::UClass>();
        case LuaObjectKind::script_struct:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::UScriptStruct>();
        case LuaObjectKind::data_table:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::UDataTable>();
        case LuaObjectKind::structure:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::UStruct>();
        case LuaObjectKind::enumeration:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::UEnum>();
        case LuaObjectKind::world:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::UWorld>();
        case LuaObjectKind::actor:return LuaMadeSimple::Lua::automatic_userdata_finalizer<LuaType::AActor>();
        default:return nullptr;
        }
    }
    static LuaObjectKind select(Unreal::UObject* object) {
        // Exact auto_construct_object order from the pinned real SDK.
        if(!object)return LuaObjectKind::object;
        if(object->IsA<Unreal::UFunction>())return LuaObjectKind::function;
        if(object->IsA<Unreal::UClass>())return LuaObjectKind::klass;
        if(object->IsA<Unreal::UScriptStruct>())return LuaObjectKind::script_struct;
        if(object->IsA<Unreal::UDataTable>())return LuaObjectKind::data_table;
        if(object->IsA<Unreal::UStruct>())return LuaObjectKind::structure;
        if(object->IsA<Unreal::UEnum>())return LuaObjectKind::enumeration;
        if(object->IsA<Unreal::UWorld>())return LuaObjectKind::world;
        if(object->IsA<Unreal::AActor>())return LuaObjectKind::actor;
        return LuaObjectKind::object;
    }
    static std::unique_ptr<LuaPreparedObject> prepare(LuaObjectKind kind,Unreal::UObject* object) {
        switch(kind) {
        case LuaObjectKind::object:return make<StoredUObject>(object);
        case LuaObjectKind::function:return make<LuaType::UFunction>(nullptr,static_cast<Unreal::UFunction*>(object));
        case LuaObjectKind::klass:return make<LuaType::UClass>(static_cast<Unreal::UClass*>(object));
        case LuaObjectKind::script_struct:return make<LuaType::UScriptStruct>(LuaType::ScriptStructWrapper{static_cast<Unreal::UScriptStruct*>(object),nullptr,nullptr});
        case LuaObjectKind::data_table:return make<LuaType::UDataTable>(static_cast<Unreal::UDataTable*>(object));
        case LuaObjectKind::structure:return make<LuaType::UStruct>(static_cast<Unreal::UStruct*>(object));
        case LuaObjectKind::enumeration:return make<LuaType::UEnum>(static_cast<Unreal::UEnum*>(object));
        case LuaObjectKind::world:return make<LuaType::UWorld>(static_cast<Unreal::UWorld*>(object));
        case LuaObjectKind::actor:return make<LuaType::AActor>(static_cast<Unreal::AActor*>(object));
        }
        return {};
    }
};
}
