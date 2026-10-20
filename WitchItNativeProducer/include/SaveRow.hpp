#pragma once
// Detached native save projection. No Lua wire format or authority receipts.
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace witchit {
struct SaveRow {
    std::string asset_type,asset_name;
    std::int32_t id{};
    bool activated{};
    std::array<double,9> transform{}; // XYZ, Pitch/Yaw/Roll, XYZ scale
    std::vector<std::uint8_t> materials;
};
}
