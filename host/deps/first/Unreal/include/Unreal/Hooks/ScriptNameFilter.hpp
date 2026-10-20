#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace RC::Unreal::Hook {
// Host-owned immutable metadata. Rejection executes no consumer code, takes no
// owner lease, allocates no Lua stack and performs no string/path reflection.
struct FScriptNameFilter {
    std::vector<std::int32_t> ComparisonIndices;
    bool Empty() const noexcept { return ComparisonIndices.empty(); }
    bool Accepts(std::int32_t name) const noexcept {
        return Empty() || std::find(ComparisonIndices.begin(),ComparisonIndices.end(),name)!=ComparisonIndices.end();
    }
};
}
