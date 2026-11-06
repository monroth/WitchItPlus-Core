// Reflection writes initialized property VALUES only, never native object memory,
// BodyInstance runtime state, render resources, ownership or an actor Template.
using Recipe=recipe::Value;
static std::wstring source_text(std::string_view text) {
 std::wstring out;for(std::size_t i=0;i<text.size();) {unsigned c=static_cast<unsigned char>(text[i++]);
 if(c>=128){unsigned count=c<224?1:c<240?2:3;unsigned mask=count==1?31:count==2?15:7;c&=mask;for(unsigned n=0;n<count;++n){require(i<text.size(),"invalid recipe UTF-8");unsigned b=static_cast<unsigned char>(text[i++]);require((b&192)==128,"invalid recipe UTF-8");c=(c<<6)|(b&63);}}
 require(c<=0x10ffff&&!(c>=0xd800&&c<=0xdfff),"invalid recipe character");if constexpr(sizeof(wchar_t)==2){if(c>=65536){c-=65536;out+=wchar_t(0xd800+(c>>10));out+=wchar_t(0xdc00+(c&1023));continue;}}out+=wchar_t(c);}
 return out;
}
using SourceResolve=std::function<UObject*(const Recipe&)>;
static void source_number(Property* p,void* data,double value) {
 using namespace RC::Unreal;require(std::isfinite(value),"nonfinite source number");
 if(CastField<FFloatProperty>(p)){float v=static_cast<float>(value);require(std::isfinite(v),"source float overflow");std::memcpy(data,&v,sizeof(v));return;}
 if(CastField<FDoubleProperty>(p)){std::memcpy(data,&value,sizeof(value));return;}
 auto* numeric=CastField<FNumericProperty>(p);require(numeric,"source numeric schema differs");auto size=p->GetElementSize();require(value==std::floor(value)&&size>=1&&size<=8,"source integer width/value invalid");
 bool signedValue=CastField<FInt8Property>(p)||CastField<FInt16Property>(p)||CastField<FIntProperty>(p)||CastField<FInt64Property>(p);
 require(signedValue?(value>=-std::ldexp(1.0,size*8-1)&&value<std::ldexp(1.0,size*8-1)):(value>=0&&value<std::ldexp(1.0,size*8)),"source integer range invalid");
 if(value<0){auto v=static_cast<std::int64_t>(value);std::memcpy(data,&v,size);}
 else {require(value<18446744073709551616.0,"source integer overflow");auto v=static_cast<std::uint64_t>(value);std::memcpy(data,&v,size);}
}
static UObject* source_object_value(Property* p,const Recipe& value,const SourceResolve& resolve) {
 using namespace RC::Unreal;auto* object=value.kind==Recipe::Null?nullptr:resolve(value);
 auto* field=CastField<FObjectPropertyBase>(p);require(field,"source object property schema differs");
 auto* expected=field->GetPropertyClass().Get();require(expected&&(!object||object->IsA(expected)),"source object value class differs");return object;
}
static void source_value(Property* p,void* data,const Recipe& value,const SourceResolve& resolve,unsigned depth=0) {
 using namespace RC::Unreal;require(depth<48,"source property depth exceeded");
 if(value.get("null")){require(value.at("null").kind==Recipe::Bool&&value.at("null").boolean,"invalid source null");Recipe nil;source_value(p,data,nil,resolve,depth+1);return;}
 if(auto* e=CastField<FEnumProperty>(p)){source_number(e->GetUnderlyingProperty(),data,value.numeric());return;}
 if(auto* b=CastField<FBoolProperty>(p)){b->SetPropertyValue(data,value.logical());return;}
 if(CastField<FNumericProperty>(p)){source_number(p,data,value.numeric());return;}
 if(CastField<FObjectProperty>(p)){require(p->GetElementSize()==sizeof(UObject*),"source object width differs");auto* object=source_object_value(p,value,resolve);std::memcpy(data,&object,sizeof(object));return;}
 if(CastField<FWeakObjectProperty>(p)){require(p->GetElementSize()==sizeof(FWeakObjectPtr),"source weak width differs");*static_cast<FWeakObjectPtr*>(data)=source_object_value(p,value,resolve);return;}
 if(CastField<FSoftObjectProperty>(p)){require(p->GetElementSize()==sizeof(FSoftObjectPtr),"source soft width differs");FSoftObjectPtr v(source_object_value(p,value,resolve));*static_cast<FSoftObjectPtr*>(data)=v;return;}
 if(CastField<FNameProperty>(p)){*static_cast<FName*>(data)=FName(source_text(value.text()).c_str());return;}
 if(CastField<FStrProperty>(p)){*static_cast<FString*>(data)=FString(source_text(value.text()).c_str());return;}
 if(CastField<FTextProperty>(p)){auto text=source_text(value.at("text").text());FText v(text.c_str());p->CopyCompleteValue(data,&v);return;}
 if(auto* a=CastField<FArrayProperty>(p)){const auto& list=value.list();require(list.size()<65536,"source array limit");FScriptArrayHelper array(a,data);array.Resize(static_cast<int>(list.size()));for(unsigned i=0;i<list.size();++i)source_value(a->GetInner(),array.GetRawPtr(i),list[i],resolve,depth+1);return;}
 if(auto* s=CastField<FStructProperty>(p)){require(value.kind==Recipe::Object,"source struct expected");auto* type=s->GetStruct().Get();
 for(auto& [name,v]:value.object){if(recipe::engine_owned(name)||recipe::delegate_value(v))continue;auto* member=field(type,source_text(name).c_str(),type->GetStructureSize());try{source_value(member,member->ContainerPtrToValuePtr<void>(data),v,resolve,depth+1);}
 catch(const std::exception& e){throw std::runtime_error("source struct member "+name+": "+e.what());}}return;}
 if(auto* m=CastField<FMapProperty>(p)){auto& pairs=value.at("map").list();require(pairs.size()<65536,"source map limit");auto* map=static_cast<FScriptMap*>(data);auto layout=m->GetMapLayout();auto* k=m->GetKeyProp();auto* v=m->GetValueProp();
 for(int i=0;i<map->GetMaxIndex();++i)if(map->IsValidIndex(i)){auto* pair=static_cast<unsigned char*>(map->GetData(i,layout));k->DestroyValue(pair);v->DestroyValue(pair+layout.ValueOffset);}map->Empty(static_cast<int>(pairs.size()),layout);
 for(auto& entry:pairs){auto& values=entry.list();require(values.size()==2,"source map entry invalid");int i=map->AddUninitialized(layout);auto* pair=static_cast<unsigned char*>(map->GetData(i,layout));k->InitializeValue(pair);v->InitializeValue(pair+layout.ValueOffset);source_value(k,pair,values[0],resolve,depth+1);source_value(v,pair+layout.ValueOffset,values[1],resolve,depth+1);}map->Rehash(layout,[&](const void* key){return k->GetValueTypeHash(key);});return;}
 if(auto* set=CastField<FSetProperty>(p)){auto& values=value.list();FScriptSetHelper helper(set,data);helper.EmptyElements();require(values.size()<65536,"source set limit");for(auto& v:values){int i=helper.AddDefaultValue_Invalid_NeedsRehash();source_value(set->GetElementProp(),helper.GetElementPtr(i),v,resolve,depth+1);}static_cast<FScriptSet*>(data)->Rehash(set->GetSetLayout(),[&](const void* item){return set->GetElementProp()->GetValueTypeHash(item);});return;}
 // Delegate storage belongs to class construction/FinishSpawning. Do not bind,
 // clear or clone inline/sparse delegates (including Actor.OnDestroyed).
 throw std::runtime_error("unsupported reflected source property type");
}
static void source_properties(RC::Unreal::UStruct* type,void* object,const Recipe& properties,const SourceResolve& resolve) {
 using namespace RC::Unreal;
 for(auto& entry:properties.list()) {
  auto& slots=entry.list();require(slots.size()==3,"source property record invalid");auto name=source_text(slots[0].text());
  if(recipe::engine_owned(slots[0].text())||recipe::delegate_value(slots[2]))continue;
  try{auto* p=type->FindProperty(RC::Unreal::FName(name.c_str(),RC::Unreal::FNAME_Find));require(p&&p->GetSize()>0&&p->IsInContainer(type->GetStructureSize()),"source reflected field unavailable");int index=static_cast<int>(slots[1].numeric());require(index>=0&&index<p->GetArrayDim(),"source property array index invalid");source_value(p,p->ContainerPtrToValuePtr<void>(object,index),slots[2],resolve);}
  catch(const std::exception& e){throw std::runtime_error("source property "+std::string(name.begin(),name.end())+": "+e.what());}
 }
}

