#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>

namespace witchit {
// Outer/Lua references do not keep a UObject alive. A lease shares an engine
// root, but explicit engine destruction can still retire that object's slot.
// Never dereference the old address to release it: resolve index + serial first.
template<class Object, class Weak> class ObjectRoots {
 struct Entry {Object* address;Weak weak;std::size_t count;bool borrowed;};
 std::map<Object*,std::shared_ptr<Entry>> objects_;
 std::map<std::int64_t,std::shared_ptr<Entry>> leases_;
 std::int64_t next_{};
public:
 std::int64_t retain(Object* object) {
  if(!object || next_==INT64_MAX)throw std::runtime_error("Native.retain: invalid object/lease exhausted");
  auto i=objects_.find(object);
  auto entry=i==objects_.end()?nullptr:i->second;
  if(!entry || entry->weak.GetEvenIfUnreachable()!=object)
   entry=std::make_shared<Entry>(Entry{object,Weak(object),0,object->IsRootSet()});
  const auto id=next_+1;
  leases_.emplace(id,entry);
  try {objects_.insert_or_assign(object,entry);}catch(...){leases_.erase(id);throw;}
  if(!entry->count && !entry->borrowed)object->SetRootSet();
  ++entry->count;next_=id;return id;
 }
 void release(std::int64_t id) {
  auto lease=leases_.find(id);if(lease==leases_.end())return;
  auto entry=lease->second;
  if(--entry->count==0){
   auto* live=entry->weak.GetEvenIfUnreachable();
   if(live==entry->address && !entry->borrowed)live->ClearRootSet();
   auto i=objects_.find(entry->address);
   if(i!=objects_.end() && i->second==entry)objects_.erase(i);
  }
  leases_.erase(lease);
 }
 void clear(){while(!leases_.empty())release(leases_.begin()->first);}
};
}
