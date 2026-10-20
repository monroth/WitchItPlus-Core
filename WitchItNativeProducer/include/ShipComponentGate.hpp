#pragma once
// Additive native seam. No raw UObject/ordinary Lua aliases are accepted here.
// Backend supplies genuine current-context and weak index+serial validation;
// this header supplies canonical receipt ownership, cooked admission and bounds.
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ship_parts {
enum class Scope { edit, cleanup };
enum class Field { visible, hidden, collision };
struct State { bool visible{},hidden{};std::uint8_t collision{}; };
struct Descriptor { std::string actor,component; };
struct Fact {
 Descriptor descriptor;
 std::string world,actor_class,component_class,mesh,guid;
 std::uint64_t incarnation{};
 bool authored{},owned{},instanced{},collision_local{};
};
struct Part { std::string name,mesh,guid; };
struct Policy { std::string world,actor_class,component_class,fingerprint;std::vector<Part> parts; };

template<class Backend> class Gate final {
public:
 using Context=typename Backend::Context;
 using Reference=typename Backend::Reference;
 using Guard=std::function<bool()>;
 class Ticket {
  friend class Gate;
  Context context_;Reference reference_;Fact fact_;std::uint64_t generation_;
  Ticket(Context c,Reference r,Fact f,std::uint64_t g):context_(std::move(c)),reference_(std::move(r)),fact_(std::move(f)),generation_(g){}
 public: Ticket(const Ticket&)=delete;Ticket& operator=(const Ticket&)=delete;
 };
 using Handle=std::shared_ptr<const Ticket>;
private:
 Backend& backend_;Policy policy_;std::uint64_t generation_{1};bool alive_{true};
 std::unordered_map<const Ticket*,std::weak_ptr<const Ticket>> issued_;
 static bool descriptor(const Descriptor& a,const Descriptor& b) {
  return a.actor==b.actor && a.component==b.component;
 }
 bool matches(const Fact& f) const {
  if(f.world!=policy_.world || f.actor_class!=policy_.actor_class || f.component_class!=policy_.component_class
   || !f.authored || !f.owned || f.instanced || !f.collision_local || !f.incarnation)return false;
  const auto prefix=policy_.world+":PersistentLevel.";
  if(f.descriptor.actor.size()<=prefix.size() || f.descriptor.actor.compare(0,prefix.size(),prefix)!=0)return false;
  const auto name=f.descriptor.actor.substr(prefix.size());
  if(name.find("Default__")!=std::string::npos || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos)return false;
  for(const auto& p:policy_.parts)
   if(p.name==f.descriptor.component)return p.mesh==f.mesh && p.guid==f.guid;
  return false;
 }
 bool current(const Context& c,Scope s,const Guard& g) const {
  return alive_ && g && g() && backend_.current(c,s) && alive_ && g() && backend_.current(c,s);
 }
 void require(bool v) const {if(!v)throw std::runtime_error("ship component receipt/scope refused");}
 void validate(const Context& c,const Handle& h,Scope s,const Guard& g) {
  // Reject aliases/control blocks BEFORE calling any object backend function.
  require(h && alive_);auto it=issued_.find(h.get());require(it!=issued_.end());
  auto canonical=it->second.lock();require(canonical && !canonical.owner_before(h) && !h.owner_before(canonical));
  require(h->generation_==generation_ && backend_.same_context(c,h->context_) && current(c,s,g));
  require(backend_.alive(c,h->reference_,g) && current(c,s,g));
  auto f=backend_.fact(c,h->reference_,g);
  require(current(c,s,g) && matches(f) && descriptor(f.descriptor,h->fact_.descriptor)
   && f.incarnation==h->fact_.incarnation);
 }
public:
 Gate(Backend& b,Policy policy):backend_(b),policy_(std::move(policy)) {
  if(policy_.parts.empty() || policy_.parts.size()>256 || policy_.fingerprint.size()!=64)throw std::runtime_error("invalid ship policy");
 }
 Handle acquire(const Context& c,const Descriptor& d,Scope s,const Guard& g) {
  require(current(c,s,g));auto reference=backend_.resolve(c,d,g);
  require(current(c,s,g) && backend_.alive(c,reference,g));
  auto f=backend_.fact(c,reference,g);require(current(c,s,g) && matches(f) && descriptor(d,f.descriptor));
  for(auto it=issued_.begin();it!=issued_.end();)if(it->second.expired())it=issued_.erase(it);else ++it;
  require(issued_.size()<4096);
  Handle h(new Ticket(c,reference,std::move(f),generation_));issued_.emplace(h.get(),h);return h;
 }
 State read(const Context& c,const Handle& h,Scope s,const Guard& g) {
  validate(c,h,s,g);auto value=backend_.read(c,h->reference_,g);
  require(value.collision<=5);validate(c,h,s,g);return value;
 }
 Handle resolve(const Context& c,const Descriptor& d,const Handle& expected,Scope s,const Guard& g) {
  if(!expected)return acquire(c,d,s,g);
  validate(c,expected,s,g);require(descriptor(expected->fact_.descriptor,d));return expected;
 }
 Fact describe(const Context& c,const Handle& h,Scope s,const Guard& g) {
  validate(c,h,s,g);return h->fact_; // detached scalar metadata, never an engine pointer
 }
 bool set_bool(const Context& c,const Handle& h,Scope s,Field field,bool value,const Guard& g) {
  require(field==Field::visible || field==Field::hidden);validate(c,h,s,g);
  // false propagation is mandatory: a selected component never hides siblings.
  const bool accepted=backend_.set_bool(c,h->reference_,field,value,false,g);
  validate(c,h,s,g);return accepted;
 }
 bool set_collision(const Context& c,const Handle& h,Scope s,unsigned value,const Guard& g) {
  require(value<=5);validate(c,h,s,g);
  const bool accepted=backend_.set_collision(c,h->reference_,static_cast<std::uint8_t>(value),g);
  validate(c,h,s,g);return accepted;
 }
 void retire() noexcept {alive_=false;++generation_;issued_.clear();}
};
} // namespace ship_parts
