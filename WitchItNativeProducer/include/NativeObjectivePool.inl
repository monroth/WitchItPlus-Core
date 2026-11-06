struct PoolValue {
 Property* field;std::vector<std::max_align_t> bytes;
 explicit PoolValue(Property* p):field(p),bytes((p->GetSize()+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t)){field->InitializeValue(bytes.data());}
 ~PoolValue(){field->DestroyValue(bytes.data());}
};
struct PoolReceipt {Allocation receiver,world;std::vector<std::unique_ptr<PoolValue>> values;std::vector<Allocation> references;bool restored{};};
std::map<std::int64_t,PoolReceipt> pool_receipts_;std::int64_t pool_receipt_serial_{};
static void pool_references(Property* p,void* data,std::vector<Allocation>& refs,unsigned depth=0) {
 using namespace RC::Unreal;require(depth<32,"pool reference depth differs");
 if(auto* o=CastField<FObjectProperty>(p)){auto* object=o->GetObjectPropertyValue(data);if(object)refs.push_back(pin(object));return;}
 if(auto* a=CastField<FArrayProperty>(p)){FScriptArrayHelper array(a,data);require(array.Num()<=65535,"pool reference array differs");for(int i=0;i<array.Num();++i)pool_references(a->GetInner(),array.GetRawPtr(i),refs,depth+1);return;}
 if(auto* m=CastField<FMapProperty>(p)){auto* map=static_cast<FScriptMap*>(data);auto layout=m->GetMapLayout();require(map->GetMaxIndex()<=65535,"pool reference map differs");for(int i=0;i<map->GetMaxIndex();++i)if(map->IsValidIndex(i)){auto* pair=static_cast<unsigned char*>(map->GetData(i,layout));pool_references(m->GetKeyProp(),pair,refs,depth+1);pool_references(m->GetValueProp(),pair+layout.ValueOffset,refs,depth+1);}return;}
 if(auto* s=CastField<FStructProperty>(p))for(auto* member:TFieldRange<FProperty>(s->GetStruct().Get()))pool_references(member,member->ContainerPtrToValuePtr<void>(data),refs,depth+1);
}
std::int64_t objective_pool_capture(UObject* gs) {
 using namespace RC::Unreal;require(enabled_&&IsInGameThreadRaw());pin(gs);
 require(gs->GetClassPrivate()->GetPathName()==L"/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectiveGameState.B_ObjectiveGameState_C","pool receiver differs");
 PoolReceipt receipt{pin(gs),pin(reinterpret_cast<UObject*>(gs->GetWorld())),{},{},false};
 for(auto name:{L"AllProps",L"ValidProps",L"Cauldrons",L"Proplist"}) {
  auto* p=field(gs->GetClassPrivate(),name,gs->GetClassPrivate()->GetStructureSize());
  require(p->GetArrayDim()==1&&p->GetSize()>0&&p->GetSize()<=1024,"pool capture schema differs");
  auto value=std::make_unique<PoolValue>(p);p->CopyCompleteValue(value->bytes.data(),p->ContainerPtrToValuePtr<void>(gs));pool_references(p,value->bytes.data(),receipt.references);receipt.values.push_back(std::move(value));
 }
 require(pool_receipts_.size()<64&&pool_receipt_serial_<9999999,"pool journal capacity exceeded");
 const auto id=++pool_receipt_serial_;pool_receipts_.emplace(id,std::move(receipt));return id;
}
bool objective_pool_restore(UObject* gs,std::int64_t id) {
 require(RC::Unreal::IsInGameThreadRaw());auto i=pool_receipts_.find(id);require(i!=pool_receipts_.end(),"pool receipt unavailable");auto& receipt=i->second;
 require(usable(receipt.receiver)&&usable(receipt.world)&&receipt.receiver.pointer==gs&&reinterpret_cast<UObject*>(gs->GetWorld())==receipt.world.pointer,"pool receipt lifetime retired");
 if(receipt.restored)return true;
 for(auto& reference:receipt.references)require(usable(reference),"pool baseline reference retired");
 // Snapshot the current containers too: failed inverse preserves both journals.
 const auto rollback=objective_pool_capture(gs);auto& previous=pool_receipts_.at(rollback);
 try {for(auto& v:receipt.values)v->field->CopyCompleteValue(v->field->ContainerPtrToValuePtr<void>(gs),v->bytes.data());}
 catch(...) {for(auto& v:previous.values)v->field->CopyCompleteValue(v->field->ContainerPtrToValuePtr<void>(gs),v->bytes.data());pool_receipts_.erase(rollback);throw;}
 pool_receipts_.erase(rollback);receipt.restored=true;return true;
}
void release_objective_pool(std::int64_t id){pool_receipts_.erase(id);}
// Apply the host's pool, never derive eligibility on a guest. Resolve the
// replicated logical IDs in this world, stage initialized property values,
// then commit all four caches. Missing actors leave the previous cache intact.
void objective_pool(UObject* gs,std::string_view body) {
 using namespace RC::Unreal;require(enabled_&&IsInGameThreadRaw()&&body.size()<=4194304,"objective pool input refused");pin(gs);
 require(gs->GetClassPrivate()->GetPathName()==L"/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectiveGameState.B_ObjectiveGameState_C","objective pool receiver differs");
 auto* world=reinterpret_cast<UObject*>(gs->GetWorld());pin(world);auto data=recipe::Parser(body,4194304).parse();
 std::map<std::uint32_t,UObject*> ids;std::map<std::string,UObject*> assets;
 auto logical_id=[&](UObject* actor)->std::uint32_t {
  auto bytes=read_bytes(actor,L"SavedMaterialIndices");if(bytes.size()<9)return 0;auto p=bytes.size()-9;
  if(bytes.substr(p,5)!=std::string("FAPI\1",5))return 0;std::uint32_t id{};std::memcpy(&id,bytes.data()+p+5,4);return id;
 };
 for(auto& path:data.at("classes").list()) {
  auto name=path.text();require(recipe::leaf_asset(source_text(name)),"invalid pool class");auto* cls=resident_asset(source_text(name));pin(cls);require(cls->IsA<UClass>(),"pool class unavailable");assets.emplace(name,cls);
  for(auto* actor:source_class_actors(world,cls)) {auto id=logical_id(actor);if(id)require(ids.emplace(id,actor).second,"duplicate pool identity");}
 }
 auto* potClass=resident_asset(L"/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectiveCauldron.B_ObjectiveCauldron_C");pin(potClass);
 std::map<std::uint32_t,UObject*> pots;
 for(auto* actor:source_class_actors(world,potClass)) {
  auto* p=CastField<FObjectProperty>(field(actor->GetClassPrivate(),L"Owner",actor->GetClassPrivate()->GetStructureSize()));require(p,"pot owner schema differs");
  auto* owner=p->GetObjectPropertyValue(p->ContainerPtrToValuePtr<void>(actor));if(!owner)continue;pin(owner);auto id=logical_id(owner);if(id)require(pots.emplace(id,actor).second,"duplicate pot binding");
 }
 auto resolve=[&](const Recipe& ref)->UObject* {
  if(auto* path=ref.get("asset")){auto i=assets.find(path->text());require(i!=assets.end(),"unlisted pool class");return i->second;}
  auto n=ref.at("id").numeric();require(n>0&&n<=4294967295.0&&n==std::floor(n),"invalid pool identity");
  auto& index=ref.get("pot")?pots:ids;auto i=index.find(static_cast<std::uint32_t>(n));require(i!=index.end(),"pool actor pending native replication");return i->second;
 };
 struct Staged {
  Property* field;std::vector<std::max_align_t> bytes;bool initialized{};
  explicit Staged(Property* p):field(p),bytes((p->GetSize()+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t)){field->InitializeValue(bytes.data());initialized=true;}
  ~Staged(){if(initialized)field->DestroyValue(bytes.data());}
 };
 std::vector<std::unique_ptr<Staged>> staged;
 for(auto name:{"AllProps","ValidProps","Cauldrons","Proplist"}) {
  auto* p=field(gs->GetClassPrivate(),source_text(name).c_str(),gs->GetClassPrivate()->GetStructureSize());require(p->GetArrayDim()==1&&p->GetSize()>0&&p->GetSize()<=1024,"pool property schema differs");
  auto value=std::make_unique<Staged>(p);source_value(p,value->bytes.data(),data.at(name),resolve);staged.push_back(std::move(value));
 }
 require(usable(pin(gs))&&reinterpret_cast<UObject*>(gs->GetWorld())==world,"pool world retired");
 for(auto& value:staged)value->field->CopyCompleteValue(value->field->ContainerPtrToValuePtr<void>(gs),value->bytes.data());
}
