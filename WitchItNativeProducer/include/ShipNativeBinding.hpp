#pragma once
#include "UIKitHostStack.hpp"
#include "ShipUnrealBackend.hpp"
#include "ShipCookedPolicy.hpp"
#include <atomic>
#include <map>
#include <mutex>

namespace ship_parts {
// Existing host C ABI is sufficient for scalar argument/results and opaque
// tokens. No new object extraction API, external wrapper or raw state is used.
template<class E> class NativeBinding final:public std::enable_shared_from_this<NativeBinding<E>> {
 using Stack=witchit::uikit::HostStack;
 using Ref=witchit::uikit::HostReference;
 using Backend=UnrealBackend<E>;
 using NativeGate=Gate<Backend>;
 using Token=ue4ss_lua_token_v1;
 enum class Op {capture,current,resolve,read,visible,hidden,collision,inventory,release,trace_ready,pick};
 struct PartRecord {typename NativeGate::Handle ticket;std::optional<State> original;Descriptor descriptor;};
 struct Record {typename E::Context context;Ref guard;bool alive{true},busy{};std::map<std::uint64_t,std::shared_ptr<PartRecord>> parts;};
 inline static std::weak_ptr<NativeBinding> active_;
 inline static std::atomic<std::uint64_t> sequence_{};
 std::uint64_t bridge_{++sequence_},next_context_{},next_part_{};
 const ue4ss_uikit_lua_host_api_v1* host_{};ue4ss_lua_vm_handle vm_{};ue4ss_lua_module_handle module_{};
 ue4ss_lua_callback_handle callback_{};
 E engine_;Backend backend_;NativeGate gate_;std::map<std::uint64_t,std::shared_ptr<Record>> records_;
 std::vector<Ref> retired_guards_;
 std::recursive_mutex mutex_;bool active_flag_{true};
 static void require(bool b){if(!b)throw std::runtime_error("ShipParts native token/owner refused");}
 bool entry(const Stack& s) const {
  return active_flag_ && witchit::uikit::same_handle(s.info().vm,vm_) && witchit::uikit::same_handle(s.info().module,module_);
 }
 void drain(Stack& s) {
  require(entry(s));auto refs=std::move(retired_guards_);retired_guards_.clear();
  for(auto ref:refs)s.release(ref); // same active VM/module; host rechecks each release
 }
 static std::string text(Stack& s,int index,unsigned max,bool ascii_only=true) {
  auto t=s.get_string(index);require(t && !t->empty() && t->size()<=max);
  for(unsigned char c:*t)require(c>=32 && c!=127 && (!ascii_only || c<=126));return *t;
 }
 Token token(Stack& s,int index,unsigned kind) const {
  auto t=s.get_token(index);require(t && t->bridge==bridge_ && t->kind==kind && t->reserved==0);return *t;
 }
 static Scope mode(Stack& s,int index) {
  auto t=text(s,index,8);require(t=="edit" || t=="cleanup");return t=="edit"?Scope::edit:Scope::cleanup;
 }
 std::shared_ptr<Record> context(Stack& s,int index) const {
  auto t=token(s,index,2);require(t.owner==t.id);auto it=records_.find(t.id);
  require(it!=records_.end() && it->second->alive);return it->second;
 }
 std::shared_ptr<PartRecord> component(Stack& s,const std::shared_ptr<Record>& r,int index) {
  auto t=token(s,index,3);auto c=token(s,1,2);require(t.owner==c.id);
  auto i=r->parts.find(t.id);require(i!=r->parts.end());return i->second;
 }
 bool guard(Stack& s,const std::shared_ptr<Record>& r,Scope lane,Ref operation={}) {
  if(!entry(s) || !r->alive || !engine_.current(r->context))return false;
  int top=s.top();s.push_reference(r->guard);s.string(lane==Scope::edit?"edit":"cleanup");
  bool accepted=s.call(1,1) && s.is_boolean(-1) && s.get_boolean(-1);s.restore(top);
  if(!accepted || !entry(s) || !r->alive || !engine_.current(r->context))return false;
  if(operation) {
   s.push_reference(operation);accepted=s.call(0,1) && s.is_boolean(-1) && s.get_boolean(-1);s.restore(top);
  }
  return accepted && entry(s) && r->alive && engine_.current(r->context);
 }
 void fact(Stack& s,const Fact& f) {
  s.table();int at=s.top();
  for(const auto& [k,v]:{std::pair{"actor",f.descriptor.actor},{"component",f.descriptor.component},
   {"actor_class",f.actor_class},{"component_class",f.component_class},{"mesh",f.mesh},{"guid",f.guid}}){s.string(v);s.set_field(at,k);}
  for(auto [k,v]:{std::pair{"authored_actor",f.authored},{"owned",f.owned},{"instanced",f.instanced},{"collision_local",f.collision_local}}){s.boolean(v);s.set_field(at,k);}
 }
 static void state(Stack& s,State value) {
  s.table();s.boolean(value.visible);s.set_field(-2,"visible");s.boolean(value.hidden);s.set_field(-2,"hidden");s.integer(value.collision);s.set_field(-2,"collision");
 }
 int operate(Stack& s,Op op) {
  require(entry(s));drain(s);
  if(op==Op::capture) {
   require(s.top()==4 && s.is_function(1));auto world=text(s,2,1024),layout=text(s,3,256,false);auto generation=s.read(4);
   require(generation.kind==UE4SS_LUA_VALUE_INTEGER && generation.integer_value>0 && world==pirate_ship_policy().world);
   Ref guard_ref=s.reference(1);int top=s.top();std::uint64_t id{};
   try {
    s.push_reference(guard_ref);s.string("edit");require(s.call(1,1) && s.is_boolean(-1) && s.get_boolean(-1));s.restore(top);
    require(entry(s) && next_context_<UINT64_MAX && records_.size()<64);id=++next_context_;
    auto r=std::make_shared<Record>();r->guard=guard_ref;r->context=engine_.capture(id,world,layout);records_[id]=r;
    require(guard(s,r,Scope::edit));s.token({bridge_,id,id,2,0});return 1;
   }catch(...){if(id)records_.erase(id);s.restore(top);s.release(guard_ref);throw;}
  }
  auto r=context(s,1);auto c=token(s,1,2);const auto lane=mode(s,2);
  if(op==Op::current){require(s.top()==2);s.boolean(guard(s,r,lane));return 1;}
  require(!r->busy);r->busy=true;
  struct Busy {std::shared_ptr<Record> record;~Busy(){record->busy=false;}} busy{r};
  if(op==Op::release) {
   require(s.top()==2 && lane==Scope::cleanup && guard(s,r,lane));
   // Never discard an unrestored original. Compare exact state while live.
   std::vector<std::shared_ptr<PartRecord>> originals;for(const auto& [id,p]:r->parts)originals.push_back(p);
   for(auto p:originals)if(p->original) {
    auto g=[&]{return guard(s,r,lane);};auto now=gate_.read(r->context,p->ticket,lane,g);
    require(now.visible==p->original->visible && now.hidden==p->original->hidden && now.collision==p->original->collision);
   }
   r->alive=false;records_.erase(c.id);s.release(r->guard);r->parts.clear();s.boolean(true);return 1;
  }
  const int count=s.top();const int operation_index=count;require(s.is_function(operation_index));
  auto operation=s.reference(operation_index);
  struct Drop {Stack& stack;Ref ref;~Drop(){try{stack.release(ref);}catch(...){}}} drop{s,operation};
  auto g=[&]{return guard(s,r,lane,operation);};require(g());
  if(op==Op::resolve) {
   require(count==6);Descriptor d{text(s,3,1024),text(s,4,128)};
   typename NativeGate::Handle expected;std::uint64_t part_id{};
   if(s.read(5).kind!=UE4SS_LUA_VALUE_NIL) {auto t=token(s,5,3);require(t.owner==c.id);
    auto it=r->parts.find(t.id);require(it!=r->parts.end());expected=it->second->ticket;part_id=t.id;}
   else for(const auto& [id,p]:r->parts)if(p->descriptor.actor==d.actor && p->descriptor.component==d.component) {
    expected=p->ticket;part_id=id;break; // one canonical receipt per document part
   }
   auto h=gate_.resolve(r->context,d,expected,lane,g);
   if(!part_id){require(next_part_<UINT64_MAX && r->parts.size()<512);part_id=++next_part_;r->parts.emplace(part_id,std::make_shared<PartRecord>(PartRecord{h,{},d}));}
   auto f=gate_.describe(r->context,h,lane,g);require(g());s.token({bridge_,c.id,part_id,3,0});fact(s,f);require(g());return 2;
  }
  if(op==Op::trace_ready) {require(count==3);s.boolean(backend_.trace_ready(r->context) && g());return 1;}
  if(op==Op::pick) {
   require(count==3 && lane==Scope::edit);auto hit=backend_.pick(r->context,g);require(g());
   s.table();int at=s.top();s.boolean(hit.blocked);s.set_field(at,"blocked");if(!hit.blocked)return 1;
   const auto d=hit.reference.descriptor;std::uint64_t id{};typename NativeGate::Handle expected;
   for(const auto& [key,p]:r->parts)if(p->descriptor.actor==d.actor && p->descriptor.component==d.component){id=key;expected=p->ticket;break;}
   auto h=gate_.resolve(r->context,d,expected,lane,g);
   auto f=gate_.describe(r->context,h,lane,g);require(f.incarnation==engine_.incarnation(hit.reference.component) && g());
   if(!id){require(next_part_<UINT64_MAX && r->parts.size()<512);id=++next_part_;r->parts.emplace(id,std::make_shared<PartRecord>(PartRecord{h,{},d}));}
   s.string(d.actor);s.set_field(at,"actor");s.string(d.component);s.set_field(at,"component");s.integer(hit.item);s.set_field(at,"instance");
   s.token({bridge_,c.id,id,3,0});s.set_field(at,"handle");require(g());return 1;
  }
  if(op==Op::inventory) {
   require(count==3);auto list=backend_.inventory(r->context,g);require(g());s.table();int at=s.top();s.integer(list.size());s.set_field(at,"count");
   unsigned n=0;for(const auto& d:list){require(g());s.table();s.string(d.actor);s.set_field(-2,"actor");s.string(d.component);s.set_field(-2,"component");s.set_field(at,std::to_string(++n));}require(g());return 1;
  }
  auto p=component(s,r,3);
  if(op==Op::read){require(count==4);state(s,gate_.read(r->context,p->ticket,lane,g));require(g());return 1;}
  require(count==5);
  auto value=s.read(4);bool accepted{};
  // Native journal is reserved BEFORE any ProcessEvent, including partial fail.
  if(lane==Scope::edit && !p->original)p->original=gate_.read(r->context,p->ticket,lane,g);
  require(lane==Scope::edit || p->original.has_value());
  if(op==Op::visible || op==Op::hidden) {
   require(value.kind==UE4SS_LUA_VALUE_BOOLEAN);bool b=value.boolean_value!=0;
   if(lane==Scope::cleanup)require(b==(op==Op::visible?p->original->visible:p->original->hidden));
   accepted=gate_.set_bool(r->context,p->ticket,lane,op==Op::visible?Field::visible:Field::hidden,b,g);
  }else {
   require(op==Op::collision && value.kind==UE4SS_LUA_VALUE_INTEGER && value.integer_value>=0 && value.integer_value<=5);
   if(lane==Scope::cleanup)require(value.integer_value==p->original->collision);
   accepted=gate_.set_collision(r->context,p->ticket,lane,unsigned(value.integer_value),g);
  }
  require(g());s.boolean(accepted);return 1;
 }
 static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL dispatch(ue4ss_lua_context_handle context) noexcept {
  auto self=active_.lock();if(!self)return UE4SS_LUA_HOST_RETIRED;
  try {
   std::lock_guard lock(self->mutex_);Stack s{self->host_,UE4SS_LUA_HOST_CAP_SCALARS_V1};require(self->entry(s) && witchit::uikit::same_handle(s.info().context,context));
   auto owner=s.upvalue(1),op=s.upvalue(2);require(owner.kind==UE4SS_LUA_VALUE_TOKEN && owner.token.bridge==self->bridge_
    && owner.token.owner==1 && owner.token.id==1 && owner.token.kind==1 && !owner.token.reserved
    && op.kind==UE4SS_LUA_VALUE_INTEGER && op.integer_value>=0 && op.integer_value<=static_cast<int>(Op::pick));
   int top=s.top();int returned;
   try{returned=self->operate(s,static_cast<Op>(op.integer_value));}
   catch(const std::exception& error){s.restore(top);s.nil();s.string(error.what());returned=2;}
   return self->host_->callback_return(context,unsigned(returned));
  }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
 }
public:
 template<class... Args> NativeBinding(const ue4ss_uikit_lua_host_api_v1* host,Args&&... args)
 :host_(host),engine_(host,std::forward<Args>(args)...),backend_(engine_,pirate_ship_policy()),gate_(backend_,pirate_ship_policy()) {
  Stack s{host_,UE4SS_LUA_HOST_CAP_SCALARS_V1};vm_=s.info().vm;module_=s.info().module;
  retired_guards_.reserve(64);
  witchit::uikit::host_require(host_->register_callback(dispatch,&callback_));require(callback_.id!=0);
 }
 ~NativeBinding(){retire();}
 void activate(){active_=this->shared_from_this();}
 void retire() noexcept {
  std::lock_guard lock(mutex_);active_flag_=false;gate_.retire();engine_.retire();
  for(auto& [id,r]:records_)r->alive=false;
  try {for(auto& [id,r]:records_)retired_guards_.push_back(r->guard);}catch(...){} // host VM cleanup owns unqueued refs
  records_.clear();
  // Revoke all receipts BEFORE optional live host release. No retained context
  // or engine object is used. If this is terminal/off-thread, host VM cleanup
  // owns the references; no Lua cleanup is attempted through a stale entry.
  try {Stack s{host_,UE4SS_LUA_HOST_CAP_SCALARS_V1};if(witchit::uikit::same_handle(s.info().vm,vm_) && witchit::uikit::same_handle(s.info().module,module_))
   for(auto ref:retired_guards_)s.release(ref);}catch(...){}
  retired_guards_.clear();
 }
 // Revocation uses pure native bookkeeping. No stale getter, Lua call or undo.
 void before_world() noexcept {
  std::lock_guard lock(mutex_);for(auto& [id,r]:records_)r->alive=false;
  try {for(auto& [id,r]:records_)retired_guards_.push_back(r->guard);}catch(...){active_flag_=false;gate_.retire();engine_.retire();}
  records_.clear();
 }
 int accessor() {
  std::lock_guard lock(mutex_);Stack s{host_,UE4SS_LUA_HOST_CAP_SCALARS_V1};require(entry(s));drain(s);s.table();s.string("ShipParts.Native.Raw/v2");s.set_field(-2,"contract");
  for(auto [name,op]:{std::pair{"capture",Op::capture},{"current",Op::current},{"resolve",Op::resolve},{"read",Op::read},
   {"set_visible",Op::visible},{"set_hidden",Op::hidden},{"set_collision",Op::collision},{"inventory",Op::inventory},{"release",Op::release},{"trace_ready",Op::trace_ready},{"pick",Op::pick}}) {
   s.token({bridge_,1,1,1,0});s.integer(static_cast<int>(op));s.callback(callback_,2);s.set_field(-2,name);
  }return 1;
 }
};
} // namespace ship_parts
