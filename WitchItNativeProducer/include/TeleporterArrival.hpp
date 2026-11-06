#pragma once
#include <vector>
#include <algorithm>
namespace witchit {
// Per-pawn destination overlap lifetime, independent of frame rate and clock.
// The engine supplies weak allocation identities; EndOverlap alone releases a
// live arrival. A destroyed/reused UObject never inherits another pawn's block.
template<class Allocation> class TeleporterArrival {
 struct Block {Allocation pawn,zone;};std::vector<Block> blocks_;
public:
 template<class Valid> bool blocked(const Allocation& zone,const Allocation& pawn,Valid valid) {
  std::erase_if(blocks_,[&](const auto& b){return !valid(b.zone)||!valid(b.pawn);});
  return std::any_of(blocks_.begin(),blocks_.end(),[&](const auto& b){return b.zone.pointer==zone.pointer&&b.pawn.pointer==pawn.pointer;});
 }
 void arrive(const Allocation& zone,const Allocation& pawn){blocks_.push_back({pawn,zone});}
 template<class Pointer> void leave(Pointer zone,Pointer pawn){std::erase_if(blocks_,[&](const auto& b){return b.zone.pointer==zone&&b.pawn.pointer==pawn;});}
 void clear(){blocks_.clear();}
};
}
