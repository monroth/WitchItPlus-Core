#if defined(_WIN32)
#include <windows.h>
#include "lua.h"

static struct {
    CRITICAL_SECTION LockSct;
} Gl;
static INIT_ONCE GlOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitializeLuaGate(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&Gl.LockSct);
    return TRUE;
}

void LuaLockInitial(lua_State* L)
{
    (void)L;
    /* One recursive gate for the embedded host module, not for each VM. */
    InitOnceExecuteOnce(&GlOnce, InitializeLuaGate, NULL, NULL);
}

void LuaLockFinal(lua_State* L)
{
    (void)L;
    /* Another VM may still use this gate. Individual state teardown must not
     * delete it or release the lua_close acquisition before state memory is
     * freed. lua_close now performs that matching unlock after close_state. */
}

void LuaLock(lua_State* L)
{
    LuaLockInitial(L);
    /* Wait for control of mutex */
    EnterCriticalSection(&Gl.LockSct);
}

void LuaUnlock(lua_State* L)
{
    (void)L;
    /* Release control of mutex */
    LeaveCriticalSection(&Gl.LockSct);
}

#else
#include <pthread.h>
#include "lua.h"
#include <stdlib.h>
static pthread_mutex_t GlobalLuaGate;
static pthread_once_t GlobalLuaGateOnce = PTHREAD_ONCE_INIT;
static void InitializeLuaGate(void) {
    pthread_mutexattr_t attribute;
    if (pthread_mutexattr_init(&attribute) ||
        pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&GlobalLuaGate, &attribute)) abort();
    pthread_mutexattr_destroy(&attribute);
}
void LuaLockInitial(lua_State* L) { (void)L; if (pthread_once(&GlobalLuaGateOnce, InitializeLuaGate)) abort(); }
void LuaLockFinal(lua_State* L) { (void)L; }
void LuaLock(lua_State* L) { LuaLockInitial(L); if (pthread_mutex_lock(&GlobalLuaGate)) abort(); }
void LuaUnlock(lua_State* L) { (void)L; if (pthread_mutex_unlock(&GlobalLuaGate)) abort(); }
#endif

/* Notices run under the same actual embedded gate as lua_newthread/GC. They
 * manipulate host POD identities only; they cannot call consumer code. */
#include <stdlib.h>
#include <lstate.h>
#include <ldo.h>
static LuaHostThreadNotice HostThreadNotice;
int LuaHostInstallThreadNotice(LuaHostThreadNotice notice) {
    int accepted;
    if(!notice) return 0;
    LuaLock(NULL);
    accepted = HostThreadNotice == NULL;
    if(accepted) HostThreadNotice = notice;
    LuaUnlock(NULL);
    return accepted;
}
void LuaHostThreadBorn(lua_State* parent, lua_State* child) {
    if(HostThreadNotice && HostThreadNotice(parent,child,parent->hook,1)!=0) {
        child->hook = NULL; child->hookmask = 0;
        luaD_throw(parent,LUA_ERRMEM);
    }
}
void LuaHostThreadFree(lua_State* parent, lua_State* child) {
    if(HostThreadNotice && HostThreadNotice(parent,child,NULL,0)!=0) abort();
}
