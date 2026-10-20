#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
namespace witchit::actors {
// Four one-shot observations; a slow frame records its actual elapsed time,
// never invents an earlier location. World change/retirement drops the owner.
struct TeleportSampleSchedule {
 inline static constexpr std::array<double,4> times{0,.1,.5,2};
 std::size_t next{};
 std::optional<double> due(double elapsed) {
  if(!std::isfinite(elapsed) || next==times.size() || elapsed<times[next])return {};
  return times[next++];
 }
 bool done()const{return next==times.size();}
};
}
