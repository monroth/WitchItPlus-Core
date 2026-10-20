#pragma once
#include "ShipComponentGate.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace ship_parts {
// Production backend. E is UE4SSEngine; tests replace only the engine boundary.
// No Lua UObject is accepted: resolution starts from a bounded cooked descriptor.
template<class E> class UnrealBackend final {
public:
 using Object=typename E::Object;
 using Context=typename E::Context;
 using Guard=std::function<bool()>;
 struct Reference {
  Object actor,component,type,actor_type,mesh,scs,node,component_template,parent,root;
  Descriptor descriptor;
 };
private:
 E& engine_;Policy policy_;
 static void require(bool b){if(!b)throw std::runtime_error("ShipParts Unreal admission refused");}
 const Part& part(std::string_view name) const {
  for(const auto& p:policy_.parts)if(p.name==name)return p;
  throw std::runtime_error("component absent from cooked ship policy");
 }
 void local(const Context& c,const Guard& guard) const {
  require(engine_.current(c));require(guard && guard());require(engine_.current(c));
 }
 bool raw_live(const Reference& r) const {
  for(const auto* p:{&r.actor,&r.component,&r.type,&r.actor_type,&r.mesh,&r.scs,&r.node,
   &r.component_template,&r.parent,&r.root})if(!engine_.live(*p))return false;
  return true;
 }
 // No Lua callbacks in this witness. Every dereference is preceded by weak
 // serial validation; GetWorld results get an immediate scope/lifetime check.
 bool witness(const Context& c,const Reference& r) const {
  if(!engine_.current(c) || !raw_live(r))return false;
  if(!engine_.same(engine_.type(r.actor),r.actor_type) || !engine_.same(engine_.type(r.component),r.type))return false;
  if(engine_.path(r.actor)!=r.descriptor.actor || engine_.name(r.component)!=r.descriptor.component
   || engine_.path(r.actor_type)!=policy_.actor_class || engine_.path(r.type)!=policy_.component_class)return false;
  if(!engine_.was_loaded(r.actor) || !engine_.same(engine_.outer(r.actor),engine_.persistent_level(c))
   || !engine_.same(engine_.outer(r.component),r.actor))return false;
  if(!engine_.in_world(c,r.actor) || !engine_.current(c) || !raw_live(r)
   || !engine_.in_world(c,r.component) || !engine_.current(c) || !raw_live(r))return false;
  const auto& p=part(r.descriptor.component);
  if(!engine_.same(engine_.object_field(r.actor,r.descriptor.component),r.component)
   || !engine_.same(engine_.object_field(r.component,"StaticMesh"),r.mesh)
   || engine_.path(r.mesh)!=p.mesh || engine_.enum_field(r.component,"CreationMethod")!=1
   || !engine_.same(engine_.object_field(r.actor_type,"SimpleConstructionScript"),r.scs))return false;
  // Require the exact SCS variable and template, not a generated name guess.
  auto nodes=engine_.object_array(r.scs,"AllNodes",256);
  if(std::count_if(nodes.begin(),nodes.end(),[&](const Object& n){return engine_.same(n,r.node);})!=1)return false;
  if(engine_.name_field(r.node,"InternalVariableName")!=p.name || engine_.guid_field(r.node,"VariableGuid")!=p.guid
   || !engine_.same(engine_.object_field(r.node,"ComponentTemplate"),r.component_template)
   || !engine_.same(engine_.type(r.component_template),r.type)
   || !engine_.same(engine_.object_field(r.component_template,"StaticMesh"),r.mesh))return false;
  // Supported collision proof: static, non-simulating SCS meshes attached only
  // to the ship's two plain SceneComponents. No primitive ancestor/body exists
  // to receive an engine attachment weld. Arbitrary welded topology is refused.
  if(engine_.enum_field(r.component,"Mobility")!=0 || engine_.bool_field(r.component,"BodyInstance.bSimulatePhysics")
   || engine_.bool_field(r.component,"bUseAttachParentBound")
   || !engine_.same(engine_.object_field(r.component,"AttachParent"),r.parent)
   || !engine_.same(engine_.object_field(r.actor,"SharedRoot"),r.parent)
   || !engine_.same(engine_.object_field(r.actor,"RootComponent"),r.root)
   || engine_.path(engine_.type(r.parent))!="/Script/Engine.SceneComponent"
   || engine_.path(engine_.type(r.root))!="/Script/Engine.SceneComponent"
   || engine_.name(r.parent)!="SharedRoot" || !engine_.same(r.parent,r.root)
   || !engine_.same(engine_.outer(r.parent),r.actor) || !engine_.same(engine_.outer(r.root),r.actor))return false;
  // Cooked RootNodes contains SharedRoot. DefaultSceneRootNode is an unused
  // editor fallback template; requiring that template would refuse every ship.
  if(engine_.optional_object_field(r.root,"AttachParent"))return false;
  return engine_.current(c) && raw_live(r);
 }
 void validate(const Context& c,const Reference& r,const Guard& g) const {
  local(c,g);require(raw_live(r));require(witness(c,r));local(c,g);require(witness(c,r));
 }
public:
 struct Pick {Reference reference;bool blocked{};int item{-1};};
 bool trace_ready(const Context& c) const {return engine_.current(c) && engine_.trace_ready(c);}
 Pick pick(const Context& c,const Guard& g) {
  local(c,g);require(engine_.trace_ready(c));auto hit=engine_.trace(c,g);
  require(engine_.current(c));if(!hit.blocked){local(c,g);return {{},false,-1};}
  require(hit.item==-1 && engine_.live(hit.component));
  // The exact native hit weak identity survives every following Lua guard.
  auto held=[&]{return engine_.current(c) && engine_.live(hit.component) && g && g()
   && engine_.current(c) && engine_.live(hit.component);};
  require(held());auto actor=engine_.outer(hit.component);
  Descriptor d{engine_.path(actor),engine_.name(hit.component)};
  auto r=resolve(c,d,held);require(held() && engine_.same(r.component,hit.component));return {r,true,-1};
 }
 UnrealBackend(E& engine,Policy policy):engine_(engine),policy_(std::move(policy)){}
 bool current(const Context& c,Scope) const{return engine_.current(c);}
 bool same_context(const Context& a,const Context& b) const{return engine_.same_context(a,b);}
 Reference resolve(const Context& c,const Descriptor& d,const Guard& g) {
  local(c,g);(void)part(d.component);
  const auto prefix=policy_.world+":PersistentLevel.";
  require(d.actor.starts_with(prefix) && d.actor.size()>prefix.size()
   && d.actor.substr(prefix.size()).find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")==std::string::npos
   && d.actor.find("Default__")==std::string::npos);
  Reference r;r.descriptor=d;r.actor=engine_.find(d.actor);local(c,g);require(engine_.live(r.actor));
  r.actor_type=engine_.type(r.actor);require(engine_.path(r.actor_type)==policy_.actor_class);
  r.component=engine_.object_field(r.actor,d.component);r.type=engine_.type(r.component);
  require(engine_.path(r.type)==policy_.component_class);
  r.mesh=engine_.object_field(r.component,"StaticMesh");r.scs=engine_.object_field(r.actor_type,"SimpleConstructionScript");
  auto nodes=engine_.object_array(r.scs,"AllNodes",256);unsigned matches=0;
  for(const auto& n:nodes)if(engine_.name_field(n,"InternalVariableName")==d.component){r.node=n;++matches;}
  require(matches==1);r.component_template=engine_.object_field(r.node,"ComponentTemplate");
  r.parent=engine_.object_field(r.component,"AttachParent");r.root=engine_.object_field(r.actor,"RootComponent");
  validate(c,r,g);return r;
 }
 bool alive(const Context& c,const Reference& r,const Guard& g) const {
  local(c,g);return raw_live(r) && witness(c,r);
 }
 Fact fact(const Context& c,const Reference& r,const Guard& g) const {
  validate(c,r,g);const auto& p=part(r.descriptor.component);
  return {r.descriptor,policy_.world,policy_.actor_class,policy_.component_class,p.mesh,p.guid,
   engine_.incarnation(r.component),true,true,false,true};
 }
 State read(const Context& c,const Reference& r,const Guard& g) const {
  validate(c,r,g);
  unsigned collision=engine_.enum_field(r.component,"BodyInstance.CollisionEnabled");require(collision<=5);
  State s{engine_.bool_field(r.component,"bVisible"),engine_.bool_field(r.component,"bHiddenInGame"),static_cast<std::uint8_t>(collision)};
  validate(c,r,g);return s;
 }
 bool set_bool(const Context& c,const Reference& r,Field f,bool value,bool propagate,const Guard& g) {
  require((f==Field::visible || f==Field::hidden) && !propagate);validate(c,r,g);
  auto function=engine_.prepare_bool(r.component,f,value,false);
  // prepare owns the exact class/function weak refs and detached scalar params.
  auto admission=[&]{return engine_.current(c) && raw_live(r) && witness(c,r) && engine_.prepared_live(function);};
  local(c,g);require(admission());engine_.execute(function);
  // No getters and no stale FProperty access following synchronous retirement.
  require(admission());local(c,g);require(admission());
  return engine_.bool_field(r.component,f==Field::visible?"bVisible":"bHiddenInGame")==value;
 }
 bool set_collision(const Context& c,const Reference& r,std::uint8_t value,const Guard& g) {
  require(value<=5);validate(c,r,g);auto function=engine_.prepare_collision(r.component,value);
  auto admission=[&]{return engine_.current(c) && raw_live(r) && witness(c,r) && engine_.prepared_live(function);};
  local(c,g);require(admission());engine_.execute(function);require(admission());local(c,g);require(admission());
  return engine_.enum_field(r.component,"BodyInstance.CollisionEnabled")==value;
 }
 std::vector<Descriptor> inventory(const Context& c,const Guard& g) {
  local(c,g);std::vector<Descriptor> out;std::set<std::string> seen;
  auto actors=engine_.actors(c,policy_.actor_class,2);
  for(const auto& actor:actors)for(const auto& p:policy_.parts) {
   Descriptor d{engine_.path(actor),p.name};require(seen.insert(d.actor+"|"+d.component).second);
   // Read-only cooked roster; resolve and snapshot only the selected edit.
   require(out.size()<512);out.push_back(std::move(d));
  }
  local(c,g);return out;
 }
};
} // namespace ship_parts
