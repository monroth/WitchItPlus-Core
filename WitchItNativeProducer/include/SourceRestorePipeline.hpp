#pragma once
#include <string>
#include <set>
#include <stdexcept>
namespace witchit::actors {
// Production batch ordering, also exercised by the no-engine stress test.
// Rollback covers *every* phase, including publication/BeginPlay failures.
template<class Allocate,class Finish,class Apply,class Publish,class Rollback>
void restore_source_batch(Allocate allocate,Finish finish,Apply apply,Publish publish,Rollback rollback) {
 try {allocate();finish();apply();publish();}
 catch(...){rollback();throw;}
}
inline bool source_component_route(const std::wstring& klass) {
 return klass==L"/Script/Engine.StaticMeshActor"||klass==L"/Script/Engine.BlockingVolume"||klass==L"/Script/Engine.DecalActor"
  ||klass==L"/Script/Engine.PointLight"||klass==L"/Script/Engine.SpotLight"||klass==L"/Script/Engine.RectLight"
  ||klass==L"/Script/Engine.SphereReflectionCapture"||klass==L"/Script/Engine.BoxReflectionCapture"
  ||klass==L"/Script/Engine.AmbientSound"||klass==L"/Script/Niagara.NiagaraActor"||klass==L"/Script/Engine.SkeletalMeshActor"||klass==L"/Script/Engine.TextRenderActor";
}
// Static geometry has exact cooked identities and is restored as generic Actors.
// Never enumerate every StaticMeshActor and read every transform per recipe row.
// Blueprint gameplay may need a class/transform match for a base-world actor.
template<class Exact,class Query,class Matches>
auto source_existing_actor(const std::wstring& klass,Exact exact,Query query,Matches matches) {
 auto actor=exact();if(actor || source_component_route(klass))return actor;
 for(auto candidate:query())if(matches(candidate)) {
  if constexpr(requires{candidate.actor.pointer;})return candidate.actor.pointer;
  else return candidate;
 }
 return decltype(actor){};
}

// Component reregistration can recreate child actors. Complete each parent's
// component state before resolving/configuring its child; bind references last.
template<class Rows,class Source,class Parent,class Configure>
void source_component_order(Rows& rows,Source source,Parent parent,Configure configure) {
 std::set<std::wstring> present,done;
 for(auto& row:rows)present.insert(source(row));
 while(done.size()<present.size()) {
  bool progress=false;
  for(auto& row:rows) {
   auto key=source(row),owner=parent(row);if(done.contains(key))continue;
   if(!owner.empty()&&present.contains(owner)&&!done.contains(owner))continue;
   configure(row);done.insert(key);progress=true;
  }
  if(!progress)throw std::runtime_error("source child component ownership cycle");
 }
}

// Shared with the cooked TargetList regression: remap resolved object pointers
// in reflected containers without copying handles or archive ownership.
template<class Get,class Set,class Remap>
void source_remap_objects(int count,Get get,Set set,Remap remap) {
 for(int i=0;i<count;++i){auto old=get(i);if(old){auto next=remap(old);if(next!=old)set(i,next);}}
}

}
