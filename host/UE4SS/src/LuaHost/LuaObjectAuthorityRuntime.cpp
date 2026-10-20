#include <LuaHost/LuaObjectAuthorityRuntime.hpp>
#include <LuaHost/LuaObjectDescriptors.hpp>
#include <LuaMadeSimple/HostUserdataObserver.hpp>
#include "LuaObjectFrame.h"
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace RC::LuaHost {
namespace {
std::atomic<LuaObjectAuthorityRuntime*> runtime{};
struct Gate {lua_State* state;explicit Gate(lua_State* s):state(s){LuaLock(state);}~Gate(){LuaUnlock(state);}};
const LuaMadeSimple::HostUserdataObserver observer{
    [](const LuaMadeSimple::HostUserdataEmission& e) noexcept {if(auto* r=runtime.load(std::memory_order_acquire))r->completed(e);},
    [](lua_State* s,void* p,const void* t) noexcept {if(auto* r=runtime.load(std::memory_order_acquire))r->destroying(s,p,t);},
    [](lua_State* s,const void* t,int (*f)(lua_State*)) noexcept {if(auto* r=runtime.load(std::memory_order_acquire))r->finalizer(s,t,f);}
};
std::size_t number(LuaObjectKind kind) noexcept {return static_cast<std::size_t>(kind);}
const LuaObjectDescriptor* descriptor_for(const void* type) {
    return lua_object_host_descriptor_for(type);
}
bool same_emission(const LuaObjectEmissionProof& a,const LuaObjectEmissionProof& b) noexcept {
    return a.vm==b.vm && a.kind==b.kind && a.storage==b.storage && a.size==b.size &&
        a.storage_type==b.storage_type && a.emission_generation && a.emission_generation==b.emission_generation;
}
bool same_metadata(const LuaObjectFactoryRecord& a,const LuaObjectFactoryRecord& b) noexcept {
    return a.vm==b.vm && a.kind==b.kind && a.storage_type==b.storage_type &&
        a.metatable_identity==b.metatable_identity && a.members_identity==b.members_identity &&
        a.metamethods_identity==b.metamethods_identity && a.destructor==b.destructor &&
        a.metamethods_metatable_identity==b.metamethods_metatable_identity && a.metamethods_destructor==b.metamethods_destructor &&
        a.private_registry_key==b.private_registry_key;
}
}
struct LuaObjectAuthorityRuntime::State {
    struct Vm {
        VmHandle handle{};lua_State* root{};bool closing{};
        std::unordered_map<const void*,LuaObjectEmissionProof> emitted;
        std::unordered_map<int (*)(lua_State*),const void*> finalizers;
        std::array<std::optional<LuaObjectFactoryRecord>,9> factories;
        std::array<std::optional<LuaObjectFactoryRecord>,9> pending_cleanup;
    };
    Lifecycle& lifecycle;LuaObjectRootResolver resolve;void* cookie;
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t,std::unique_ptr<Vm>> vms;
    std::unordered_map<lua_State*,VmHandle> roots;
    std::uint64_t sequence{};
    State(Lifecycle& l,LuaObjectRootResolver r,void* c):lifecycle(l),resolve(r),cookie(c){}
    Vm* find(VmHandle h) const noexcept {
        auto i=vms.find(h.id);return i!=vms.end() && i->second->handle==h?i->second.get():nullptr;
    }
    Vm* find_root(lua_State* root) const noexcept {
        auto i=roots.find(root);return i==roots.end()?nullptr:find(i->second);
    }
    std::uint64_t next() noexcept {return sequence==UINT64_MAX?0:++sequence;}
};
LuaObjectAuthorityRuntime::LuaObjectAuthorityRuntime(Lifecycle& l,LuaObjectRootResolver r,void* c):state_(std::make_unique<State>(l,r,c)){}
LuaObjectAuthorityRuntime::~LuaObjectAuthorityRuntime(){
    // Host shutdown is required to be quiescent. Defensively disconnect future
    // SDK observations even on an incorrectly ordered teardown; do not leave
    // the global observer pointing at a destroyed catalogue. This performs no
    // Lua cleanup and does not authorize destroying a runtime with live entries.
    auto* expected=this;
    if(runtime.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel))
        LuaMadeSimple::set_host_userdata_observer(nullptr);
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::install_observer() noexcept {
    if(!state_->resolve)return UE4SS_LUA_HOST_INVALID_HANDLE;
    LuaObjectAuthorityRuntime* expected{};
    if(!runtime.compare_exchange_strong(expected,this,std::memory_order_acq_rel) && expected!=this)return UE4SS_LUA_HOST_BUSY;
    LuaMadeSimple::set_host_userdata_observer(&observer);return UE4SS_LUA_HOST_OK;
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::uninstall_observer() noexcept {
    try {
        {std::lock_guard lock(state_->mutex);if(!state_->vms.empty())return UE4SS_LUA_HOST_BUSY;}
        auto* expected=this;
        if(runtime.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel))LuaMadeSimple::set_host_userdata_observer(nullptr);
        return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::attach_vm(VmHandle handle,lua_State* root) noexcept {
    if(!root || !handle.id || !handle.generation)return UE4SS_LUA_HOST_INVALID_HANDLE;
    try {
        auto live=state_->lifecycle.vm_state(handle);
        if(live.status!=Status::ok)return static_cast<ue4ss_lua_host_status>(live.status);
        if(live.value!=VmState::opening && live.value!=VmState::open)return UE4SS_LUA_HOST_RETIRED;
        auto row=std::make_unique<State::Vm>();row->handle=handle;row->root=root;
        std::lock_guard lock(state_->mutex);
        if(auto* existing=state_->find(handle))return existing->root==root && !existing->closing?UE4SS_LUA_HOST_OK:UE4SS_LUA_HOST_WRONG_OWNER;
        if(state_->vms.contains(handle.id) || state_->roots.contains(root))return UE4SS_LUA_HOST_WRONG_OWNER;
        state_->vms.emplace(handle.id,std::move(row));
        try{state_->roots.emplace(root,handle);}catch(...){state_->vms.erase(handle.id);throw;}
        return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
void LuaObjectAuthorityRuntime::completed(const LuaMadeSimple::HostUserdataEmission& emission) noexcept {
    try {
        const auto* descriptor=descriptor_for(emission.storage_type);
        if(!descriptor || !emission.storage || emission.size!=descriptor->size || emission.alignment!=descriptor->alignment)return;
        auto* root=state_->resolve(emission.actual_stack,state_->cookie);if(!root)return;
        std::lock_guard lock(state_->mutex);auto* vm=state_->find_root(root);if(!vm || vm->closing)return;
        const auto generation=state_->next();if(!generation){vm->emitted.erase(emission.storage);return;}
        LuaObjectEmissionProof proof{vm->handle,descriptor->kind,emission.storage,emission.size,0,descriptor->storage_type,generation};
        vm->emitted.insert_or_assign(emission.storage,proof);
    }catch(...){} // Missing real observation means later admission rejects.
}
void LuaObjectAuthorityRuntime::destroying(lua_State* actual,void* storage,const void* type) noexcept {
    (void)type;
    try {
        auto* root=state_->resolve(actual,state_->cookie);if(!root)return;
        std::lock_guard lock(state_->mutex);auto* vm=state_->find_root(root);if(!vm)return;
        auto i=vm->emitted.find(storage);
        // Revoke even if the SDK's conflicting metatable invokes a different
        // typed finalizer. Any destruction of these bytes ends this emission.
        if(i!=vm->emitted.end())vm->emitted.erase(i);
    }catch(...){}
}
void LuaObjectAuthorityRuntime::finalizer(lua_State* actual,const void* type,int (*function)(lua_State*)) noexcept {
    try {
        if(!descriptor_for(type) || !function)return;
        auto* root=state_->resolve(actual,state_->cookie);if(!root)return;
        std::lock_guard lock(state_->mutex);auto* vm=state_->find_root(root);if(!vm || vm->closing)return;
        auto [i,created]=vm->finalizers.emplace(function,type);
        if(!created && i->second!=type)i->second=nullptr; // conflicting type claims never authorize a cache
    }catch(...){}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::factory(const ScopeLease& scope,LuaObjectKind kind,LuaObjectFactoryRecord& output) const noexcept {
    try {
        std::lock_guard lock(state_->mutex);auto* vm=state_->find(scope.vm());if(!vm)return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(vm->closing)return UE4SS_LUA_HOST_RETIRED;
        auto n=number(kind);if(n>=vm->factories.size())return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(!vm->factories[n])return UE4SS_LUA_HOST_PENDING;
        output=*vm->factories[n];return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::validate_factory(const ScopeLease& scope,LuaObjectKind kind,const LuaObjectFactoryRecord& incoming) const noexcept {
    try {
        std::lock_guard lock(state_->mutex);auto* vm=state_->find(scope.vm());if(!vm)return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(vm->closing)return UE4SS_LUA_HOST_RETIRED;
        const auto* descriptor=lua_object_host_descriptor(kind);auto n=number(kind);
        if(!descriptor || n>=vm->factories.size() || incoming.vm!=scope.vm() || incoming.kind!=kind ||
           incoming.storage_type!=descriptor->storage_type || incoming.metatable_ref<=0 || incoming.members_ref<=0 ||
           incoming.metamethods_ref<=0 || !incoming.metatable_identity || !incoming.members_identity ||
           !incoming.metamethods_identity || !incoming.metamethods_metatable_identity ||
           (incoming.private_registry_key && incoming.private_registry_key!=lua_object_host_private_factory_key(kind)))return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto destructor=vm->finalizers.find(incoming.destructor);
        if(destructor==vm->finalizers.end() || destructor->second!=descriptor->storage_type)return UE4SS_LUA_HOST_PENDING;
        const auto* auxiliary=lua_object_host_metamethod_descriptor();auto methods=vm->emitted.find(incoming.metamethods_identity);
        if(methods==vm->emitted.end() || methods->second.kind!=LuaObjectKind::meta_methods ||
           methods->second.storage_type!=auxiliary->storage_type || methods->second.size!=auxiliary->size)return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto methods_destructor=vm->finalizers.find(incoming.metamethods_destructor);
        if(methods_destructor==vm->finalizers.end() || methods_destructor->second!=auxiliary->storage_type)return UE4SS_LUA_HOST_PENDING;
        for(const auto& cached:vm->factories)
            if(cached && cached->metatable_identity==incoming.metatable_identity && cached->storage_type!=incoming.storage_type)
                return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(vm->factories[n] && !same_metadata(*vm->factories[n],incoming))return UE4SS_LUA_HOST_INVALID_HANDLE;
        return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::authenticate(const ScopeLease& scope,const void* storage,LuaObjectEmissionProof& output) const noexcept {
    try {
        std::lock_guard lock(state_->mutex);auto* vm=state_->find(scope.vm());if(!vm)return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(vm->closing)return UE4SS_LUA_HOST_RETIRED;
        auto i=vm->emitted.find(storage);if(i==vm->emitted.end())return UE4SS_LUA_HOST_INVALID_HANDLE;
        output=i->second;auto n=number(output.kind);
        if(n<vm->factories.size() && vm->factories[n])output.factory_generation=vm->factories[n]->factory_generation;
        return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::cache(const ScopeLease& scope,LuaObjectEmissionProof& proof,LuaObjectFactoryRecord& incoming,bool& adopted) noexcept {
    adopted=false;
    try {
        std::lock_guard lock(state_->mutex);auto* vm=state_->find(scope.vm());if(!vm)return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(vm->closing)return UE4SS_LUA_HOST_RETIRED;
        auto i=vm->emitted.find(proof.storage);
        if(i==vm->emitted.end() || !same_emission(i->second,proof))return UE4SS_LUA_HOST_INVALID_HANDLE;
        const auto* descriptor=lua_object_host_descriptor(proof.kind);auto n=number(proof.kind);
        if(!descriptor || n>=vm->factories.size() || incoming.vm!=scope.vm() || incoming.kind!=proof.kind ||
           incoming.storage_type!=proof.storage_type || proof.size!=descriptor->size ||
           incoming.metatable_ref<=0 || incoming.members_ref<=0 || incoming.metamethods_ref<=0 ||
           !incoming.metatable_identity || !incoming.members_identity || !incoming.metamethods_identity ||
           (incoming.private_registry_key && incoming.private_registry_key!=lua_object_host_private_factory_key(proof.kind)))return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto destructor=vm->finalizers.find(incoming.destructor);
        if(destructor==vm->finalizers.end() || destructor->second!=proof.storage_type)return UE4SS_LUA_HOST_PENDING;
        const auto* auxiliary=lua_object_host_metamethod_descriptor();
        auto methods=vm->emitted.find(incoming.metamethods_identity);
        if(methods==vm->emitted.end() || methods->second.kind!=LuaObjectKind::meta_methods ||
           methods->second.storage_type!=auxiliary->storage_type || methods->second.size!=auxiliary->size ||
           !incoming.metamethods_metatable_identity)return UE4SS_LUA_HOST_INVALID_HANDLE;
        auto methods_destructor=vm->finalizers.find(incoming.metamethods_destructor);
        if(methods_destructor==vm->finalizers.end() || methods_destructor->second!=auxiliary->storage_type)return UE4SS_LUA_HOST_PENDING;
        for(const auto& cached:vm->factories)
            if(cached && cached->metatable_identity==incoming.metatable_identity && cached->storage_type!=incoming.storage_type)
                return UE4SS_LUA_HOST_INVALID_HANDLE; // actual pinned UStruct/UClass name collision
        if(vm->factories[n]) {
            if(!same_metadata(*vm->factories[n],incoming))return UE4SS_LUA_HOST_INVALID_HANDLE;
            // Requester's original refs remain its responsibility on reuse.
            // Supply the existing pinned metadata after it releases the C request.
            incoming=*vm->factories[n];proof.factory_generation=incoming.factory_generation;return UE4SS_LUA_HOST_OK;
        }
        incoming.factory_generation=state_->next();if(!incoming.factory_generation)return UE4SS_LUA_HOST_INTERNAL_ERROR;
        vm->factories[n]=incoming;proof.factory_generation=incoming.factory_generation;adopted=true;
        return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::publish(const ScopeLease& scope,LuaObjectEmissionProof& proof) noexcept {
    try {
        std::lock_guard lock(state_->mutex);auto* vm=state_->find(scope.vm());if(!vm)return UE4SS_LUA_HOST_INVALID_HANDLE;
        if(vm->closing)return UE4SS_LUA_HOST_RETIRED;
        auto n=number(proof.kind);const auto* descriptor=lua_object_host_descriptor(proof.kind);
        if(!descriptor || n>=vm->factories.size() || !vm->factories[n] || !proof.storage || proof.vm!=scope.vm() ||
           proof.storage_type!=descriptor->storage_type || proof.size!=descriptor->size ||
           proof.factory_generation!=vm->factories[n]->factory_generation)return UE4SS_LUA_HOST_INVALID_HANDLE;
        proof.emission_generation=state_->next();if(!proof.emission_generation)return UE4SS_LUA_HOST_INTERNAL_ERROR;
        if(vm->emitted.contains(proof.storage))return UE4SS_LUA_HOST_WRONG_OWNER;
        vm->emitted.emplace(proof.storage,proof);return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
void LuaObjectAuthorityRuntime::retract(const ScopeLease& scope,const LuaObjectEmissionProof& proof) noexcept {
    try {
        std::lock_guard lock(state_->mutex);auto* vm=state_->find(scope.vm());if(!vm)return;
        auto i=vm->emitted.find(proof.storage);if(i!=vm->emitted.end() && same_emission(i->second,proof))vm->emitted.erase(i);
    }catch(...){}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::detach_vm(VmHandle handle,lua_State* root) noexcept {
    try {
        auto status=state_->lifecycle.validate_vm_finalizer(handle);
        if(status!=Status::ok)return static_cast<ue4ss_lua_host_status>(status);
        std::array<std::optional<LuaObjectFactoryRecord>,9> retired;
        {
            std::lock_guard lock(state_->mutex);auto* vm=state_->find(handle);
            if(!vm || vm->root!=root)return UE4SS_LUA_HOST_WRONG_OWNER;
            vm->closing=true;vm->emitted.clear();vm->finalizers.clear();
            for(std::size_t n=0;n<retired.size();++n){retired[n]=vm->pending_cleanup[n]?vm->pending_cleanup[n]:vm->factories[n];vm->pending_cleanup[n].reset();vm->factories[n].reset();}
        } // No native control mutex is held while acquiring the Lua gate.
        ue4ss_lua_host_status result=UE4SS_LUA_HOST_OK;
        {
        Gate gate{root};
        for(auto& factory:retired)if(factory) {
            uikit_lua_object_request r{};r.operation=UIKIT_OBJECT_RELEASE;
            r.metatable_ref=factory->metatable_ref;r.members_ref=factory->members_ref;r.metamethods_ref=factory->metamethods_ref;
            if(uikit_lua_object_run(root,&r)!=LUA_OK || r.status!=UE4SS_LUA_HOST_OK)result=UE4SS_LUA_HOST_INTERNAL_ERROR;
            factory->metatable_ref=r.metatable_ref;factory->members_ref=r.members_ref;factory->metamethods_ref=r.metamethods_ref;
            if(r.metatable_ref<=0 && r.members_ref<=0 && r.metamethods_ref<=0)factory.reset();
        }
        }
        {std::lock_guard lock(state_->mutex);if(auto* vm=state_->find(handle))vm->pending_cleanup=retired;}
        return result;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
ue4ss_lua_host_status LuaObjectAuthorityRuntime::forget_closed_vm(VmHandle handle,lua_State* root) noexcept {
    try {
        const auto closed=state_->lifecycle.validate_closed_vm(handle);
        if(closed!=Status::ok)return static_cast<ue4ss_lua_host_status>(closed);
        std::lock_guard lock(state_->mutex);auto* vm=state_->find(handle);
        if(!vm || vm->root!=root || !vm->closing)return UE4SS_LUA_HOST_WRONG_OWNER;
        state_->roots.erase(root);state_->vms.erase(handle.id);return UE4SS_LUA_HOST_OK;
    }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
}
