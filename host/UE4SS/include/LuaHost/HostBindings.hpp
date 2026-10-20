#pragma once

#include <LuaHost/HostApiRuntime.hpp>
#include <LuaHost/LuaReferences.hpp>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace RC::LuaHost {
// Host-owned sidecar. Actual LuaMod roots are attached after LuaMadeSimple
// creates them; consumers cannot register states or select module owners.
class HostBindings {
public:
    struct VmObserver {
        ue4ss_lua_host_status (*attach)(VmHandle,lua_State*) noexcept{};
        ue4ss_lua_host_status (*detach)(VmHandle,lua_State*) noexcept{};
        ue4ss_lua_host_status (*forget)(VmHandle,lua_State*) noexcept{};
    };
private:
    struct ThreadBinding {
        lua_State* actual{};
        int reference{-2};
        std::uint64_t adoption_epoch{};
        bool busy{};
        bool failed{};
    };
    struct VmBinding {
        VmHandle vm;
        lua_State* root;
        LuaReferences references;
        bool observer_bound{};
        std::vector<std::shared_ptr<ThreadBinding>> threads;
        VmBinding(Lifecycle& lifecycle, VmHandle handle, lua_State* state)
            : vm(handle), root(state), references(lifecycle, handle, state) {}
    };
    struct ModuleImage { ModuleHandle module; void* image; };
    std::unordered_map<std::uint64_t, ModuleImage> module_images_;
    VmObserver vm_observer_{};
    Lifecycle lifecycle_;
    HostApiRuntime api_{lifecycle_};
    std::atomic<void (*)(ModuleHandle)> module_revoker_{};
    std::atomic<void (*)(VmHandle)> vm_revoker_{};
    mutable std::mutex bindings_mutex_;
    std::unordered_map<lua_State*, std::shared_ptr<VmBinding>> roots_;
    std::shared_ptr<VmBinding> binding(VmHandle) const;

public:
    static HostBindings& instance();
    Lifecycle& lifecycle() noexcept { return lifecycle_; }
    HostApiRuntime& api() noexcept { return api_; }
    bool install_vm_observer(const VmObserver&) noexcept;
    Result<VmHandle> attach_vm(lua_State* actual_host_root);
    Result<ModuleHandle> attach_module(void* actual_loaded_image = nullptr);
    Result<ModuleHandle> module_for_image(void* actual_loaded_image) const;
    Result<LuaStackBinding> stack_binding(lua_State* actual_root, lua_State* actual_stack,
        int (*trampoline)(lua_State*) = nullptr, std::uint32_t first = 1, std::uint32_t count = 0) const;
    // Actual dedicated coroutine pool: rooted through VM cleanup; nested
    // callbacks reserve distinct stacks. No root-as-callback-stack fallback.
    ue4ss_lua_host_status dispatch_callback_lua(VmHandle, ModuleHandle,
        const std::function<ue4ss_lua_host_status(ue4ss_lua_context_handle)>&);
    // Host code uses only the actual VM lease/gate; it grants no Native owner.
    Status dispatch_host_callback_lua(VmHandle, void*, void (*)(void*));
    Status dispatch_host_callback_lua_on_stack(VmHandle,lua_State*,void*,void (*)(void*));
    Result<VmHandle> find_vm(lua_State* actual_host_root) const;
    bool vm_is_closing(VmHandle) const;
    bool module_is_retiring(ModuleHandle) const;
    bool install_module_revoker(void (*)(ModuleHandle)) noexcept;
    bool install_vm_revoker(void (*)(VmHandle)) noexcept;
    // Claim or resume the same thread's exclusive finalization; never wait.
    Status claim_vm_close(VmHandle);
    Status claim_module_retire(ModuleHandle);
    Status cleanup_vm(VmHandle);
    Status cleanup_module(ModuleHandle);
    // finish_vm_close is called only AFTER the real lua_close has returned.
    Status finish_vm_close(VmHandle);
    // finish_module_retire is called only AFTER consumer destruction/unload.
    Status finish_module_retire(ModuleHandle);
    std::size_t vm_count() const;
};
}
