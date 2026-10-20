#include <LuaHost/LuaReferences.hpp>
#include <lua.hpp>
#include "LuaReferenceFrame.h"
#include <cassert>
#include <new>
#include <stdexcept>
#include <type_traits>

// These are the existing embedded LuaRaw gate, not a per-VM/consumer mutex.
extern "C" void LuaLock(lua_State*);
extern "C" void LuaUnlock(lua_State*);

namespace RC::LuaHost {
namespace {
struct Gate {
    lua_State* root;
    explicit Gate(lua_State* state):root(state) { LuaLock(root); }
    ~Gate() { LuaUnlock(root); }
};
using Request = uikit_lua_reference_request;
static_assert(std::is_trivially_destructible_v<Request>);
Status run(lua_State* state, Request& request, int value_index=0) {
    return uikit_lua_reference_run(state, &request, value_index)==LUA_OK
        ? Status::ok : Status::internal_error;
}
}
LuaReferences::LuaReferences(Lifecycle& life,VmHandle vm,lua_State* root)
    :lifecycle_(life),vm_(vm),root_(root) {
    if (!root || !vm.id || !vm.generation) throw std::invalid_argument("verified host VM binding required");
}
LuaReferences::~LuaReferences() {
    // A future host VM owner must enforce this before destroying its descriptor.
    // Never dereference a state from a destructor that might run after lua_close.
    assert(records_.empty());
}
Result<ReferenceHandle> LuaReferences::retain(const ScopeLease& scope,int index) {
    const auto status=lifecycle_.validate_scope(scope);
    if (status!=Status::ok) return {status,{}};
    if (scope.vm()!=vm_) return {Status::wrong_owner,{}};
    // A matching live scope pins this VM before reading the root binding.
    return retain(scope,index,root_);
}
Result<ReferenceHandle> LuaReferences::retain(const ScopeLease& scope,int index,lua_State* actual) {
    auto status=lifecycle_.validate_scope(scope);
    if (status!=Status::ok) return {status,{}};
    if (scope.vm()!=vm_ || !root_ || !actual) return {Status::wrong_owner,{}};
    Gate guard(actual);
    status=lifecycle_.validate_scope(scope);
    if (status!=Status::ok) return {status,{}};
    if (!uikit_lua_reference_vm_matches(actual,root_)) return {Status::wrong_owner,{}};
    index=uikit_lua_reference_normalize(actual,index);
    if (!index) return {Status::invalid_handle,{}};
    auto handle=lifecycle_.create_reference(scope);
    if (handle.status!=Status::ok) return handle;
    // Allocate bookkeeping before creating a Lua ref. A C++ allocation failure
    // therefore cannot lose ownership of a newly allocated registry slot.
    try { records_.emplace(handle.value.id,Record{handle.value,LUA_NOREF,scope.module()}); }
    catch (const std::bad_alloc&) {
        lifecycle_.release_reference_for_cleanup(handle.value,scope);
        return {Status::internal_error,{}};
    }
    size_.store(records_.size());
    Request request{UIKIT_REFERENCE_RETAIN,LUA_NOREF};
    status=run(actual,request,index);
    // Do not hold a map iterator across Lua: GC may reenter and rehash the map.
    records_.at(handle.value.id).index=request.index;
    if (status==Status::ok) status=lifecycle_.validate_reference(handle.value,scope);
    if (status!=Status::ok) {
        if (request.index!=LUA_NOREF) {
            Request rollback{UIKIT_REFERENCE_RELEASE,request.index};
            if (run(actual,rollback)!=Status::ok) {
                // Track this private pending cleanup slot until VM finalization;
                // no successful handle was returned to the consumer.
                return {Status::internal_error,{}};
            }
        }
        lifecycle_.release_reference_for_cleanup(handle.value,scope);
        records_.erase(handle.value.id);size_.store(records_.size());
        return {status,{}};
    }
    records_.at(handle.value.id).committed=true;
    return handle;
}
Status LuaReferences::push(const ScopeLease& scope,ReferenceHandle handle) {
    const auto status=lifecycle_.validate_reference(handle,scope);
    if (status!=Status::ok) return status;
    if (scope.vm()!=vm_) return Status::wrong_owner;
    return push(scope,handle,root_);
}
Status LuaReferences::push(const ScopeLease& scope,ReferenceHandle handle,lua_State* actual) {
    auto status=lifecycle_.validate_reference(handle,scope);
    if (status!=Status::ok) return status;
    if (scope.vm()!=vm_ || !root_ || !actual) return Status::wrong_owner;
    Gate guard(actual);
    status=lifecycle_.validate_reference(handle,scope);
    if (status!=Status::ok) return status;
    if (!uikit_lua_reference_vm_matches(actual,root_)) return Status::wrong_owner;
    auto found=records_.find(handle.id);
    if (found==records_.end() || !found->second.committed) return Status::invalid_handle;
    if (found->second.busy) return Status::busy;
    found->second.busy=true;
    const int top=uikit_lua_reference_top(actual);
    Request request{UIKIT_REFERENCE_PUSH,found->second.index};
    status=run(actual,request);
    records_.at(handle.id).busy=false;
    if (status==Status::ok) {
        status=lifecycle_.validate_reference(handle,scope);
        if (status!=Status::ok) uikit_lua_reference_pop_to(actual,top);
    }
    return status;
}
Status LuaReferences::release(const ScopeLease& scope,ReferenceHandle handle) {
    const auto status=lifecycle_.validate_reference(handle,scope);
    if (status!=Status::ok) return status;
    if (scope.vm()!=vm_) return Status::wrong_owner;
    return release(scope,handle,root_);
}
Status LuaReferences::release(const ScopeLease& scope,ReferenceHandle handle,lua_State* actual) {
    auto status=lifecycle_.validate_reference(handle,scope);
    if (status!=Status::ok) return status;
    if (scope.vm()!=vm_ || !root_ || !actual) return Status::wrong_owner;
    Gate guard(actual);
    status=lifecycle_.validate_reference(handle,scope);
    if (status!=Status::ok) return status;
    if (!uikit_lua_reference_vm_matches(actual,root_)) return Status::wrong_owner;
    auto found=records_.find(handle.id);
    if (found==records_.end() || !found->second.committed) return Status::invalid_handle;
    if (found->second.busy) return Status::busy;
    found->second.busy=true;
    Request request{UIKIT_REFERENCE_RELEASE,found->second.index};
    status=run(actual,request);
    if (status!=Status::ok) { records_.at(handle.id).busy=false; return status; }
    status=lifecycle_.release_reference_for_cleanup(handle,scope);
    records_.erase(handle.id);size_.store(records_.size());
    return status;
}
Status LuaReferences::cleanup_vm() {
    auto status=lifecycle_.validate_vm_finalizer(vm_);
    if (status!=Status::ok) return status;
    if (!root_) return Status::ok;
    Gate guard(root_);
    status=lifecycle_.validate_vm_finalizer(vm_);
    if (status!=Status::ok) return status;
    if (cleaning_) return Status::busy;
    cleaning_=true;
    while (!records_.empty()) {
        const auto record=records_.begin()->second;
        if (record.index!=LUA_NOREF) {
            Request request{UIKIT_REFERENCE_RELEASE,record.index};
            status=run(root_,request);
            if (status!=Status::ok) { cleaning_=false; return status; }
        }
        status=lifecycle_.release_reference_at_vm_finalizer(record.handle,vm_);
        // A previously finalized module may already have dropped its pure record.
        if (status!=Status::ok && status!=Status::invalid_handle) {
            cleaning_=false;return status;
        }
        records_.erase(record.handle.id);size_.store(records_.size());
    }
    root_=nullptr;cleaning_=false;
    return Status::ok;
}
Status LuaReferences::cleanup_module(const ScopeLease& scope) {
    auto status=lifecycle_.validate_module_cleanup(scope);
    if (status!=Status::ok) return status;
    if (scope.vm()!=vm_ || !root_) return Status::wrong_owner;
    Gate guard(root_);
    status=lifecycle_.validate_module_cleanup(scope);
    if (status!=Status::ok) return status;
    if (cleaning_) return Status::busy;
    cleaning_=true;
    for (;;) {
        auto found=records_.begin();
        while (found!=records_.end() && found->second.module!=scope.module()) ++found;
        if (found==records_.end()) break;
        const auto record=found->second;
        if (record.index!=LUA_NOREF) {
            Request request{UIKIT_REFERENCE_RELEASE,record.index};
            status=run(root_,request);
            if (status!=Status::ok) { cleaning_=false; return status; }
        }
        status=lifecycle_.release_reference_at_module_cleanup(record.handle,scope);
        if (status!=Status::ok) { cleaning_=false; return status; }
        records_.erase(record.handle.id);size_.store(records_.size());
        // Never keep an iterator across Lua. Other live modules may reenter.
    }
    cleaning_=false;
    return Status::ok;
}
}
