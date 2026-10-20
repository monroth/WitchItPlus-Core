#pragma once

#include <Unreal/UObjectArray.hpp>

namespace RC::Unreal::Hook::Internal {
// Internal stable host implementation, called by the existing exported SDK
// boundary with that boundary's actual caller address. Not a consumer API.
void AddOwnedCreateListener(FUObjectCreateListener*, const void* origin);
void RemoveOwnedCreateListener(FUObjectCreateListener*, const void* origin);
void AddOwnedDeleteListener(FUObjectDeleteListener*, const void* origin);
void RemoveOwnedDeleteListener(FUObjectDeleteListener*, const void* origin);
}
