#ifndef UE4SS_UIKIT_LUA_HOST_H
#define UE4SS_UIKIT_LUA_HOST_H

#include <stdint.h>

#if defined(_WIN32)
#define UE4SS_LUA_HOST_CALL __cdecl
#else
#define UE4SS_LUA_HOST_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UE4SS_UIKIT_LUA_HOST_ABI_VERSION UINT32_C(1)
#define UE4SS_UIKIT_LUA_HOST_PREFIX_SIZE UINT32_C(8)

/* IDs are lookup keys, never addresses or authority without an active host entry. */
#pragma pack(push, 8)
typedef struct ue4ss_lua_vm_handle { uint64_t id; uint64_t generation; } ue4ss_lua_vm_handle;
typedef struct ue4ss_lua_module_handle { uint64_t id; uint64_t generation; } ue4ss_lua_module_handle;
typedef struct ue4ss_lua_context_handle { uint64_t id; uint64_t generation; } ue4ss_lua_context_handle;
typedef struct ue4ss_lua_reference_handle { uint64_t id; uint64_t generation; } ue4ss_lua_reference_handle;
typedef struct ue4ss_lua_callback_handle { uint64_t id; uint64_t generation; } ue4ss_lua_callback_handle;

typedef uint32_t ue4ss_lua_host_status;
enum {
    UE4SS_LUA_HOST_OK = 0,
    UE4SS_LUA_HOST_INVALID_HANDLE = 1,
    UE4SS_LUA_HOST_STALE_GENERATION = 2,
    UE4SS_LUA_HOST_RETIRED = 3,
    UE4SS_LUA_HOST_WRONG_THREAD = 4,
    UE4SS_LUA_HOST_WRONG_OWNER = 5,
    UE4SS_LUA_HOST_PENDING = 6,
    UE4SS_LUA_HOST_READY = 7,
    UE4SS_LUA_HOST_BUSY = 8,
    UE4SS_LUA_HOST_UNSUPPORTED_ABI = 9,
    UE4SS_LUA_HOST_UNSUPPORTED_SIZE = 10,
    UE4SS_LUA_HOST_YIELD_FORBIDDEN = 11,
    UE4SS_LUA_HOST_INTERNAL_ERROR = 12
};

/* Shared native consumer ABI. Preserve names/offsets for matched native modules. */
#define UE4SS_LUA_HOST_CAP_VALUES_V1 UINT64_C(1)
/* Scalar callback/token backend ready; does not certify object wrappers. */
#define UE4SS_LUA_HOST_CAP_SCALARS_V1 UINT64_C(2)
typedef uint32_t ue4ss_lua_value_kind;
enum {
    UE4SS_LUA_VALUE_NIL = 0,
    UE4SS_LUA_VALUE_BOOLEAN = 1,
    UE4SS_LUA_VALUE_INTEGER = 2,
    UE4SS_LUA_VALUE_NUMBER = 3,
    UE4SS_LUA_VALUE_STRING = 4,
    UE4SS_LUA_VALUE_TABLE = 5,
    UE4SS_LUA_VALUE_FUNCTION = 6,
    UE4SS_LUA_VALUE_TOKEN = 7,
    UE4SS_LUA_VALUE_OTHER = 8
};
typedef struct ue4ss_lua_token_v1 {
    uint64_t bridge;
    uint64_t owner;
    uint64_t id;
    uint32_t kind;
    uint32_t reserved;
} ue4ss_lua_token_v1;
typedef struct ue4ss_lua_value_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    ue4ss_lua_value_kind kind;
    uint32_t boolean_value;
    int64_t integer_value;
    double number_value;
    const char* string_data;
    uint64_t string_size;
    ue4ss_lua_token_v1 token;
} ue4ss_lua_value_v1;

typedef struct ue4ss_lua_host_context_info_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    ue4ss_lua_context_handle context;
    ue4ss_lua_vm_handle vm;
    ue4ss_lua_module_handle module;
} ue4ss_lua_host_context_info_v1;

/* Consumers return a status. They must not yield, call lua_error, retain the
 * context, or let an exception escape this callback. Registration and owner
 * validation happen in a host trampoline before consumer code is entered.
 * Every operation requires that thread's active host entry and owner binding;
 * handle values alone never establish permission to act on another owner. */
typedef ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *ue4ss_lua_host_callback_v1)(
    ue4ss_lua_context_handle context);

