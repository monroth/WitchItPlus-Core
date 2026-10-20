#pragma once
#include <array>
#include <cmath>
#include <stdexcept>
namespace witchit::actors {
using LandingVector=std::array<double,3>;
// Circumscribed XY collision radius about the ACTOR origin, including a mesh
// whose bounds centre is offset from that origin. Bounds are already world AABBs.
inline double landing_radius(const LandingVector& actor,const LandingVector& origin,const LandingVector& extent) {
 for(const auto& v:{actor,origin,extent})for(double n:v)if(!std::isfinite(n))throw std::runtime_error("non-finite teleporter collision bounds");
 for(double n:extent)if(n<0)throw std::runtime_error("negative teleporter collision bounds");
 return std::hypot(origin[0]-actor[0],origin[1]-actor[1])+std::hypot(extent[0],extent[1]);
}
inline LandingVector landing_ahead(const LandingVector& exit,double yaw,double marker_radius,double pawn_radius,double z) {
 constexpr double margin=16; // UU, beyond BOTH collision footprints
 if(!std::isfinite(yaw)||!std::isfinite(marker_radius)||!std::isfinite(pawn_radius)||!std::isfinite(z)||marker_radius<0||pawn_radius<0)throw std::runtime_error("invalid teleporter destination bounds");
 const double distance=marker_radius+pawn_radius+margin,radians=yaw*3.14159265358979323846/180;
 return {exit[0]+std::cos(radians)*distance,exit[1]+std::sin(radians)*distance,z};
}
}
