#include <LuaHost/OwnedUserdataBindings.hpp>
#include <LuaHost/HostBindings.hpp>
#include <LuaHost/HostIntegration.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaMadeSimple/HostOwnedUserdataHooks.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <LuaMadeSimple/HostThreadAccess.hpp>
#include "LuaOwnedUserdataFrame.h"
#include "LuaNativeFrame.h"
#include <lauxlib.h>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace RC::LuaHost {
namespace {
using LuaMadeSimple::HostOwnedUserdataConstruction;
using LuaMadeSimple::HostOwnedUserdataPhase;
enum class Phase { allocated,constructing,alive,destroying,destroyed,failed };
struct Record {
    std::uint64_t ticket{};VmHandle vm{};ModuleHandle module{};
    const void* type{};void* storage{};std::size_t size{},alignment{};
    void (*destroy)(void*){};int (*automatic_gc)(lua_State*){};
    int reference{LUA_NOREF};Phase phase{Phase::allocated};CallbackLease pin;
};
// A failed receipt is deliberately never auto-disposed by static teardown.
// Pins are released only by explicit completed destruction/unused allocation
// rollback, not by process-global C++ object destruction order.
std::mutex& RecordsMutex=*new std::mutex;
auto& Records=*new std::unordered_map<std::uint64_t,std::shared_ptr<Record>>;
std::uint64_t Next{1};
thread_local Record* Current{};
struct CurrentScope {Record* previous;explicit CurrentScope(Record* r):previous(Current){Current=r;}
    ~CurrentScope(){Current=previous;}};
std::uint32_t code(Status status){return static_cast<std::uint32_t>(status);}
// Host-owned holders are outside this POD same-CallInfo frame. The exact
// instantiation's destructor/constructor can nonlocally return only to here.
struct NativeRequest {lua_State* actual;void* payload;void (*function)(void*);bool returned{},threw{};};
void invoke_native(void* opaque) noexcept {
    auto* r=static_cast<NativeRequest*>(opaque);
    try {r->function(r->payload);r->returned=true;}catch(...){r->threw=true;}
}
void run_destructor(void* opaque) noexcept {
    auto* r=static_cast<NativeRequest*>(opaque);
    if(uikit_lua_native_run(r->actual,r,invoke_native)!=LUA_OK)r->returned=false;
}
Status destroy_record(const std::shared_ptr<Record>& record,lua_State* actual) noexcept {
    {
        std::lock_guard lock{RecordsMutex};
        if(record->phase==Phase::destroyed)return Status::ok;
        if(record->phase==Phase::failed||record->phase==Phase::constructing)return Status::internal_error;
        if(record->phase==Phase::destroying)return Status::pending;
        if(record->phase!=Phase::alive)return Status::ok; // T does not exist yet
        record->phase=Phase::destroying;
    }
    NativeRequest request{actual,record->storage,record->destroy};
    // CallbackLease remains alive while Native code executes, including its
    // return instruction. Retiring admission is overridden only by this exact
    // previously authenticated owned emission. No shared metadata lock held.
    HostBindings::instance().api().run_code_manager_destruction(record->module,&request,run_destructor);
    const bool done=request.returned&&!request.threw;
    {
        std::lock_guard lock{RecordsMutex};
        record->phase=done?Phase::destroyed:Phase::failed;
    }
    if(!done)return Status::internal_error; // persistent pin refuses unload
    LuaMadeSimple::notify_host_userdata_destroying(actual,record->storage,record->type);
    return Status::ok;
}
void erase_record(const std::shared_ptr<Record>& record) noexcept {
    std::shared_ptr<Record> detached;
    {std::lock_guard lock{RecordsMutex};const auto it=Records.find(record->ticket);
     if(it!=Records.end()&&it->second==record){detached=std::move(it->second);Records.erase(it);}}
    // detached/pin disposal occurs outside the metadata lock and after the real
    // Native destructor has returned; it invokes no consumer manager.
}
void complete_record(const std::shared_ptr<Record>& record) noexcept {
    // Keep a type/lifetime tombstone while the actual userdata may still exist
    // (manual __gc). Its Native function addresses are never used again.
    {std::lock_guard lock{RecordsMutex};record->destroy=nullptr;record->automatic_gc=nullptr;}
    record->pin.reset(); // outside lock, AFTER actual Native destruction return
}
std::uint32_t execute(const HostOwnedUserdataConstruction* r,bool* handled) noexcept {
    if(!handled)return code(Status::invalid_handle);*handled=false;
    if(!r||!r->lua||!r->construct||!r->destroy||!r->type||!r->size||!r->alignment)
        return code(Status::invalid_handle);
    try {
        const auto image=code_image(reinterpret_cast<const void*>(r->construct));
        const auto destruction=code_image(reinterpret_cast<const void*>(r->destroy));
        if(!image||!destruction)return code(Status::invalid_handle);
        if(image==host_code_image()&&destruction==host_code_image()) {
            if(!Current)return 0;
            // Nested actual host SDK emission must not inherit the Native T's
            // allocation ticket. Keep its original host raw automatic GC.
            *handled=true;CurrentScope mask{nullptr};
            NativeRequest request{r->lua->get_lua_state(),r->payload,r->construct};
            const auto status=uikit_lua_native_run(request.actual,&request,invoke_native);
            return status==LUA_OK&&request.returned&&!request.threw?0:code(Status::internal_error);
        }
        *handled=true;
        if(image!=destruction)return code(Status::wrong_owner);
        // Neither template code location nor callback return proves the creator
        // of a shared_ptr control block, or actual destruction by custom __gc.
        if(r->shared||r->custom_gc)return code(Status::wrong_owner);
        if(!r->automatic_gc||code_image(reinterpret_cast<const void*>(r->automatic_gc))!=image)
            return code(Status::wrong_owner);
        auto& host=HostBindings::instance();
        const auto owner=host.api().code_owner();const auto module=host.module_for_image(image);
        if(owner.status!=Status::ok)return code(owner.status);
        if(module.status!=Status::ok)return code(module.status);
        if(owner.value!=module.value)return code(Status::wrong_owner);
        const auto actual=r->lua->get_lua_state();
        const auto root=LuaMadeSimple::cached_root_lua_state(actual);
        if(!root||!LuaMadeSimple::has_host_execution_scope(root))return code(Status::wrong_owner);
        const auto vm=host.find_vm(root);if(vm.status!=Status::ok)return code(vm.status);
        auto pin=host.lifecycle().enter_callback(owner.value);if(pin.status!=Status::ok)return code(pin.status);
        auto scope=host.lifecycle().enter_scope(vm.value,pin.value);if(scope.status!=Status::ok)return code(scope.status);
        auto record=std::make_shared<Record>();record->vm=vm.value;record->module=owner.value;
        record->type=r->type;record->size=r->size;record->alignment=r->alignment;
        record->destroy=r->destroy;record->automatic_gc=r->automatic_gc;record->pin=std::move(pin.value);
        {
            std::lock_guard lock{RecordsMutex};
            if(Next>=static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max()))return code(Status::internal_error);
            record->ticket=Next++;Records.emplace(record->ticket,record);
        }
        const int original_top=lua_gettop(actual);
        CurrentScope current{record.get()};
        NativeRequest request{actual,r->payload,r->construct};
        const auto lua_status=uikit_lua_native_run(actual,&request,invoke_native);
        if(lua_status==LUA_OK&&request.returned&&!request.threw&&record->phase==Phase::alive) {
            uikit_owned_userdata_request commit{};commit.action=UIKIT_OWNED_COMMIT;
            commit.reference=record->reference;commit.ticket=record->ticket;
            if(uikit_owned_userdata_run(actual,&commit)==LUA_OK){record->reference=LUA_NOREF;return 0;}
        }
        // The strong allocation receipt survives all construction failures.
        // A C++ constructor exception unwinds its subobjects normally; a Lua
        // nonlocal return DURING T construction has no valid complete-T
        // destructor, so the failed record/root/pin stays as an explicit block.
        if(record->phase==Phase::constructing) {
            std::lock_guard lock{RecordsMutex};record->phase=Phase::failed;
            return code(Status::internal_error);
        }
        if(record->phase==Phase::alive&&destroy_record(record,actual)!=Status::ok)return code(Status::internal_error);
        if(record->phase==Phase::allocated&&record->reference==LUA_NOREF) {
            // Failed allocation/root receipt created no T. Releasing its code
            // pin requires no Lua allocation or weak-table construction.
            if(record->storage){
                {std::lock_guard lock{RecordsMutex};record->phase=Phase::destroyed;}
                complete_record(record);
            }else erase_record(record);
            return code(Status::internal_error);
        }
        uikit_owned_userdata_request remove{};remove.action=UIKIT_OWNED_REMOVE;
        remove.ticket=record->ticket;remove.reference=record->reference;
        // Preserve the protected frame's returned Lua error/stack on failure.
        remove.top=original_top;
        if(uikit_owned_userdata_run(actual,&remove)!=LUA_OK)return code(Status::internal_error);
        record->reference=LUA_NOREF;
        if(record->storage) {
            {std::lock_guard lock{RecordsMutex};record->phase=Phase::destroyed;}
            complete_record(record);
        }else erase_record(record);
        return code(Status::internal_error);
    }catch(...){return code(Status::internal_error);}
}
std::uint32_t allocate(lua_State* actual,const void* type,std::size_t size,int values,void** storage,bool* owned) noexcept {
    if(!storage||!owned)return code(Status::invalid_handle);*storage=nullptr;*owned=false;
    try {
        if(!Current){
            // HOST emissions retain their original raw path; no persistent
            // generic ownership record is inferred from type address alone.
            return 0;}
        if(Current->storage||Current->type!=type||Current->size!=size)return code(Status::wrong_owner);
        const auto root=LuaMadeSimple::cached_root_lua_state(actual);const auto vm=HostBindings::instance().find_vm(root);
        if(vm.status!=Status::ok||vm.value!=Current->vm)return code(Status::wrong_owner);
        uikit_owned_userdata_request r{};r.action=UIKIT_OWNED_ALLOC;r.size=size;r.values=values;r.reference=LUA_NOREF;
        const auto status=uikit_owned_userdata_run(actual,&r);
        Current->storage=r.storage;Current->reference=r.reference;
        if(status!=LUA_OK)return code(Status::internal_error);
        if(reinterpret_cast<std::uintptr_t>(r.storage)%Current->alignment)return code(Status::wrong_owner);
        *storage=r.storage;*owned=true;return 0;
    }catch(...){return code(Status::internal_error);}
}
std::uint32_t arm(lua_State* actual,void* storage) noexcept {
    try {if(!Current)return 0;if(Current->storage!=storage)return code(Status::wrong_owner);
        uikit_owned_userdata_request r{};r.action=UIKIT_OWNED_ARM;r.expected_gc=uikit_owned_userdata_gc;
        return uikit_owned_userdata_run(actual,&r)==LUA_OK?0:code(Status::internal_error);
    }catch(...){return code(Status::internal_error);}
}
std::uint32_t finalizer(lua_State* actual,const void* type,int (*original)(lua_State*),int (**published)(lua_State*)) noexcept {
    if(!published||!type||!original)return code(Status::invalid_handle);*published=nullptr;
    try {
        const auto image=code_image(reinterpret_cast<const void*>(original));
        if(!image)return code(Status::invalid_handle);
        if(image==host_code_image()){*published=original;return 0;}
        auto& host=HostBindings::instance();const auto owner=host.api().code_owner();const auto module=host.module_for_image(image);
        if(owner.status!=Status::ok)return code(owner.status);
        if(module.status!=Status::ok)return code(module.status);
        if(owner.value!=module.value)return code(Status::wrong_owner);
        const auto root=LuaMadeSimple::cached_root_lua_state(actual);
        if(!root||!LuaMadeSimple::has_host_execution_scope(root))return code(Status::wrong_owner);
        // No Native GC address is retained in Lua; original is used only as
        // pre-emission compiler identity, and requires an exact owned record.
        *published=uikit_owned_userdata_gc;return 0;
    }catch(...){return code(Status::internal_error);}
}
bool readable(lua_State* actual,void* storage,const void* type) noexcept {
    try {const auto root=LuaMadeSimple::cached_root_lua_state(actual);
        if(!root||!LuaMadeSimple::has_host_execution_scope(root))return false;
        const auto vm=HostBindings::instance().find_vm(root);if(vm.status!=Status::ok)return false;
        // Exact HOST storage retains its separate real descriptor authority;
        // Native tombstones cannot certify or reject a new HOST allocation.
        if(code_image(type)==host_code_image())return true;
        std::lock_guard lock{RecordsMutex};const Record* latest{};
        for(const auto& [ticket,r]:Records)if(r->vm==vm.value&&r->storage==storage&&(!latest||ticket>latest->ticket))latest=r.get();
        return !latest||(latest->type==type&&latest->phase==Phase::alive);
    }catch(...){return false;}
}
void phase(lua_State*,void* storage,HostOwnedUserdataPhase value) noexcept {
    try {if(!Current||Current->storage!=storage)return;std::lock_guard lock{RecordsMutex};
        if(value==HostOwnedUserdataPhase::constructing&&Current->phase==Phase::allocated)Current->phase=Phase::constructing;
        else if(value==HostOwnedUserdataPhase::constructed&&Current->phase==Phase::constructing)Current->phase=Phase::alive;
        else if(value==HostOwnedUserdataPhase::constructor_threw&&Current->phase==Phase::constructing)Current->phase=Phase::allocated;
    }catch(...){}
}
}
bool install_owned_userdata_bindings() noexcept {
    return LuaMadeSimple::install_host_owned_userdata_hooks({execute,allocate,arm,finalizer,readable,phase});
}
Status cleanup_owned_userdata_vm(VmHandle vm,lua_State* root) noexcept {
    try {
        auto& host=HostBindings::instance();
        auto status=host.lifecycle().validate_vm_finalizer(vm);if(status!=Status::ok)return status;
        LuaMadeSimple::LuaFinalizationScope closing{root,root};
        std::vector<std::shared_ptr<Record>> records;
        {std::lock_guard lock{RecordsMutex};for(const auto& [ticket,r]:Records){(void)ticket;if(r->vm==vm)records.push_back(r);}}
        for(const auto& record:records) {
            if(record->phase==Phase::failed||record->phase==Phase::constructing)return Status::internal_error;
            if(record->phase==Phase::destroying)return Status::pending;
            if(record->phase==Phase::alive) {
                uikit_owned_userdata_request get{};get.action=UIKIT_OWNED_GET;get.ticket=record->ticket;
                const int top=lua_gettop(root);
                if(uikit_owned_userdata_run(root,&get)!=LUA_OK)return Status::internal_error;
                // Weak value disappearance without the authenticated HOST GC
                // receipt proves no destruction; never touch dangling storage.
                if(get.storage!=record->storage||get.observed_size!=record->size)return Status::internal_error;
                status=destroy_record(record,root);if(status!=Status::ok)return status;
                uikit_owned_userdata_request remove{};remove.action=UIKIT_OWNED_REMOVE;
                remove.ticket=record->ticket;remove.reference=record->reference;remove.top=top;
                if(uikit_owned_userdata_run(root,&remove)!=LUA_OK)return Status::internal_error;
            }else {
                uikit_owned_userdata_request remove{};remove.action=UIKIT_OWNED_REMOVE;
                remove.ticket=record->ticket;remove.reference=record->reference;remove.top=lua_gettop(root);
                if(uikit_owned_userdata_run(root,&remove)!=LUA_OK)return Status::internal_error;
            }
            record->reference=LUA_NOREF;erase_record(record);
        }
        return Status::ok;
    }catch(...){return Status::internal_error;}
}
Status forget_owned_userdata_vm(VmHandle vm) noexcept {
    try {std::lock_guard lock{RecordsMutex};for(const auto& [ticket,r]:Records){(void)ticket;if(r->vm==vm)return Status::pending;}
        return Status::ok;}catch(...){return Status::internal_error;}
}
}
extern "C" int uikit_owned_userdata_authorize_gc(lua_State* actual) {
    try {const auto root=RC::LuaMadeSimple::cached_root_lua_state(actual);
        return root&&RC::LuaMadeSimple::has_host_execution_scope(root)&&
            RC::LuaHost::HostBindings::instance().find_vm(root).status==RC::LuaHost::Status::ok;
    }catch(...){return 0;}
}
extern "C" int uikit_owned_userdata_dispatch_gc(lua_State* actual,void* storage,size_t size) {
    using namespace RC::LuaHost;
    try {
        const auto root=RC::LuaMadeSimple::cached_root_lua_state(actual);
        if(!root||!RC::LuaMadeSimple::has_host_execution_scope(root))return 1;
        const auto vm=HostBindings::instance().find_vm(root);if(vm.status!=Status::ok)return 1;
        std::shared_ptr<Record> record;
        {std::lock_guard lock{RecordsMutex};for(const auto& [ticket,r]:Records){(void)ticket;
            if(r->vm==vm.value&&r->storage==storage&&(!record||ticket>record->ticket)){record=r;}}}
        if(!record)return 0; // destroyed/unconstructed object; never enter Native
        if(size!=record->size)return 1;
        const auto status=destroy_record(record,actual);if(status!=Status::ok)return 1;
        if(record->phase==Phase::destroyed)complete_record(record);
        return 0;
    }catch(...){return 1;}
}
