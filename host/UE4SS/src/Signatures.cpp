#include <filesystem>

#include <UE4SSProgram.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <LuaLibrary.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaMadeSimple/HostExecutionHooks.hpp>
#include <LuaMadeSimple/HostThreadAccess.hpp>
#include <LuaHost/HostBindings.hpp>
#include <memory>
#include <optional>
#include <SigScanner/SinglePassSigScanner.hpp>
#include <Signatures.hpp>
#include <ExceptionHandling.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Script.hpp>
#include <Unreal/Signatures.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectHashTables.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UnrealInitializer.hpp>

#include <Helpers/String.hpp>
extern "C" void LuaLock(lua_State*);
extern "C" void LuaUnlock(lua_State*);

// Required for SEH_TRY and SEH_EXCEPT.
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace RC
{
    namespace {
        // A scan retains its actual host-created VM until all scanner workers
        // complete. Destruction performs pure bookkeeping only.
        struct SignatureLuaVm {
            const LuaMadeSimple::Lua* lua{};
            lua_State* root{};
            LuaHost::VmHandle vm{};
            bool closed{};
            void close() {
                if (closed) return;
                auto& host=LuaHost::HostBindings::instance();
                if(host.claim_vm_close(vm)!=LuaHost::Status::ready)
                    throw std::runtime_error("Signature VM still executing at scan completion");
                LuaMadeSimple::LuaFinalizationScope finalizing{root,root};
                if(host.cleanup_vm(vm)!=LuaHost::Status::ok)
                    throw std::runtime_error("Signature VM cleanup failed");
                lua_close(root);
                finalizing.close_completed();
                if(host.finish_vm_close(vm)!=LuaHost::Status::ok)
                    throw std::runtime_error("Signature VM metadata finalization failed");
                LuaMadeSimple::forget_closed_host_vm(root);
                closed=true;
            }
        };
    }
    auto scan_complete_default_func(DidLuaScanSucceed) -> void
    {
    }

    auto scan_from_lua_script(std::filesystem::path& script_file_path_and_name,
                              std::vector<SignatureContainer>& signature_containers,
                              LuaScriptMatchFoundFunc& match_found_func,
                              LuaScriptScanCompleteFunc& scan_complete_func) -> void
    {
        auto owned=std::make_shared<SignatureLuaVm>();
        owned->lua=&LuaMadeSimple::new_state();
        owned->root=owned->lua->get_lua_state();
        const auto binding=LuaHost::HostBindings::instance().attach_vm(owned->root);
        owned->vm=binding.value;
        if(binding.status!=LuaHost::Status::ok) {
            if(owned->vm.id) owned->close();
            else {
                // No host record was published and no library/userdata setup
                // ran. Keep the real gate through close and cache invalidation.
                LuaLock(nullptr);
                lua_close(owned->root);
                LuaMadeSimple::forget_closed_host_vm(owned->root);
                LuaUnlock(nullptr);
                owned->closed=true;
            }
            throw std::runtime_error("Signature VM ownership binding failed");
        }
        const LuaMadeSimple::Lua& lua=*owned->lua;
        try {
        std::optional<void*> immediate;
        std::string signature;
        constexpr const char* global_register_func_name = "Register";
        constexpr const char* global_on_match_found_func_name = "OnMatchFound";
        {
        LuaMadeSimple::LuaExecutionScope execution{owned->root,owned->root};

        lua.open_all_libs();
        lua.register_function("Print", LuaLibrary::global_print);
        lua.register_function("print", LuaLibrary::global_print);
        lua.register_function("DerefToInt32", LuaLibrary::deref_to_int32);
        lua.register_function("dereftoint32", LuaLibrary::deref_to_int32);
        lua.register_function("LoadExport", LuaLibrary::load_export);
        lua.register_function("loadexport", LuaLibrary::load_export);

        lua.execute_file(script_file_path_and_name.string());

        if (lua.get_stack_size() > 0)
        {
            if (lua.is_integer())
            {
                auto found_address = reinterpret_cast<void*>(lua.get_integer());
                immediate=found_address;
            }
            else if (lua.is_nil())
            {
                throw std::runtime_error{fmt::format("Lua file returned nil (symbol not found): {}", to_string(script_file_path_and_name))};
            }
        }

        if(!immediate.has_value()) {
        if (!lua.is_global_function(global_register_func_name) || !lua.is_global_function(global_on_match_found_func_name))
        {
            Output::send(STR("Lua functions 'Register' and 'OnMatchFound' must be "
                             "present in {}\n"),
                         ensure_str(script_file_path_and_name));
            throw std::runtime_error{"See error message above"};
        }

        lua.call_function(global_register_func_name, 0, 1);

        if (!lua.is_string())
        {
            throw std::runtime_error{"Lua function 'Register' must return a string "
                                     "that contains the signature to scan for"};
        }
        signature=std::string(lua.get_string()); // copy while actual VM/gate is pinned
        }
        }
        if(immediate.has_value()) {
            owned->close();
            match_found_func(*immediate);
            return;
        }

        signature_containers.emplace_back(SignatureContainer{{
                                                                     {signature},
                                                             },
                                                             // On Match Found
                                                             [owned, match_found_func](SignatureContainer& self) -> bool {
                                                                 if(owned->closed) return false;
                                                                 LuaMadeSimple::LuaExecutionScope execution{owned->root,owned->root};
                                                                 const auto& lua=*owned->lua;
                                                                 lua.prepare_function_call(global_on_match_found_func_name);
                                                                 lua.set_integer(reinterpret_cast<uintptr_t>(static_cast<void*>(self.get_match_address())));
                                                                 lua.call_function(1, 1);

                                                                 if (!lua.is_integer())
                                                                 {
                                                                     return false;
                                                                 }

                                                                 void* found_address = reinterpret_cast<void*>(lua.get_integer());
                                                                 if (!found_address)
                                                                 {
                                                                     return false;
                                                                 }

                                                                 DidLuaScanSucceed did_lua_scan_succeed = match_found_func(found_address);

                                                                 if (did_lua_scan_succeed == DidLuaScanSucceed::Yes)
                                                                 {
                                                                     self.get_did_succeed() = true;
                                                                     return true;
                                                                 }
                                                                 else
                                                                 {
                                                                     return false;
                                                                 }
                                                             },
                                                             // On Scan Completed
                                                             [owned, scan_complete_func]([[maybe_unused]] const SignatureContainer& self) {
                                                                 // Actual scanner waits all worker futures before this callback.
                                                                 owned->close();
                                                                 scan_complete_func(self.get_did_succeed() ? DidLuaScanSucceed::Yes : DidLuaScanSucceed::No);
                                                             }});
        } catch (...) {
            owned->close();
            throw;
        }
    }

    auto setup_lua_scan_overrides(std::filesystem::path& working_directory, Unreal::UnrealInitializer::Config& config) -> void
    {
        auto lua_guobjectarray_scan_script = working_directory / "UE4SS_Signatures/GUObjectArray.lua";
        if (std::filesystem::exists(lua_guobjectarray_scan_script))
        {
            config.ScanOverrides.guobjectarray = [lua_guobjectarray_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                                 Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_guobjectarray_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("GUObjectArray address: {} <- Lua Script\n"), address);
                            Unreal::UObjectArray::SetupGUObjectArrayAddress(address);
                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'GUObjectArray' via Lua script");
                            }
                        });
            };
        }

        auto lua_fts_scan_script = working_directory / "UE4SS_Signatures/FName_ToString.lua";
        if (std::filesystem::exists(lua_fts_scan_script))
        {
            config.ScanOverrides.fname_to_string = [lua_fts_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                         Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_fts_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("FName::ToString address: {} <- Lua Script\n"), address);
                            Unreal::FName::ToStringInternal.assign_address(address);
                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'FName::ToString' via Lua "
                                                                "script");
                            }
                        });
            };
        }

        auto lua_fnc_scan_script = working_directory / "UE4SS_Signatures/FName_Constructor.lua";
        if (std::filesystem::exists(lua_fnc_scan_script))
        {
            config.ScanOverrides.fname_constructor = [lua_fnc_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                           Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_fnc_scan_script,
                        signature_containers,
                        [&scan_result](void* address) {
                            if (!address)
                            {
                                scan_result.Errors.emplace_back("Lua script 'FName_Constructor.lua' returned an address of 0 for FName::FName.");
                                return DidLuaScanSucceed::No;
                            }
                            if (!Unreal::UEngine::TickInternal.is_ready())
                            {
                                scan_result.Errors.emplace_back("UGameEngine::Tick is a requirement when FName_Constructor.lua is present!\n"
                                                                "Please supply 'UE4SS_Signatures/GameEngineTick.lua'");
                                return DidLuaScanSucceed::No;
                            }
                            Unreal::UnrealInitializer::VerifyFNameConstructor(address);
                            Unreal::FName name{};
                            // ByteProperty is a hardcoded EName, registered by the name pool itself, so it is
                            // findable as soon as any FName exists. bCanBeDamaged only appears once AActor
                            // is registered, which made this fail and re-run the whole Lua scan phase
                            // dozens of times on games that reach this point early.
                            SEH_TRY({ name = Unreal::FName(STR("ByteProperty"), Unreal::FNAME_Find, address); })
                            SEH_EXCEPT({ Output::send<LogLevel::Error>(STR("Error: Crashed calling FName constructor.\n")); });

                            if (UE4SSProgram::settings_manager.General.DefaultFNameToStringMethod == Unreal::UnrealInitializer::FNameToStringMethod::Conv_NameToString)
                            {
                                Output::send(STR("Skipping confirmation of FName constructor address with a call to FName::ToString because FNameToStringMethod == Conv_NameToString\n"));
                                Output::send(STR("FName::FName address: {} <- Lua Script\n"), address);
                                Unreal::FName::ConstructorInternal.assign_address(address);
                                return DidLuaScanSucceed::Yes;
                            }

                            DidLuaScanSucceed did_succeed{};
                            SEH_TRY({
                                if (name == STR("ByteProperty"))
                                {
                                    Output::send(STR("FName::FName address: {} <- Lua Script\n"), address);
                                    Unreal::FName::ConstructorInternal.assign_address(address);
                                    did_succeed = DidLuaScanSucceed::Yes;
                                }
                                else
                                {
                                    scan_result.Errors.emplace_back("Lua script 'FName_Constructor.lua' did not return a "
                                                                    "valid address for FName::FName.");
                                    did_succeed = DidLuaScanSucceed::No;
                                }
                            })
                            SEH_EXCEPT({ Output::send<LogLevel::Error>(STR("Error: Crashed calling FName::ToString.\n")); })
                            return did_succeed;
                        },
                        [&scan_result]([[maybe_unused]] DidLuaScanSucceed did_lua_scan_succeed) {
                            if (!Unreal::FName::ConstructorInternal.get_function_address())
                            {
                                scan_result.Errors.emplace_back("Lua script 'FName_Constructor.lua' did not return a "
                                                                "valid address for FName::FName.");
                            }
                        });
            };
        }

        // For compatibility, we look for 'FMemory_Free.lua' if 'GMalloc.lua' doesn't
        // exist.
        auto lua_ffree_scan_script_new = working_directory / "UE4SS_Signatures/GMalloc.lua";
        auto lua_ffree_scan_script_compat = working_directory / "UE4SS_Signatures/FMemory_Free.lua";
        auto lua_ffree_scan_script = std::filesystem::exists(lua_ffree_scan_script_new) ? lua_ffree_scan_script_new : lua_ffree_scan_script_compat;
        if (std::filesystem::exists(lua_ffree_scan_script))
        {
            config.ScanOverrides.fmemory_free = [lua_ffree_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                        Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_ffree_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("GMalloc address: {} <- Lua Script\n"), address);
                            Unreal::GMalloc = static_cast<Unreal::FMalloc**>(address);
                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'GMalloc' via Lua script");
                            }
                        });
            };
        }

        auto lua_sco_scan_script = working_directory / "UE4SS_Signatures/StaticConstructObject.lua";
        if (std::filesystem::exists(lua_sco_scan_script))
        {
            config.ScanOverrides.static_construct_object = [lua_sco_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                                 Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_sco_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("StaticConstructObject_Internal address: {} "
                                             "<- Lua Script\n"),
                                         address);
                            Unreal::UObjectGlobals::SetupStaticConstructObjectInternalAddress(address);
                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'StaticConstructObject' via "
                                                                "Lua script");
                            }
                        });
            };
        }

        auto lua_guhashtables_scan_script = working_directory / "UE4SS_Signatures/GUObjectHashTables.lua";
        if (std::filesystem::exists(lua_guhashtables_scan_script))
        {
            config.ScanOverrides.fuobject_hash_tables_get = [lua_guhashtables_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                                           Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_guhashtables_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("GUObjectHashTables_Get address: {} <- Lua Script\n"), address);
                            Unreal::FUObjectHashTables::SetupGetAddress(address);
                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'GUObjectHashTables' via Lua "
                                                                "script");
                            }
                        });
            };
        }

        auto lua_gnatives_scan_script = working_directory / "UE4SS_Signatures/GNatives.lua";
        if (std::filesystem::exists(lua_gnatives_scan_script))
        {
            config.ScanOverrides.gnatives = [lua_gnatives_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                       Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_gnatives_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("GNatives address: {} <- Lua Script\n"), address);
                            Unreal::GNatives_Internal = reinterpret_cast<Unreal::FNativeFuncPtr*>(address);
                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'GNatives' via Lua script");
                            }
                        });
            };
        }

        auto lua_consolemanager_scan_script = working_directory / "UE4SS_Signatures/ConsoleManager.lua";
        if (std::filesystem::exists(lua_consolemanager_scan_script))
        {
            config.ScanOverrides.console_manager_singleton = [lua_consolemanager_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                                              Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_consolemanager_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("ConsoleManagerSingleton address: {} <- Lua Script\n"), address);

                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'ConsoleManagerSingleton' "
                                                                "via Lua script");
                            }
                        });
            };
        }

        auto lua_process_local_script_function_scan_script = working_directory / "UE4SS_Signatures/ProcessLocalScriptFunction.lua";
        if (std::filesystem::exists(lua_process_local_script_function_scan_script))
        {
            config.ScanOverrides.process_local_script_function =
                    [lua_process_local_script_function_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                    Unreal::Signatures::ScanResult& scan_result) mutable {
                        scan_from_lua_script(
                                lua_process_local_script_function_scan_script,
                                signature_containers,
                                [](void* address) {
                                    Output::send(STR("ProcessLocalScriptFunction address: {} <- "
                                                     "Lua Script\n"),
                                                 address);
                                    Unreal::UObject::ProcessLocalScriptFunctionInternal.assign_address(address);
                                    return DidLuaScanSucceed::Yes;
                                },
                                [&](DidLuaScanSucceed did_lua_scan_succeed) {
                                    if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                                    {
                                        scan_result.Errors.emplace_back("Was unable to find AOB for 'ProcessLocalScriptFunction' "
                                                                        "via Lua script");
                                    }
                                });
                    };
        }

        auto lua_process_internal_scan_script = working_directory / "UE4SS_Signatures/ProcessInternal.lua";
        if (std::filesystem::exists(lua_process_internal_scan_script))
        {
            config.ScanOverrides.process_internal = [lua_process_internal_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                                       Unreal::Signatures::ScanResult& scan_result) mutable {
                scan_from_lua_script(
                        lua_process_internal_scan_script,
                        signature_containers,
                        [](void* address) {
                            Output::send(STR("ProcessInternal address: {} <- Lua Script\n"), address);
                            Unreal::UObject::ProcessInternalInternal.assign_address(address);
                            return DidLuaScanSucceed::Yes;
                        },
                        [&](DidLuaScanSucceed did_lua_scan_succeed) {
                            if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                            {
                                scan_result.Errors.emplace_back("Was unable to find AOB for 'ProcessInternal' via Lua "
                                                                "script");
                            }
                        });
            };
        }

        auto lua_call_function_by_name_with_arguments_scan_script = working_directory / "UE4SS_Signatures/CallFunctionByNameWithArguments.lua";
        if (std::filesystem::exists(lua_call_function_by_name_with_arguments_scan_script))
        {
            config.ScanOverrides.call_function_by_name_with_arguments =
                    [lua_call_function_by_name_with_arguments_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                           Unreal::Signatures::ScanResult& scan_result) mutable {
                        scan_from_lua_script(
                                lua_call_function_by_name_with_arguments_scan_script,
                                signature_containers,
                                [](void* address) {
                                    Output::send(STR("CallFunctionByNameWithArguments address: {} "
                                                     "<- Lua Script\n"),
                                                 address);
                                    Unreal::UObject::CallFunctionByNameWithArgumentsInternal.assign_address(address);
                                    return DidLuaScanSucceed::Yes;
                                },
                                [&](DidLuaScanSucceed did_lua_scan_succeed) {
                                    if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                                    {
                                        scan_result.Errors.emplace_back("Was unable to find AOB for "
                                                                        "'CallFunctionByNameWithArguments' via Lua script");
                                    }
                                });
                    };
        }

        auto lua_gameengine_tick_scan_script = working_directory / "UE4SS_Signatures/GameEngineTick.lua";
        if (std::filesystem::exists(lua_gameengine_tick_scan_script))
        {
            config.ScanOverrides.gameengine_tick =
                    [lua_gameengine_tick_scan_script](std::vector<SignatureContainer>& signature_containers,
                                                                           Unreal::Signatures::ScanResult& scan_result) mutable {
                        scan_from_lua_script(
                                lua_gameengine_tick_scan_script,
                                signature_containers,
                                [](void* address) {
                                    Output::send(STR("GameEngine::Tick address: {} "
                                                     "<- Lua Script\n"),
                                                 address);
                                    Unreal::UEngine::TickInternal.assign_address(address);
                                    return DidLuaScanSucceed::Yes;
                                },
                                [&](DidLuaScanSucceed did_lua_scan_succeed) {
                                    if (did_lua_scan_succeed == DidLuaScanSucceed::No)
                                    {
                                        scan_result.Errors.emplace_back("Was unable to find AOB for "
                                                                        "'GameEngine::Tick' via Lua script");
                                    }
                                });
                    };
        }
    }
} // namespace RC