typedef struct ue4ss_uikit_lua_host_api_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t capabilities;
    /* Caller initializes output's ABI version and byte capacity. Host validates
     * both before writing; v1 requires the entire context-info record. Larger
     * buffers receive only known v1 fields. Context is the active thread scope. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *current_context)(
        ue4ss_lua_host_context_info_v1* output);
    /* Owner is the module of the active host entry, never a client argument. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *register_callback)(
        ue4ss_lua_host_callback_v1 callback, ue4ss_lua_callback_handle* output);
    /* VM must match the active scope; callback must belong to the same module. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *invoke)(
        ue4ss_lua_vm_handle vm, ue4ss_lua_callback_handle callback);
    /* Only the active scope's VM may be closed. Pending never waits here. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *request_vm_close)(
        ue4ss_lua_vm_handle vm);
    /* Retires only the active host entry's module. Pending never waits here. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *request_module_retire)(void);
    /* All tail operations validate the active context's generation, VM, module,
     * thread, and retirement state BEFORE touching Lua. No state address crosses
     * this ABI. Stack indexes are 1-based or negative-from-top; index 0 and Lua
     * pseudo-indexes are forbidden. Reading an absent ordinary stack slot yields
     * NIL; other operations require existing slots. read is exact-type (no coercion). Strings
     * returned by read remain borrowed only until the next host operation. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *stack_top)(ue4ss_lua_context_handle, int32_t*);
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *stack_set_top)(ue4ss_lua_context_handle, int32_t);
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *value_read)(ue4ss_lua_context_handle, int32_t, ue4ss_lua_value_v1*);
    /* Consumer upvalues exclude host-private closure bookkeeping. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *upvalue_read)(ue4ss_lua_context_handle, uint32_t, ue4ss_lua_value_v1*);
    /* Push supports NIL/BOOLEAN/INTEGER/NUMBER/STRING/TABLE/TOKEN. TABLE creates
     * an empty table; STRING is copied. TOKEN is a host-owned POD userdata with
     * an exact protected host metatable and host module provenance; no DLL gc. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *value_push)(ue4ss_lua_context_handle, const ue4ss_lua_value_v1*);
    /* Raw assignment; consumes the top value. Key bytes are copied. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *table_set_field)(ue4ss_lua_context_handle, int32_t, const char*, uint64_t);
    /* Creates a registry reference without consuming the indexed value. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *reference_create)(ue4ss_lua_context_handle, int32_t, ue4ss_lua_reference_handle*);
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *reference_push)(ue4ss_lua_context_handle, ue4ss_lua_reference_handle);
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *reference_release)(ue4ss_lua_context_handle, ue4ss_lua_reference_handle);
    /* Calls the function below nargs. Fixed nresults only, no yield. Failure
     * leaves the pre-function stack top (function + arguments removed). */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *protected_call)(ue4ss_lua_context_handle, int32_t, int32_t);
    /* Consumes nupvalues and pushes a HOST trampoline closure. The callback
     * handle is revalidated on every invocation before entering consumer code. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *callback_push)(ue4ss_lua_context_handle, ue4ss_lua_callback_handle, uint32_t);
    /* Sets trampoline result count; valid only inside that registered callback. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *callback_return)(ue4ss_lua_context_handle, uint32_t);
    /* Host uses the real UE4SS UObject wrapper metatable + SDK-owned storage tag
     * validation before extraction. Arbitrary userdata/lightuserdata rejected.
     * push uses RC::LuaType::auto_construct_object in the serialized host scope.
     * Consumer must validate its own UObject incarnation before calling push. */
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *object_read)(ue4ss_lua_context_handle, int32_t, void**);
    ue4ss_lua_host_status (UE4SS_LUA_HOST_CALL *object_push)(ue4ss_lua_context_handle, void*);
} ue4ss_uikit_lua_host_api_v1;
#pragma pack(pop)

/* Validate this prefix before reading a table member. Larger compatible tables
 * are accepted; unsupported versions and prefixes shorter than eight bytes are
 * rejected even when the caller requests no optional fields. The
 * accessor returns an immutable table valid for the host module lifetime. */
static inline ue4ss_lua_host_status ue4ss_lua_host_validate_prefix(
    uint32_t abi_version, uint32_t struct_size, uint32_t required_size)
{
    if (abi_version != UE4SS_UIKIT_LUA_HOST_ABI_VERSION)
        return UE4SS_LUA_HOST_UNSUPPORTED_ABI;
    if (struct_size < UE4SS_UIKIT_LUA_HOST_PREFIX_SIZE || struct_size < required_size)
        return UE4SS_LUA_HOST_UNSUPPORTED_SIZE;
    return UE4SS_LUA_HOST_OK;
}

/* Resolve this versioned C export with GetProcAddress. It does not expose raw
 * Lua symbols or accept a consumer-created VM. Table acquisition grants no
 * authority: operations still require that thread's active host entry. */
#if defined(_WIN32)
# if defined(RC_UE4SS_EXPORTS)
#  define UE4SS_UIKIT_LUA_HOST_EXPORT __declspec(dllexport)
# else
#  define UE4SS_UIKIT_LUA_HOST_EXPORT __declspec(dllimport)
# endif
#else
# define UE4SS_UIKIT_LUA_HOST_EXPORT
#endif
UE4SS_UIKIT_LUA_HOST_EXPORT ue4ss_lua_host_status UE4SS_LUA_HOST_CALL
UE4SS_GetUIKitLuaHostApi_v1(uint32_t requested_abi_version,
                           uint32_t minimum_table_size,
                           const ue4ss_uikit_lua_host_api_v1** output);

#ifdef __cplusplus
}
#endif
#endif
