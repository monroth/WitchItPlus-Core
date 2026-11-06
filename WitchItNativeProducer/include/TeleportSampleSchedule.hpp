#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
namespace witchit::actors {
// A user-armed diagnostic captures only the actual overlap event. No frame
// observer is retained after the sample.
struct TeleportSampleSchedule {
 inline static constexpr std::array<double,1> times{0};
 std::size_t next{};
 std::optional<double> due(double elapsed) {
  if(!std::isfinite(elapsed) || next==times.size() || elapsed<times[next])return {};
  return times[next++];
 }
 bool done()const{return next==times.size();}
};
}
