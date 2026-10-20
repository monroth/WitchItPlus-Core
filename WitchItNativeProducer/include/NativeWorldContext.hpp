#pragma once
// Optional gameplay context shared by preview/spawn backends. UI needs no world.
#include <cstdint>
namespace witchit::uikit {
struct NativeWorldObservation {
    std::uint64_t generation{};void* world{};void* local_anchor{};
    std::int32_t world_index{-1},world_serial{},anchor_index{-1},anchor_serial{};
};
struct NativeWorldSource {
    void* backend{};
    NativeWorldObservation (*capture)(void*) noexcept{};
    bool (*current)(void*,NativeWorldObservation) noexcept{};
};
}
