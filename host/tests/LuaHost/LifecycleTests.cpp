#include "TestSupport.hpp"

#include <LuaHost/Lifecycle.hpp>

#include <array>
#include <barrier>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

using namespace RC::LuaHost;

namespace
{
    static_assert(sizeof(VmHandle) == 2 * sizeof(std::uint64_t));
    static_assert(sizeof(ModuleHandle) == 2 * sizeof(std::uint64_t));
    static_assert(sizeof(ContextHandle) == 2 * sizeof(std::uint64_t));
    static_assert(sizeof(ReferenceHandle) == 2 * sizeof(std::uint64_t));
    static_assert(std::is_trivially_copyable_v<VmHandle>);
    static_assert(std::is_trivially_copyable_v<ModuleHandle>);
    static_assert(std::is_trivially_copyable_v<ContextHandle>);
    static_assert(std::is_trivially_copyable_v<ReferenceHandle>);
    static_assert(!std::is_copy_constructible_v<CallbackLease>);
    static_assert(!std::is_copy_assignable_v<CallbackLease>);
    static_assert(!std::is_copy_constructible_v<ScopeLease>);
    static_assert(!std::is_copy_assignable_v<ScopeLease>);
    static_assert(std::is_nothrow_move_constructible_v<CallbackLease>);
    static_assert(std::is_nothrow_move_assignable_v<CallbackLease>);
    static_assert(std::is_nothrow_move_constructible_v<ScopeLease>);
    static_assert(std::is_nothrow_move_assignable_v<ScopeLease>);

    bool rejects(Status status)
    {
        return status != Status::ok && status != Status::ready;
    }

    VmHandle live_vm(Lifecycle& host)
    {
        auto result = host.create_vm();
        CHECK(result.status == Status::ok);
        CHECK(result.value.id != 0);
        CHECK(result.value.generation != 0);
        CHECK(host.publish_vm(result.value) == Status::ok);
        return result.value;
    }

    ModuleHandle live_module(Lifecycle& host)
    {
        auto result = host.create_module();
        CHECK(result.status == Status::ok);
        CHECK(result.value.id != 0);
        CHECK(result.value.generation != 0);
        CHECK(host.publish_module(result.value) == Status::ok);
        return result.value;
    }

    CallbackLease callback(Lifecycle& host, ModuleHandle module)
    {
        auto result = host.enter_callback(module);
        CHECK(result.status == Status::ok);
        CHECK(result.value);
        CHECK(result.value.module() == module);
        return std::move(result.value);
    }

    ScopeLease scope(Lifecycle& host, VmHandle vm, const CallbackLease& entry)
    {
        auto result = host.enter_scope(vm, entry);
        CHECK(result.status == Status::ok);
        CHECK(result.value);
        CHECK(result.value.vm() == vm);
        CHECK(result.value.module() == entry.module());
        CHECK(host.validate_context(result.value.handle(), entry) == Status::ok);
        return std::move(result.value);
    }

