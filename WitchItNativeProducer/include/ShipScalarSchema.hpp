#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ship_parts {
enum class ScalarKind { boolean, collision_enum };
struct ScalarSlot {std::string name;ScalarKind kind;int offset{},width{},array_dim{1},byte_offset{};unsigned mask{};
 bool out{},returned{},reference{};};
// Pure schema validation also used by the real reflected parameter producer.
// Bit-packed bools can share a byte only when their masks are disjoint.
inline std::string scalar_description(int size,const std::vector<ScalarSlot>& slots,std::string_view function) {
 std::string out(function);out+=" parms_size="+std::to_string(size);
 for(const auto& s:slots)out+=" ["+s.name+" offset="+std::to_string(s.offset)+" width="+std::to_string(s.width)
  +" array_dim="+std::to_string(s.array_dim)+" byte_offset="+std::to_string(s.byte_offset)+" mask="+std::to_string(s.mask)
  +" out="+std::to_string(s.out)+" return="+std::to_string(s.returned)+" reference="+std::to_string(s.reference)+"]";
 return out;
}
inline void scalar_schema(int size,const std::vector<ScalarSlot>& slots,const std::vector<std::string>& names,std::string_view function={}) {
 auto fail=[&](std::string reason) {throw std::runtime_error("ShipParts scalar schema: "+reason+"; "+scalar_description(size,slots,function));};
 if(size<=0 || size>256)fail("parameter buffer size outside 1..256");
 if(slots.size()!=names.size())fail("parameter count differs: expected "+std::to_string(names.size()));
 std::set<std::string> seen;
 for(const auto& s:slots) {
  if(!seen.insert(s.name).second)fail(s.name+": duplicate field");
  if(std::find(names.begin(),names.end(),s.name)==names.end())fail(s.name+": unexpected field name");
  if(s.array_dim!=1 || s.width!=1 || s.offset<0 || s.offset>=size || s.byte_offset<0
   || s.byte_offset>=s.width)fail(s.name+": scalar layout/bounds differ");
  if(s.out || s.returned || s.reference)fail(s.name+": expected input value flags");
  if((s.name=="NewType")!=(s.kind==ScalarKind::collision_enum))fail(s.name+": scalar type differs");
  if(s.kind==ScalarKind::boolean && (!s.mask || s.mask>255 || (s.mask!=255 && (s.mask&(s.mask-1)))))fail(s.name+": invalid boolean mask");
  for(const auto& b:slots)if(&s!=&b && s.offset+s.byte_offset==b.offset+b.byte_offset
   && (s.kind!=ScalarKind::boolean || b.kind!=ScalarKind::boolean || (s.mask&b.mask)))fail(s.name+": overlaps "+b.name);
 }
}
} // namespace ship_parts
