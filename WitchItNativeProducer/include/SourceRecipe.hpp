#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <stdexcept>
#include <cmath>
#include <charconv>
namespace witchit::actors::recipe {
// Bounded, strict JSON for offline-extracted property values. No engine calls.
struct Value {
 enum Kind {Null,Bool,Number,String,Array,Object} kind{Null};
 bool boolean{};double number{};std::string string;std::vector<Value> array;std::map<std::string,Value> object;
 const Value& at(const std::string& key)const{auto i=object.find(key);if(kind!=Object||i==object.end())throw std::runtime_error("recipe field missing: "+key);return i->second;}
 const Value* get(const std::string& key)const{auto i=object.find(key);return i==object.end()?nullptr:&i->second;}
 const std::string& text()const{if(kind!=String)throw std::runtime_error("recipe string expected");return string;}
 const std::vector<Value>& list()const{if(kind!=Array)throw std::runtime_error("recipe array expected");return array;}
 // Native-serialized Box.IsValid is a byte in cooked assets even though the
 // reflected field is FBoolProperty. Convert only canonical 0/1 at a reflected
 // boolean destination; numeric fields retain their numeric type.
 bool logical()const{
  if(kind==Bool)return boolean;
  if(kind==Number&&(number==0||number==1))return number==1;
  throw std::runtime_error("source boolean expected (true/false or 0/1)");
 }
 double numeric()const{if(kind!=Number||!std::isfinite(number))throw std::runtime_error("finite recipe number expected");return number;}
};
class Parser {
 std::string_view input;std::size_t pos{},nodes{};
 [[noreturn]]void bad()const{throw std::runtime_error("invalid source property JSON at byte "+std::to_string(pos));}
 void space(){while(pos<input.size()&&(input[pos]==' '||input[pos]=='\n'||input[pos]=='\r'||input[pos]=='\t'))++pos;}
 char take(){if(pos==input.size())bad();return input[pos++];}
 unsigned hex(){unsigned n=0;for(int i=0;i<4;++i){char c=take();unsigned k=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;if(k>15)bad();n=n*16+k;}return n;}
 static void utf8(std::string& s,unsigned c){if(c<128)s+=char(c);else if(c<2048){s+=char(192|(c>>6));s+=char(128|(c&63));}else if(c<65536){s+=char(224|(c>>12));s+=char(128|((c>>6)&63));s+=char(128|(c&63));}else{s+=char(240|(c>>18));s+=char(128|((c>>12)&63));s+=char(128|((c>>6)&63));s+=char(128|(c&63));}}
 std::string quoted(){if(take()!='"')bad();std::string out;while(true){unsigned char c=take();if(c=='"')return out;if(c<32)bad();if(c!='\\'){out+=char(c);continue;}char e=take();switch(e){case '"':case '\\':case '/':out+=e;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;case 'u':{unsigned u=hex();if(u>=0xd800&&u<=0xdbff){if(take()!='\\'||take()!='u')bad();auto v=hex();if(v<0xdc00||v>0xdfff)bad();u=65536+((u-0xd800)<<10)+(v-0xdc00);}else if(u>=0xdc00&&u<=0xdfff)bad();utf8(out,u);break;}default:bad();}}}
 Value value(unsigned depth){space();if(depth>48||++nodes>262144)bad();Value v;char c=pos<input.size()?input[pos]:0;
 if(c=='"'){v.kind=Value::String;v.string=quoted();return v;}
 if(c=='['||c=='{'){++pos;bool obj=c=='{';v.kind=obj?Value::Object:Value::Array;space();if(pos<input.size()&&input[pos]==(obj?'}':']')){++pos;return v;}
 while(true){std::string key;if(obj){space();key=quoted();space();if(take()!=':')bad();}auto child=value(depth+1);if(obj){if(!v.object.emplace(key,std::move(child)).second)bad();}else v.array.push_back(std::move(child));space();char end=take();if(end==(obj?'}':']'))return v;if(end!=',')bad();}}
 for(auto word:{std::string_view("true"),std::string_view("false"),std::string_view("null")})if(input.substr(pos,word.size())==word){pos+=word.size();v.kind=word=="null"?Value::Null:Value::Bool;v.boolean=word=="true";return v;}
 std::size_t start=pos;if(c=='-')++pos;if(pos>=input.size())bad();if(input[pos]=='0')++pos;else{if(input[pos]<'1'||input[pos]>'9')bad();while(pos<input.size()&&input[pos]>='0'&&input[pos]<='9')++pos;}
 if(pos<input.size()&&input[pos]=='.'){++pos;auto digits=pos;while(pos<input.size()&&input[pos]>='0'&&input[pos]<='9')++pos;if(pos==digits)bad();}
 if(pos<input.size()&&(input[pos]=='e'||input[pos]=='E')){++pos;if(pos<input.size()&&(input[pos]=='+'||input[pos]=='-'))++pos;auto digits=pos;while(pos<input.size()&&input[pos]>='0'&&input[pos]<='9')++pos;if(pos==digits)bad();}
 auto result=std::from_chars(input.data()+start,input.data()+pos,v.number);if(result.ec!=std::errc()||!std::isfinite(v.number))bad();v.kind=Value::Number;return v;
 }
public:
 explicit Parser(std::string_view text,std::size_t limit=1250000):input(text){if(text.size()>limit)throw std::runtime_error("source property JSON exceeds cap");}
 Value parse(){auto v=value(0);space();if(pos!=input.size())bad();return v;}
};
// Reflected serialization is not an authored-state contract. These fields are
// engine-owned build/runtime data; a new component must generate its own.
inline bool engine_owned(std::string_view name) {
 for(auto key:{"LODData","MapBuildDataId","MapBuildData","LightmapData","LightMap","ShadowMap",
  "LightGuid","OriginalLightGuid","PreviewShadowMapChannel","StreamingTextureData",
  "bIsValidTextureStreamingBuiltData","bHasNoStreamableTextures","LightmassSettings",
  "bOverrideLightMapRes","OverriddenLightMapRes","OverrideVertexColors","OverrideColors",
  "VertexColorOverride","PerInstanceSMData","PerInstanceRenderData","CachedMaxDrawDistance",
  "NaniteResources","NaniteOverrideMaterials","RuntimeVirtualTextures","VirtualTextureRenderPassType",
  "PrimaryComponentTick","PrimaryActorTick","SkinnedAsset","ClothingSimulationFactory",
  "OverrideParameters","UserParameterRedirects","SortedParameterOffsets","ParameterOffsets"})if(name==key)return true;
 return false;
}
inline bool delegate_value(const Value& v) {
 if(v.get("delegates"))return true;
 for(auto& [key,item]:v.object)if(delegate_value(item))return true;
 for(auto& item:v.array)if(delegate_value(item))return true;
 return false;
}
inline Value authored_value(const Value& v) {
 Value out=v;
 if(out.kind==Value::Object)for(auto i=out.object.begin();i!=out.object.end();) {
  if(engine_owned(i->first)||delegate_value(i->second))i=out.object.erase(i);
  else{i->second=authored_value(i->second);++i;}
 }
 else if(out.kind==Value::Array)for(auto& item:out.array)item=authored_value(item);
 return out;
}
inline Value authored_properties(const Value& properties) {
 Value out;out.kind=Value::Array;
 for(auto& entry:properties.list()) {
  auto& p=entry.list();if(p.size()!=3)throw std::runtime_error("source property record invalid");
  if(engine_owned(p[0].text())||delegate_value(p[2]))continue;
  auto clean=entry;clean.array[2]=authored_value(p[2]);out.array.push_back(std::move(clean));
 }
 return out;
}
inline Value authored_state(Value state) {
 state.object.at("p")=authored_properties(state.at("p"));
 for(auto& c:state.object.at("components").array) {
  c.object.at("p")=authored_properties(c.at("p"));
  if(auto* defaults=c.get("render_defaults"))for(auto& [name,value]:defaults->object) {
   const auto& properties=c.at("p").list();
   if(std::none_of(properties.begin(),properties.end(),[&](const auto& p){return p.list()[0].text()==name;})) {
    Value key;key.kind=Value::String;key.string=name;Value index;index.kind=Value::Number;index.number=0;
    Value entry;entry.kind=Value::Array;entry.array={key,index,value};c.object.at("p").array.push_back(std::move(entry));
   }
  }
  if(auto* type=c.get("class");type && type->text()=="/Script/Engine.DecalComponent") {
   const auto& properties=c.at("p").list();
   auto material=std::find_if(properties.begin(),properties.end(),[](const auto& p){return p.list()[0].text()=="DecalMaterial";});
   if(material==properties.end() || (!material->list()[2].get("asset") && !material->list()[2].get("object")))throw std::runtime_error("decal material missing; update scenery recipe");
  }
 }
 for(auto& [name,o]:state.object.at("objects").object)o.object.at("p")=authored_properties(o.at("p"));
 return state;
}
inline bool leaf_asset(std::wstring_view path){
 // Map directories hold UWorlds and level-owned exports, never runtime inputs.
 auto lower=std::wstring(path);for(auto& c:lower)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';
 return !lower.starts_with(L"/game/maps/")&&lower.find(L"wir_sourcearchive")==std::wstring::npos;
}
}