static const Recipe* source_property(const Recipe& properties,std::string_view name) {
 for(auto& entry:properties.list())if(entry.list()[0].text()==name)return &entry.list()[2];return nullptr;
}
static UObject* source_value_object(UObject* actor,const Recipe& value,const SourceResolve& resolve) {
 using namespace RC::Unreal;auto klass=source_text(value.at("class").text());require(recipe::leaf_asset(klass),"map-owned value class forbidden");
 auto* cls=resident_asset(klass);require(cls&&cls->IsA<UClass>(),"source value class unavailable");
 auto* objectClass=static_cast<UClass*>(cls);auto* component=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.ActorComponent");
 auto* actorClass=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.Actor");auto* world=UObjectGlobals::StaticFindObject<UClass*>(nullptr,nullptr,L"/Script/Engine.World");
 require(klass==L"/Script/Engine.BodySetup"||klass==L"/Script/Engine.MaterialInstanceDynamic","source value class is not a supported offline value object");
 require(!objectClass->IsChildOf(actorClass)&&!objectClass->IsChildOf(component)&&!objectClass->IsChildOf(world),"source value object has engine ownership");
 if(klass==L"/Script/Engine.MaterialInstanceDynamic") {
  auto* parent=source_property(value.at("p"),"Parent");require(parent&&parent->get("asset"),"source dynamic material must have a leaf parent");
  auto* lib=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__KismetMaterialLibrary");pin(lib);
  auto* fn=function(lib,L"CreateDynamicMaterialInstance",L"/Script/Engine.KismetMaterialLibrary:CreateDynamicMaterialInstance");native::Parameters args(fn);
  for(auto name:{L"WorldContextObject",L"Parent"}) {auto* p=CastField<FObjectProperty>(field(fn,name,fn->GetParmsSize()));require(p&&p->GetSize()==sizeof(UObject*),"dynamic material parameter differs");*static_cast<UObject**>(args.value(p))=name==std::wstring_view(L"Parent")?resolve(*parent):actor;}
  lib->ProcessEvent(fn,args.data());auto* p=CastField<FObjectProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));require(p,"dynamic material return missing");auto* object=p->GetObjectPropertyValue(args.value(p));pin(object);
  for(auto& entry:value.at("p").list()) {auto name=entry.list()[0].text();if(name=="Parent")continue;
   const wchar_t* method=name=="ScalarParameterValues"?L"SetScalarParameterValue":name=="VectorParameterValues"?L"SetVectorParameterValue":name=="TextureParameterValues"?L"SetTextureParameterValue":nullptr;
   if(!method){Recipe one;one.kind=Recipe::Array;one.array.push_back(entry);source_properties(object->GetClassPrivate(),object,one,resolve);continue;}
   auto path=std::wstring(L"/Script/Engine.MaterialInstanceDynamic:")+method;
   for(auto& param:entry.list()[2].list()) {auto& info=param.at("ParameterInfo");require(info.at("Association").numeric()==2,"source layered dynamic material requires parameter-info binding");
    auto* set=function(object,method,path.c_str());native::Parameters a(set);auto* n=field(set,L"ParameterName",set->GetParmsSize());*static_cast<FName*>(a.value(n))=FName(source_text(info.at("Name").text()).c_str());auto* v=field(set,L"Value",set->GetParmsSize());source_value(v,a.value(v),param.at("ParameterValue"),resolve);object->ProcessEvent(set,a.data());}
  }return object;
 }
 auto* outer=value.get("outer")?resolve(value.at("outer")):actor;pin(outer);
 FStaticConstructObjectParameters params{objectClass,outer};params.SetFlags=RF_Transient;auto* object=UObjectGlobals::StaticConstructObject(params);pin(object);return object;
}
static void source_object_properties(UObject* object,const Recipe& value,const SourceResolve& resolve) {
 if(value.at("class").text()=="/Script/Engine.MaterialInstanceDynamic")return;
 source_properties(object->GetClassPrivate(),object,value.at("p"),resolve);
}
