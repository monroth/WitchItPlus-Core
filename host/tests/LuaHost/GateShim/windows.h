#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef int BOOL;
typedef void* PVOID;
#define TRUE 1
#define FALSE 0
#define CALLBACK
#define INIT_ONCE_STATIC_INIT {0}
typedef struct { void* value; } INIT_ONCE;
typedef INIT_ONCE* PINIT_ONCE;
typedef struct { void* value; } CRITICAL_SECTION;
typedef BOOL (*PINIT_ONCE_FN)(PINIT_ONCE,PVOID,PVOID*);
BOOL InitOnceExecuteOnce(PINIT_ONCE,PINIT_ONCE_FN,PVOID,PVOID*);
void InitializeCriticalSection(CRITICAL_SECTION*);
void DeleteCriticalSection(CRITICAL_SECTION*);
void EnterCriticalSection(CRITICAL_SECTION*);
void LeaveCriticalSection(CRITICAL_SECTION*);
#ifdef __cplusplus
}
#endif
