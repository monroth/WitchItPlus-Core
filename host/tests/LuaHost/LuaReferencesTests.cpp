#include <LuaHost/LuaReferences.hpp>
#include <lua.hpp>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace RC::LuaHost;
static std::recursive_mutex gate;
static int gate_entries;
static std::function<void()> on_gate;
extern "C" void LuaLock(lua_State*) {
    gate.lock(); ++gate_entries;
    if (on_gate) { auto call=std::move(on_gate); on_gate={}; call(); }
}
extern "C" void LuaUnlock(lua_State*) { gate.unlock(); }
static int checks;
static void check(bool value,const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
}
struct Allocator { bool fail{}; Lifecycle* retire_life{}; VmHandle retire_vm{}; bool retired{}; };
static void* allocate(void* context,void* pointer,size_t,size_t size) {
    if (!size) { std::free(pointer); return nullptr; }
    auto* allocator=static_cast<Allocator*>(context);
    if (allocator->retire_life && !allocator->retired) {
        allocator->retired=true;
        allocator->retire_life->request_vm_close(allocator->retire_vm);
    }
    if (allocator->fail) return nullptr;
    return std::realloc(pointer,size);
}
static int collected[3];
static int finalizer(lua_State* state) {
    const auto tag=*static_cast<int*>(lua_touserdata(state,1));
    if (tag>0 && tag<3) ++collected[tag];
    return 0;
}
static void push_collectible(lua_State* state,int tag) {
    *static_cast<int*>(lua_newuserdatauv(state,sizeof(int),0))=tag;
    lua_newtable(state);lua_pushcfunction(state,finalizer);
    lua_setfield(state,-2,"__gc");lua_setmetatable(state,-2);
}
static LuaReferences* hook_refs;
static const ScopeLease* hook_scope;
static std::vector<ReferenceHandle> hook_handles;
static bool hook_failed;
static void reentry_hook(lua_State* state,lua_Debug*) {
    lua_sethook(state,nullptr,0,0);
    if (lua_gettop(state)<2) { hook_failed=true; return; }
    for (int i=0;i<128;++i) {
        const auto result=hook_refs->retain(*hook_scope,2);
        if (result.status!=Status::ok) { hook_failed=true; return; }
        try { hook_handles.push_back(result.value); }
        catch (...) { hook_failed=true; return; }
    }
}
int main() {
    Allocator allocator;
    auto* root=lua_newstate(allocate,&allocator);
    check(root!=nullptr,"real Lua VM");
    Lifecycle life;
    const auto vm=life.create_vm().value;
    const auto module=life.create_module().value;
    check(life.publish_vm(vm)==Status::ok,"publish VM");
    check(life.publish_module(module)==Status::ok,"publish module");
    LuaReferences refs(life,vm,root);
    auto callback=life.enter_callback(module);
    auto scope=life.enter_scope(vm,callback.value);
    lua_newtable(root);
    const int top=lua_gettop(root);
    auto reference=refs.retain(scope.value,-1);
    check(reference.status==Status::ok && refs.size()==1,"retain real registry reference");
    check(lua_gettop(root)==top,"retain preserves caller stack");
    const void* identity=lua_topointer(root,-1);
    check(refs.push(scope.value,reference.value)==Status::ok,"push retained table");
    check(lua_gettop(root)==top+1 && lua_topointer(root,-1)==identity,"same Lua object");
    lua_pop(root,1);
    auto nilref=refs.retain(scope.value,0);
    check(nilref.status==Status::invalid_handle && refs.size()==1,"invalid stack index rejected");
    auto stale=reference.value; ++stale.generation;
    check(refs.push(scope.value,stale)==Status::stale_generation,"stale reference rejected");
    const auto other_module=life.create_module().value;
    life.publish_module(other_module);
    auto other_callback=life.enter_callback(other_module);
    auto other_scope=life.enter_scope(vm,other_callback.value);
    check(refs.push(other_scope.value,reference.value)==Status::wrong_owner,"cross-module read rejected");
    const int before_thread=gate_entries;
    Status foreign{};
    std::thread worker([&]{ foreign=refs.push(scope.value,reference.value); }); worker.join();
    check(foreign==Status::wrong_thread && gate_entries==before_thread,"wrong thread before Lua gate");
    check(refs.release(scope.value,reference.value)==Status::ok && refs.size()==0,"release actual registry slot");
    check(refs.push(scope.value,reference.value)==Status::invalid_handle,"released reference cannot be read");
    push_collectible(root,1);
    auto collectible=refs.retain(scope.value,-1);lua_pop(root,1);
    lua_gc(root,LUA_GCCOLLECT);
    check(collected[1]==0,"registry reference really roots userdata during GC");
    check(refs.release(scope.value,collectible.value)==Status::ok,"unref collectible");
    lua_gc(root,LUA_GCCOLLECT);
    check(collected[1]==1,"actual Lua unref permits collection");
    lua_pushnil(root);auto nil_handle=refs.retain(scope.value,-1);lua_pop(root,1);
    check(nil_handle.status==Status::ok,"nil reference has an owned handle");
    check(refs.push(scope.value,nil_handle.value)==Status::ok && lua_isnil(root,-1),"nil restored without registry index aliasing");
    lua_pop(root,1);check(refs.release(scope.value,nil_handle.value)==Status::ok,"nil handle released");
    // A real Lua call hook reenters while retain's C frame has not yet run.
    // It grows the C++ map enough to rehash; the outer operation must not keep
    // an invalidated iterator across pcall or lose either set of references.
    hook_refs=&refs;hook_scope=&scope.value;hook_handles.reserve(128);
    lua_sethook(root,reentry_hook,LUA_MASKCALL,0);
    auto reentered=refs.retain(scope.value,-1);
    check(!hook_failed && reentered.status==Status::ok && hook_handles.size()==128,"actual Lua call-hook reentry completed");
    check(refs.size()==129,"outer plus nested references owned after rehash");
    check(refs.release(scope.value,reentered.value)==Status::ok,"outer reference survives rehash");
    for (auto handle:hook_handles) check(refs.release(scope.value,handle)==Status::ok,"nested reference released");
    hook_handles.clear();hook_refs=nullptr;hook_scope=nullptr;
    check(refs.size()==0 && lua_gettop(root)==top,"reentry cleanup and caller stack balance");

    // A real Lua allocator failure must return through pcall, preserve the stack,
    // and leave no successful registry handle or stale bookkeeping record.
    std::vector<ReferenceHandle> retained;
    allocator.fail=true;
    bool failed=false;
    for (int i=0;i<256;++i) {
        const auto before=refs.size(); auto r=refs.retain(scope.value,-1);
        check(lua_gettop(root)==top,"allocator failure stack balance");
        if (r.status!=Status::ok) {
            check(r.status==Status::internal_error && refs.size()==before,"OOM has no committed reference");
            failed=true; break;
        }
        retained.push_back(r.value);
    }
    allocator.fail=false;
    check(failed,"real Lua allocation failure exercised");
    for (auto handle:retained) check(refs.release(scope.value,handle)==Status::ok,"release after OOM");
    check(refs.size()==0,"all OOM-prefix refs released");

    // Retirement racing gate acquisition must be checked again after the gate.
    on_gate=[&]{ life.request_vm_close(vm); };
    const auto raced=refs.retain(scope.value,-1);
    check(raced.status==Status::retired && refs.size()==0,"retirement during gate acquisition");
    scope.value.reset();callback.value.reset();other_scope.value.reset();other_callback.value.reset();
    check(life.claim_vm_finalization(vm)==Status::ready,"drained VM claim");
    check(refs.cleanup_vm()==Status::ok,"cleanup before Lua close");
    check(life.finish_vm_finalization(vm)==Status::ok,"finish VM after resources");
    lua_close(root);

    // Cleanup requires the actual claim, and unrefs outstanding objects before close.
    auto* second=luaL_newstate(); const auto vm2=life.create_vm().value;
    life.publish_vm(vm2); LuaReferences second_refs(life,vm2,second);
    auto cb2=life.enter_callback(module);auto scope2=life.enter_scope(vm2,cb2.value);
    lua_pushliteral(second,"kept");auto kept=second_refs.retain(scope2.value,-1);
    check(kept.status==Status::ok,"second VM retained value");
    check(second_refs.cleanup_vm()!=Status::ok,"no cleanup without finalizer claim");
    check(life.request_vm_close(vm2)==Status::pending,"close pinned by active scope");
    check(second_refs.push(scope2.value,kept.value)==Status::retired,"no normal operation while closing");
    scope2.value.reset();cb2.value.reset();
    check(life.claim_vm_finalization(vm2)==Status::ready,"second VM drained claim");
    check(second_refs.cleanup_vm()==Status::ok && second_refs.size()==0,"actual outstanding references cleaned");
    check(life.finish_vm_finalization(vm2)==Status::ok,"second VM finalized");
    lua_close(second);

    // Retiring one module must neither close the shared VM nor drop another
    // module's registry ownership. Finalizer scope pins the VM during cleanup.
    {
        auto* shared=luaL_newstate();Lifecycle shared_life;
        const auto shared_vm=shared_life.create_vm().value;
        const auto a=shared_life.create_module().value,b=shared_life.create_module().value;
        shared_life.publish_vm(shared_vm);shared_life.publish_module(a);shared_life.publish_module(b);
        LuaReferences shared_refs(shared_life,shared_vm,shared);
        auto acb=shared_life.enter_callback(a),bcb=shared_life.enter_callback(b);
        auto ascope=shared_life.enter_scope(shared_vm,acb.value),bscope=shared_life.enter_scope(shared_vm,bcb.value);
        collected[1]=collected[2]=0;
        push_collectible(shared,1);auto aref=shared_refs.retain(ascope.value,-1);lua_pop(shared,1);
        push_collectible(shared,2);auto bref=shared_refs.retain(bscope.value,-1);lua_pop(shared,1);
        check(aref.status==Status::ok && bref.status==Status::ok,"both modules own real references");
        lua_gc(shared,LUA_GCCOLLECT);
        check(collected[1]==0 && collected[2]==0,"both module references rooted");
        check(shared_refs.cleanup_module(ascope.value)==Status::wrong_owner,"normal scope cannot masquerade as cleanup");
        check(shared_life.request_module_retire(a)==Status::pending,"retire waits for old callback/scope");
        ascope.value.reset();acb.value.reset();
        check(shared_life.claim_module_finalization(a)==Status::ready,"module finalizer claim");
        auto cleanup=shared_life.enter_module_cleanup(shared_vm,a);
        check(cleanup.status==Status::ok,"host module cleanup pins shared VM");
        check(shared_life.validate_scope(cleanup.value)==Status::wrong_owner,"cleanup scope cannot admit normal operations");
        check(shared_life.create_reference(cleanup.value).status==Status::wrong_owner,"cleanup cannot mint consumer references");
        check(shared_life.release_reference_at_module_cleanup(bref.value,cleanup.value)==Status::wrong_owner,"cleanup cannot erase another module record");
        check(shared_life.finish_module_finalization(a)==Status::pending,"module cannot finalize while cleanup code is active");
        const auto gates_before=gate_entries;
        Status wrong_thread{};
        std::thread other([&]{ wrong_thread=shared_refs.cleanup_module(cleanup.value); });other.join();
        check(wrong_thread==Status::wrong_thread && gate_entries==gates_before,"foreign cleanup rejected before Lua");
        check(shared_refs.cleanup_module(cleanup.value)==Status::ok && shared_refs.size()==1,"only retiring module references removed");
        lua_gc(shared,LUA_GCCOLLECT);
        check(collected[1]==1 && collected[2]==0,"GC proves module-selective cleanup");
        check(shared_refs.push(bscope.value,bref.value)==Status::ok && lua_isuserdata(shared,-1),"live module still reads its exact value");
        lua_pop(shared,1);
        check(shared_refs.release(bscope.value,bref.value)==Status::ok,"live module can release");
        bscope.value.reset();bcb.value.reset();
        check(shared_life.request_vm_close(shared_vm)==Status::pending,"cleanup alone keeps VM pinned");
        check(shared_life.claim_vm_finalization(shared_vm)==Status::pending,"VM close cannot overtake module cleanup");
        cleanup.value.reset();
        check(shared_life.finish_module_finalization(a)==Status::ok,"module finalized after cleanup drain");
        check(shared_life.claim_vm_finalization(shared_vm)==Status::ready,"VM finalizer admitted after cleanup release");
        check(shared_refs.cleanup_vm()==Status::ok,"remaining shared VM resources cleared");
        check(shared_life.finish_vm_finalization(shared_vm)==Status::ok,"shared VM finalized");
        lua_close(shared);
    }
    // Close during an actual Lua allocation: operation must roll back the Lua
    // reference as well as its opaque record while the old scope still pins VM.
    {
        Allocator reentrant;
        auto* state=lua_newstate(allocate,&reentrant);Lifecycle owner;
        const auto v=owner.create_vm().value;
        const auto m=owner.create_module().value;
        owner.publish_vm(v);owner.publish_module(m);LuaReferences retained(owner,v,state);
        auto cb=owner.enter_callback(m);auto active=owner.enter_scope(v,cb.value);
        lua_newtable(state);
        reentrant.retire_life=&owner;reentrant.retire_vm=v;
        bool rejected=false;std::vector<ReferenceHandle> prefix;
        for (int i=0;i<1024;++i) {
            const auto before=retained.size();auto r=retained.retain(active.value,-1);
            if (r.status!=Status::ok) {
                check(r.status==Status::retired && retained.size()==before,"allocation reentry rolls back its unpublished reference");
                rejected=true;break;
            }
            prefix.push_back(r.value);
        }
        check(rejected && reentrant.retired,"retirement inside actual Lua allocator executed");
        active.value.reset();cb.value.reset();
        check(owner.claim_vm_finalization(v)==Status::ready,"reentrant close drains");
        check(retained.cleanup_vm()==Status::ok && retained.size()==0,"all earlier live references cleaned before close");
        check(owner.finish_vm_finalization(v)==Status::ok,"reentrant VM finalization completes");
        lua_close(state);
    }
    std::cout<<checks<<" real Lua reference checks PASS\n";
}