    void abi_prefix_validation()
    {
        constexpr auto prefix = static_cast<std::uint32_t>(sizeof(ue4ss_uikit_lua_host_api_v1));
        CHECK(ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, prefix, prefix) == UE4SS_LUA_HOST_OK);
        CHECK(ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, prefix + 16, prefix) == UE4SS_LUA_HOST_OK);
        CHECK(ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION + 1, prefix, prefix) ==
              UE4SS_LUA_HOST_UNSUPPORTED_ABI);
        CHECK(ue4ss_lua_host_validate_prefix(UE4SS_UIKIT_LUA_HOST_ABI_VERSION, prefix - 1, prefix) ==
              UE4SS_LUA_HOST_UNSUPPORTED_SIZE);
    }

    void registry_rejects_unknown_ids_and_generations()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        auto entry = callback(host, module);
        auto active = scope(host, vm, entry);
        auto reference = host.create_reference(active);
        CHECK(reference.status == Status::ok);

        // An address-shaped integer is only an unknown registry key. No pointer
        // is supplied or dereferenced to validate these handles.
        constexpr std::uint64_t address_shaped_id = UINT64_C(0xfffffffffffffff0);
        CHECK(host.publish_vm({}) == Status::invalid_handle);
        CHECK(host.publish_module({}) == Status::invalid_handle);
        CHECK(host.enter_callback({address_shaped_id, 1}).status == Status::invalid_handle);
        CHECK(host.enter_scope({address_shaped_id, 1}, entry).status == Status::invalid_handle);
        CHECK(host.validate_context({address_shaped_id, 1}, entry) == Status::invalid_handle);
        CHECK(host.validate_reference({address_shaped_id, 1}, active) == Status::invalid_handle);
        CHECK(host.release_reference({address_shaped_id, 1}, active) == Status::invalid_handle);
        CHECK(host.request_vm_close({address_shaped_id, 1}) == Status::invalid_handle);
        CHECK(host.request_module_retire({address_shaped_id, 1}) == Status::invalid_handle);

        CHECK(host.publish_vm({vm.id, vm.generation + 1}) == Status::stale_generation);
        CHECK(host.publish_module({module.id, module.generation + 1}) == Status::stale_generation);
        CHECK(host.enter_callback({module.id, module.generation + 1}).status == Status::stale_generation);
        CHECK(host.enter_scope({vm.id, vm.generation + 1}, entry).status == Status::stale_generation);
        CHECK(host.validate_context({active.handle().id, active.handle().generation + 1}, entry) == Status::stale_generation);
        CHECK(host.validate_reference({reference.value.id, reference.value.generation + 1}, active) ==
              Status::stale_generation);
        CHECK(host.release_reference({reference.value.id, reference.value.generation + 1}, active) ==
              Status::stale_generation);
        CHECK(host.validate_reference(reference.value, active) == Status::ok);
    }

    void unpublished_records_reject_admission()
    {
        Lifecycle host;
        auto vm = host.create_vm();
        auto module = host.create_module();
        CHECK(vm.status == Status::ok);
        CHECK(module.status == Status::ok);
        CHECK(rejects(host.enter_callback(module.value).status));
        CHECK(host.publish_module(module.value) == Status::ok);
        auto entry = callback(host, module.value);
        CHECK(rejects(host.enter_scope(vm.value, entry).status));
        CHECK(host.publish_vm(vm.value) == Status::ok);
        auto active = scope(host, vm.value, entry);
        CHECK(host.validate_context(entry.handle(), entry) == Status::wrong_owner);
        CHECK(host.validate_context(active.handle(), entry) == Status::ok);
    }

    void contexts_bind_to_exact_callback_and_creator_thread()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        auto other_module = live_module(host);
        auto entry = callback(host, module);
        auto same_owner_entry = callback(host, module);
        auto other_entry = callback(host, other_module);
        auto active = scope(host, vm, entry);
        CHECK(host.validate_context(active.handle(), same_owner_entry) == Status::wrong_owner);
        CHECK(host.validate_context(active.handle(), other_entry) == Status::wrong_owner);

        Status context_status{};
        Status admission_status{};
        Status reference_status{};
        auto reference = host.create_reference(active);
        CHECK(reference.status == Status::ok);
        std::thread worker([&] {
            context_status = host.validate_context(active.handle(), entry);
            admission_status = host.enter_scope(vm, entry).status;
            reference_status = host.validate_reference(reference.value, active);
        });
        worker.join();
        CHECK(context_status == Status::wrong_thread);
        CHECK(admission_status == Status::wrong_thread);
        CHECK(reference_status == Status::wrong_thread);
        CHECK(host.validate_context(active.handle(), entry) == Status::ok);
    }

    void leases_from_another_registry_reject_authority()
    {
        Lifecycle first;
        Lifecycle second;
        auto vm = live_vm(first);
        auto module = live_module(first);
        auto foreign_vm = live_vm(second);
        auto foreign_module = live_module(second);
        auto entry = callback(first, module);
        auto foreign_entry = callback(second, foreign_module);
        auto active = scope(first, vm, entry);
        auto foreign_scope = scope(second, foreign_vm, foreign_entry);
        auto reference = first.create_reference(active);
        CHECK(reference.status == Status::ok);
        CHECK(first.enter_scope(vm, foreign_entry).status == Status::wrong_owner);
        CHECK(first.validate_context(active.handle(), foreign_entry) == Status::wrong_owner);
        CHECK(first.create_reference(foreign_scope).status == Status::wrong_owner);
        CHECK(first.validate_reference(reference.value, foreign_scope) == Status::wrong_owner);
        CHECK(first.release_reference(reference.value, foreign_scope) == Status::wrong_owner);
        CHECK(first.validate_reference(reference.value, active) == Status::ok);
    }

    void references_bind_to_module_and_vm_and_release_once()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto other_vm = live_vm(host);
        auto module = live_module(host);
        auto other_module = live_module(host);
        auto entry = callback(host, module);
        auto other_entry = callback(host, other_module);
        auto active = scope(host, vm, entry);
        auto reference = host.create_reference(active);
        CHECK(reference.status == Status::ok);
        auto other_vm_scope = scope(host, other_vm, entry);
        auto other_module_scope = scope(host, vm, other_entry);
        CHECK(host.validate_reference(reference.value, other_vm_scope) == Status::wrong_owner);
        CHECK(host.validate_reference(reference.value, other_module_scope) == Status::wrong_owner);
        CHECK(host.release_reference(reference.value, other_vm_scope) == Status::wrong_owner);
        CHECK(host.release_reference(reference.value, other_module_scope) == Status::wrong_owner);
        CHECK(host.validate_reference(reference.value, active) == Status::ok);

        active.reset();
        auto later_entry = callback(host, module);
        auto later_scope = scope(host, vm, later_entry);
        CHECK(host.validate_reference(reference.value, later_scope) == Status::ok);
        CHECK(host.release_reference(reference.value, later_scope) == Status::ok);
        CHECK(rejects(host.validate_reference(reference.value, later_scope)));
        CHECK(rejects(host.release_reference(reference.value, later_scope)));
    }

    void closing_rejects_new_vm_scope_and_normal_use()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        auto entry = callback(host, module);
        auto active = scope(host, vm, entry);
        auto reference = host.create_reference(active);
        CHECK(reference.status == Status::ok);
        CHECK(host.request_vm_close(vm) == Status::pending);
        CHECK(host.enter_scope(vm, entry).status == Status::retired);
        CHECK(host.validate_context(active.handle(), entry) == Status::retired);
        CHECK(host.validate_reference(reference.value, active) == Status::retired);
        CHECK(host.create_reference(active).status == Status::retired);
        CHECK(host.claim_vm_finalization(vm) == Status::pending);
        CHECK(rejects(host.finish_vm_finalization(vm)));
        active.reset();
        CHECK(host.claim_vm_finalization(vm) == Status::ready);
        CHECK(host.finish_vm_finalization(vm) == Status::ok);
    }

    void retiring_rejects_new_module_entry_and_normal_use()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        auto entry = callback(host, module);
        auto active = scope(host, vm, entry);
        CHECK(host.request_module_retire(module) == Status::pending);
        CHECK(host.enter_callback(module).status == Status::retired);
        CHECK(host.enter_scope(vm, entry).status == Status::retired);
        CHECK(host.validate_context(active.handle(), entry) == Status::retired);
        CHECK(host.claim_module_finalization(module) == Status::pending);
        CHECK(rejects(host.finish_module_finalization(module)));
        active.reset();
        CHECK(host.claim_module_finalization(module) == Status::pending);
        entry.reset();
        CHECK(host.claim_module_finalization(module) == Status::ready);
        CHECK(host.finish_module_finalization(module) == Status::ok);
    }

    void reentrant_close_and_retire_defer_without_waiting()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        auto entry = callback(host, module);
        auto active = scope(host, vm, entry);
        // These calls run on the same thread that owns the pins. A blocking
        // drain would deadlock here and the external test timeout would fail.
        CHECK(host.request_vm_close(vm) == Status::pending);
        CHECK(host.request_module_retire(module) == Status::pending);
        CHECK(host.claim_vm_finalization(vm) == Status::pending);
        CHECK(host.claim_module_finalization(module) == Status::pending);
        active.reset();
        CHECK(host.claim_vm_finalization(vm) == Status::ready);
        CHECK(host.claim_module_finalization(module) == Status::pending);
        CHECK(host.finish_vm_finalization(vm) == Status::ok);
        entry.reset();
        CHECK(host.claim_module_finalization(module) == Status::ready);
        CHECK(host.finish_module_finalization(module) == Status::ok);
    }

    void finalization_requires_drain_and_has_one_claim()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        CHECK(rejects(host.claim_vm_finalization(vm)));
        CHECK(rejects(host.claim_module_finalization(module)));
        CHECK(rejects(host.finish_vm_finalization(vm)));
        CHECK(rejects(host.finish_module_finalization(module)));
        CHECK(host.request_vm_close(vm) == Status::ready);
        CHECK(host.request_module_retire(module) == Status::ready);
        CHECK(host.claim_vm_finalization(vm) == Status::ready);
        CHECK(host.claim_vm_finalization(vm) == Status::busy);
        CHECK(host.claim_module_finalization(module) == Status::ready);
        CHECK(host.claim_module_finalization(module) == Status::busy);
        CHECK(host.finish_vm_finalization(vm) == Status::ok);
        CHECK(host.finish_module_finalization(module) == Status::ok);
        CHECK(host.finish_vm_finalization(vm) == Status::stale_generation);
        CHECK(host.finish_module_finalization(module) == Status::stale_generation);
        CHECK(host.request_vm_close(vm) == Status::stale_generation);
        CHECK(host.request_module_retire(module) == Status::stale_generation);
        CHECK(host.enter_callback(module).status == Status::stale_generation);

        auto next_vm = live_vm(host);
        auto next_module = live_module(host);
        CHECK(next_vm.id != vm.id || next_vm.generation != vm.generation);
        CHECK(next_module.id != module.id || next_module.generation != module.generation);
        auto entry = callback(host, next_module);
        CHECK(host.enter_scope(vm, entry).status == Status::stale_generation);
        auto active = scope(host, next_vm, entry);
    }

    void destructors_release_each_pin()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        {
            auto entry = callback(host, module);
            {
                auto active = scope(host, vm, entry);
                CHECK(host.request_vm_close(vm) == Status::pending);
                CHECK(host.request_module_retire(module) == Status::pending);
            }
            CHECK(host.claim_vm_finalization(vm) == Status::ready);
            CHECK(host.claim_module_finalization(module) == Status::pending);
        }
        CHECK(host.claim_module_finalization(module) == Status::ready);
        CHECK(host.finish_vm_finalization(vm) == Status::ok);
        CHECK(host.finish_module_finalization(module) == Status::ok);
    }

    void moves_transfer_pins_and_assignment_releases_destination()
    {
        Lifecycle host;
        auto first_vm = live_vm(host);
        auto second_vm = live_vm(host);
        auto first_module = live_module(host);
        auto second_module = live_module(host);
        auto first_entry = callback(host, first_module);
        auto second_entry = callback(host, second_module);
        auto first_scope = scope(host, first_vm, first_entry);
        auto second_scope = scope(host, second_vm, first_entry);
        auto retained_context = first_scope.handle();

        ScopeLease moved_scope{std::move(first_scope)};
        CHECK(!first_scope);
        CHECK(first_scope.handle() == ContextHandle{});
        CHECK(first_scope.vm() == VmHandle{});
        CHECK(first_scope.module() == ModuleHandle{});
        first_scope.reset();
        CHECK(host.validate_context(retained_context, first_entry) == Status::ok);
        second_scope = std::move(moved_scope);
        CHECK(!moved_scope);
        CHECK(second_scope.handle() == retained_context);
        CHECK(host.request_vm_close(second_vm) == Status::ready);
        CHECK(host.request_vm_close(first_vm) == Status::pending);

        auto retained_entry_handle = first_entry.handle();
        CallbackLease moved_entry{std::move(first_entry)};
        CHECK(!first_entry);
        CHECK(first_entry.handle() == ContextHandle{});
        CHECK(first_entry.module() == ModuleHandle{});
        first_entry.reset();
        CHECK(host.validate_context(retained_context, moved_entry) == Status::retired);
        second_entry = std::move(moved_entry);
        CHECK(!moved_entry);
        CHECK(second_entry.handle() == retained_entry_handle);
        CHECK(host.request_module_retire(second_module) == Status::ready);
        CHECK(host.request_module_retire(first_module) == Status::pending);
        second_entry.reset();
        second_entry.reset();
        CHECK(host.claim_module_finalization(first_module) == Status::pending);
        second_scope.reset();
        second_scope.reset();
        CHECK(host.claim_vm_finalization(first_vm) == Status::ready);
        CHECK(host.claim_module_finalization(first_module) == Status::ready);
    }

    void released_contexts_and_expired_callbacks_cannot_be_reused()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        auto entry = callback(host, module);
        auto active = scope(host, vm, entry);
        auto expired_context = active.handle();
        active.reset();
        CHECK(rejects(host.validate_context(expired_context, entry)));
        CHECK(rejects(host.create_reference(active).status));
        auto fresh = scope(host, vm, entry);
        CHECK(fresh.handle() != expired_context);
        CHECK(rejects(host.validate_context(expired_context, entry)));

        auto reference = host.create_reference(fresh);
        CHECK(reference.status == Status::ok);
        auto retained_context = fresh.handle();
        entry.reset();
        CHECK(rejects(host.validate_context(retained_context, entry)));
        CHECK(rejects(host.create_reference(fresh).status));
        CHECK(rejects(host.validate_reference(reference.value, fresh)));
        auto replacement_entry = callback(host, module);
        CHECK(rejects(host.validate_context(retained_context, replacement_entry)));
        CHECK(host.request_module_retire(module) == Status::pending);
        replacement_entry.reset();
        CHECK(host.claim_module_finalization(module) == Status::pending);
        fresh.reset();
        CHECK(host.claim_module_finalization(module) == Status::ready);
    }

    void concurrent_first_registration_has_unique_handles()
    {
        constexpr std::size_t workers = 8;
        Lifecycle host;
        std::barrier start{static_cast<std::ptrdiff_t>(workers)};
        std::array<Result<VmHandle>, workers> vms{};
        std::array<Result<ModuleHandle>, workers> modules{};
        std::array<Status, workers> vm_publication{};
        std::array<Status, workers> module_publication{};
        std::array<std::thread, workers> threads;
        for (std::size_t index = 0; index < workers; ++index)
        {
            threads[index] = std::thread([&, index] {
                start.arrive_and_wait();
                vms[index] = host.create_vm();
                modules[index] = host.create_module();
                vm_publication[index] = host.publish_vm(vms[index].value);
                module_publication[index] = host.publish_module(modules[index].value);
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        for (std::size_t first = 0; first < workers; ++first)
        {
            CHECK(vms[first].status == Status::ok);
            CHECK(modules[first].status == Status::ok);
            CHECK(vm_publication[first] == Status::ok);
            CHECK(module_publication[first] == Status::ok);
            for (std::size_t second = first + 1; second < workers; ++second)
            {
                CHECK(vms[first].value != vms[second].value);
                CHECK(modules[first].value != modules[second].value);
            }
        }
    }

    void publication_racing_retirement_cannot_reopen_records()
    {
        // Exercise both deterministic histories before the simultaneous starts.
        {
            Lifecycle host;
            auto vm = host.create_vm();
            auto module = host.create_module();
            CHECK(host.request_vm_close(vm.value) == Status::ready);
            CHECK(host.request_module_retire(module.value) == Status::ready);
            CHECK(host.publish_vm(vm.value) == Status::retired);
            CHECK(host.publish_module(module.value) == Status::retired);
        }
        {
            Lifecycle host;
            auto vm = live_vm(host);
            auto module = live_module(host);
            CHECK(host.request_vm_close(vm) == Status::ready);
            CHECK(host.request_module_retire(module) == Status::ready);
            CHECK(host.publish_vm(vm) == Status::retired);
            CHECK(host.publish_module(module) == Status::retired);
        }
        constexpr std::size_t rounds = 64;
        for (std::size_t round = 0; round < rounds; ++round)
        {
            Lifecycle host;
            auto vm = host.create_vm();
            auto module = host.create_module();
            CHECK(vm.status == Status::ok);
            CHECK(module.status == Status::ok);
            std::barrier start{2};
            Status vm_publication{}, module_publication{}, vm_close{}, module_retire{};
            std::thread publish([&] {
                start.arrive_and_wait();
                vm_publication = host.publish_vm(vm.value);
                module_publication = host.publish_module(module.value);
            });
            std::thread retire([&] {
                start.arrive_and_wait();
                vm_close = host.request_vm_close(vm.value);
                module_retire = host.request_module_retire(module.value);
            });
            publish.join();
            retire.join();
            CHECK(vm_publication == Status::ok || vm_publication == Status::retired);
            CHECK(module_publication == Status::ok || module_publication == Status::retired);
            CHECK(vm_close == Status::ready);
            CHECK(module_retire == Status::ready);
            CHECK(host.publish_vm(vm.value) == Status::retired);
            CHECK(host.publish_module(module.value) == Status::retired);
            CHECK(host.enter_callback(module.value).status == Status::retired);
            CHECK(host.claim_vm_finalization(vm.value) == Status::ready);
            CHECK(host.claim_module_finalization(module.value) == Status::ready);
            CHECK(host.finish_vm_finalization(vm.value) == Status::ok);
            CHECK(host.finish_module_finalization(module.value) == Status::ok);
        }
    }

    void concurrent_callback_admission_racing_retire_preserves_pin()
    {
        constexpr std::size_t rounds = 64;
        for (std::size_t round = 0; round < rounds; ++round)
        {
            Lifecycle host;
            auto module = live_module(host);
            std::barrier start{2};
            std::barrier admitted{2};
            std::barrier release{2};
            Status entry_status{}, retire_status{}, claim_while_held{};
            std::thread admit([&] {
                start.arrive_and_wait();
                auto entry = host.enter_callback(module);
                entry_status = entry.status;
                admitted.arrive_and_wait();
                release.arrive_and_wait();
            });
            start.arrive_and_wait();
            retire_status = host.request_module_retire(module);
            admitted.arrive_and_wait();
            claim_while_held = host.claim_module_finalization(module);
            release.arrive_and_wait();
            admit.join();
            CHECK(entry_status == Status::ok || entry_status == Status::retired);
            if (entry_status == Status::ok)
            {
                CHECK(retire_status == Status::pending);
                CHECK(claim_while_held == Status::pending);
                CHECK(host.claim_module_finalization(module) == Status::ready);
            }
            else
            {
                CHECK(retire_status == Status::ready);
                CHECK(claim_while_held == Status::ready);
                CHECK(host.claim_module_finalization(module) == Status::busy);
            }
            CHECK(host.finish_module_finalization(module) == Status::ok);
        }
    }

    void concurrent_finalization_has_exactly_one_claimant()
    {
        constexpr std::size_t workers = 8;
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        CHECK(host.request_vm_close(vm) == Status::ready);
        CHECK(host.request_module_retire(module) == Status::ready);
        std::barrier start{static_cast<std::ptrdiff_t>(workers)};
        std::barrier claims_done{static_cast<std::ptrdiff_t>(workers)};
        std::array<Status, workers> vm_claims{}, module_claims{};
        std::array<Status, workers> vm_finishes{}, module_finishes{};
        std::array<std::thread, workers> threads;
        for (std::size_t index = 0; index < workers; ++index)
        {
            threads[index] = std::thread([&, index] {
                start.arrive_and_wait();
                vm_claims[index] = host.claim_vm_finalization(vm);
                module_claims[index] = host.claim_module_finalization(module);
                claims_done.arrive_and_wait();
                vm_finishes[index] = vm_claims[index] == Status::ready ? host.finish_vm_finalization(vm) : Status::busy;
                module_finishes[index] = module_claims[index] == Status::ready ? host.finish_module_finalization(module) : Status::busy;
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
        std::size_t vm_winners{}, module_winners{};
        for (std::size_t index = 0; index < workers; ++index)
        {
            CHECK(vm_claims[index] == Status::ready || vm_claims[index] == Status::busy);
            CHECK(module_claims[index] == Status::ready || module_claims[index] == Status::busy);
            CHECK(vm_finishes[index] == (vm_claims[index] == Status::ready ? Status::ok : Status::busy));
            CHECK(module_finishes[index] == (module_claims[index] == Status::ready ? Status::ok : Status::busy));
            vm_winners += vm_claims[index] == Status::ready;
            module_winners += module_claims[index] == Status::ready;
        }
        CHECK(vm_winners == 1);
        CHECK(module_winners == 1);
        CHECK(host.finish_vm_finalization(vm) == Status::stale_generation);
        CHECK(host.finish_module_finalization(module) == Status::stale_generation);
    }

    void concurrent_scope_admission_racing_close_preserves_pin()
    {
        constexpr std::size_t rounds = 64;
        for (std::size_t round = 0; round < rounds; ++round)
        {
            Lifecycle host;
            auto vm = live_vm(host);
            auto module = live_module(host);
            std::barrier start{2};
            std::barrier admitted{2};
            std::barrier release{2};
            Status entry_status{}, scope_status{}, close_status{}, claim_while_held{};
            std::thread admit([&] {
                auto entry = host.enter_callback(module);
                entry_status = entry.status;
                start.arrive_and_wait();
                auto active = host.enter_scope(vm, entry.value);
                scope_status = active.status;
                admitted.arrive_and_wait();
                release.arrive_and_wait();
            });
            start.arrive_and_wait();
            close_status = host.request_vm_close(vm);
            admitted.arrive_and_wait();
            claim_while_held = host.claim_vm_finalization(vm);
            release.arrive_and_wait();
            admit.join();
            CHECK(entry_status == Status::ok);
            CHECK(scope_status == Status::ok || scope_status == Status::retired);
            if (scope_status == Status::ok)
            {
                CHECK(close_status == Status::pending);
                CHECK(claim_while_held == Status::pending);
                CHECK(host.claim_vm_finalization(vm) == Status::ready);
            }
            else
            {
                CHECK(close_status == Status::ready);
                CHECK(claim_while_held == Status::ready);
                CHECK(host.claim_vm_finalization(vm) == Status::busy);
            }
            CHECK(host.finish_vm_finalization(vm) == Status::ok);
        }
    }

    void finalization_is_owned_by_claiming_thread()
    {
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        CHECK(host.request_vm_close(vm) == Status::ready);
        CHECK(host.request_module_retire(module) == Status::ready);
        CHECK(host.claim_vm_finalization(vm) == Status::ready);
        CHECK(host.claim_module_finalization(module) == Status::ready);
        Status vm_finish{}, module_finish{};
        std::thread wrong_owner([&] {
            vm_finish = host.finish_vm_finalization(vm);
            module_finish = host.finish_module_finalization(module);
        });
        wrong_owner.join();
        CHECK(vm_finish == Status::wrong_thread);
        CHECK(module_finish == Status::wrong_thread);
        CHECK(host.claim_vm_finalization(vm) == Status::busy);
        CHECK(host.claim_module_finalization(module) == Status::busy);
        CHECK(host.finish_vm_finalization(vm) == Status::ok);
        CHECK(host.finish_module_finalization(module) == Status::ok);
    }

    void shutdown_rejects_admission_and_requires_finished_records()
    {
        {
            Lifecycle empty;
            CHECK(empty.host_state() == HostState::running);
            CHECK(empty.finish_shutdown() == Status::retired);
            CHECK(empty.begin_shutdown() == Status::ready);
            CHECK(empty.host_state() == HostState::stopping);
            CHECK(empty.finish_shutdown() == Status::ok);
            CHECK(empty.host_state() == HostState::stopped);
            CHECK(empty.begin_shutdown() == Status::retired);
            CHECK(empty.finish_shutdown() == Status::retired);
            CHECK(empty.create_vm().status == Status::retired);
            CHECK(empty.create_module().status == Status::retired);
        }
        Lifecycle host;
        auto vm = live_vm(host);
        auto module = live_module(host);
        auto entry = callback(host, module);
        auto active = scope(host, vm, entry);
        auto reference = host.create_reference(active);
        CHECK(reference.status == Status::ok);
        CHECK(host.begin_shutdown() == Status::pending);
        CHECK(host.host_state() == HostState::stopping);
        CHECK(host.create_vm().status == Status::retired);
        CHECK(host.create_module().status == Status::retired);
        CHECK(host.publish_vm(vm) == Status::retired);
        CHECK(host.publish_module(module) == Status::retired);
        CHECK(host.enter_callback(module).status == Status::retired);
        CHECK(host.enter_scope(vm, entry).status == Status::retired);
        CHECK(host.validate_context(active.handle(), entry) == Status::retired);
        CHECK(host.validate_reference(reference.value, active) == Status::retired);
        CHECK(host.finish_shutdown() == Status::pending);
        CHECK(host.claim_vm_finalization(vm) == Status::pending);
        CHECK(host.claim_module_finalization(module) == Status::pending);
        active.reset();
        entry.reset();
        CHECK(host.finish_shutdown() == Status::pending);
        CHECK(host.claim_vm_finalization(vm) == Status::ready);
        CHECK(host.finish_vm_finalization(vm) == Status::ok);
        CHECK(host.finish_shutdown() == Status::pending);
        CHECK(host.claim_module_finalization(module) == Status::ready);
        CHECK(host.finish_module_finalization(module) == Status::ok);
        CHECK(host.begin_shutdown() == Status::ready);
        CHECK(host.finish_shutdown() == Status::ok);
        CHECK(host.host_state() == HostState::stopped);
        CHECK(host.create_vm().status == Status::retired);
        CHECK(host.create_module().status == Status::retired);
    }
} // namespace

int main(int argc, char** argv)
{
    return LuaHostTests::run({
        {"ABI prefix validation", abi_prefix_validation},
        {"unknown registry IDs and stale generations", registry_rejects_unknown_ids_and_generations},
        {"unpublished records reject admission", unpublished_records_reject_admission},
        {"exact callback and creator thread binding", contexts_bind_to_exact_callback_and_creator_thread},
        {"cross-registry lease rejection", leases_from_another_registry_reject_authority},
        {"reference module/VM binding and release", references_bind_to_module_and_vm_and_release_once},
        {"closing VM rejects admission and use", closing_rejects_new_vm_scope_and_normal_use},
        {"retiring module rejects admission and use", retiring_rejects_new_module_entry_and_normal_use},
        {"reentrant close/retire returns Pending", reentrant_close_and_retire_defer_without_waiting},
        {"drained finalization and one claim", finalization_requires_drain_and_has_one_claim},
        {"destructors release pins", destructors_release_each_pin},
        {"move/reset lease ownership", moves_transfer_pins_and_assignment_releases_destination},
        {"released context and callback invalidation", released_contexts_and_expired_callbacks_cannot_be_reused},
        {"concurrent first registrations", concurrent_first_registration_has_unique_handles},
        {"publication/retirement race", publication_racing_retirement_cannot_reopen_records},
        {"callback admission/retirement race", concurrent_callback_admission_racing_retire_preserves_pin},
        {"scope admission/close race", concurrent_scope_admission_racing_close_preserves_pin},
        {"concurrent finalization claims", concurrent_finalization_has_exactly_one_claimant},
        {"finalizer creator thread binding", finalization_is_owned_by_claiming_thread},
        {"host shutdown drains finalized records", shutdown_rejects_admission_and_requires_finished_records},
    }, argc, argv);
}
