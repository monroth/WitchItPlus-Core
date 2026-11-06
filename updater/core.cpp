#include "../consumer/native/include/FilePolicy.hpp"
#include "core.hpp"
#include "vendor/monocypher.h"
#include "vendor/monocypher-ed25519.h"
#include "vendor/picosha2.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif
namespace wip {
void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
// The built-in packages, then those a signed Core release has named as dependencies.
static std::vector<Package>& registry(){static std::vector<Package> p={
 {"core","Core","monroth/WitchItPlus-Core",""},
 {"editor-tweaks","BaseWorldPlus","monroth/WitchItPlus-EditorTweaks","ue4ss/Mods/BaseWorldPlus/"},
 {"fillapot-plus","FillAPotPlus","monroth/WitchItPlus-FaPplus","ue4ss/Mods/FillAPotPlus/"},
 {"matchrules-plus","MatchRulesPlusPlus","monroth/WitchItPlus-MatchRulesPP","ue4ss/Mods/MatchRulesPlusPlus/"}};return p;}
const std::vector<Package>& packages(){return registry();}
const Package& package(const std::string& id){for(auto& p:packages())if(p.id==id)return p;throw std::runtime_error("unknown public package "+id);}
void fields(const Json& j,std::initializer_list<const char*> names){require(j.is_object()&&j.size()==names.size(),"metadata fields");for(auto n:names)require(j.contains(n),std::string("missing field ")+n);}
// A dependency is a public package on the same signing key, with its own Mods folder.
static Package dependency(const Json& j){
 fields(j,{"id","canonical","repo"});auto id=j["id"].get<std::string>(),canonical=j["canonical"].get<std::string>(),repo=j["repo"].get<std::string>();
 require(std::regex_match(id,std::regex("[a-z0-9-]{1,40}"))&&std::regex_match(canonical,std::regex("[A-Za-z0-9]{1,40}"))&&std::regex_match(repo,std::regex("monroth/WitchItPlus-[A-Za-z0-9-]{1,40}")),"dependency identity");
 return {id,canonical,repo,"ue4ss/Mods/"+canonical+"/"};
}
Json parse(const std::string& s){
 require(!s.empty()&&s.size()<=metadata_cap&&s.compare(0,3,"\xef\xbb\xbf")!=0,"metadata size/encoding");
 std::map<int,std::set<std::string>> keys;
 auto callback=[&](int d,Json::parse_event_t e,Json& x){require(d<=24,"metadata depth");
  if(e==Json::parse_event_t::object_start)keys[d+1].clear();
  if(e==Json::parse_event_t::key)require(keys[d].insert(lower(x.get<std::string>())).second,"duplicate JSON key");return true;};
 return Json::parse(s,callback,true,false);
}
std::string hash(const std::string& s){return picosha2::hash256_hex_string(s);}
std::string read(const fs::path& p,size_t cap){std::ifstream f(p,std::ios::binary);require(bool(f),"read "+p.string());f.seekg(0,std::ios::end);auto n=f.tellg();require(n>=0&&uint64_t(n)<=cap,"file cap "+p.string());f.seekg(0);std::string b(size_t(n),0);f.read(b.data(),b.size());require(bool(f),"incomplete read "+p.string());return b;}
std::string file_hash(const fs::path& p){std::ifstream f(p,std::ios::binary);require(bool(f),"hash read "+p.string());picosha2::hash256_one_by_one h;std::array<char,65536>b;while(f){f.read(b.data(),b.size());h.process(b.begin(),b.begin()+f.gcount());}require(f.eof(),"hash incomplete");h.finish();return picosha2::get_hash_hex_string(h);}
void atomic_write(const fs::path& p,const std::string& b){
 fs::create_directories(p.parent_path());auto tmp=p;tmp+=".writing";
#ifdef _WIN32
 HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);require(h!=INVALID_HANDLE_VALUE,"create atomic temporary");DWORD n=0;bool ok=WriteFile(h,b.data(),DWORD(b.size()),&n,nullptr)&&n==b.size()&&FlushFileBuffers(h);CloseHandle(h);require(ok,"flush atomic temporary");require(MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"publish atomic file");
#else
 int h=::open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);require(h>=0,"create atomic temporary");size_t at=0;while(at<b.size()){auto n=::write(h,b.data()+at,b.size()-at);if(n<=0){::close(h);throw std::runtime_error("write atomic temporary");}at+=n;}int ok=::fsync(h);::close(h);require(ok==0,"flush atomic temporary");fs::rename(tmp,p);h=::open(p.parent_path().c_str(),O_RDONLY|O_DIRECTORY);if(h>=0){::fsync(h);::close(h);}
#endif
}
std::string safe_path(const std::string& p){require(!p.empty()&&p.size()<=240&&std::regex_match(p,std::regex("[A-Za-z0-9_./-]+")),"unsafe path characters");std::stringstream in(p);std::string s;require(p.front()!='/'&&p.back()!='/',"absolute/empty path");while(std::getline(in,s,'/')){require(!s.empty()&&s!="."&&s!=".."&&s.back()!='.',"unsafe path segment");auto stem=lower(s.substr(0,s.find('.')));require(!std::regex_match(stem,std::regex("(con|prn|aux|nul|com[0-9]|lpt[0-9])")),"reserved Windows name");}return p;}
void owned_path(const Package& pkg,const std::string& name,const std::string& mode){
 auto p=lower(safe_path(name));require(mode=="managed"||mode=="seed","file mode");
 require((mode=="seed")==witchit::file_policy::seed(name),"file ownership policy differs");
 auto ends=[&](const char* x){return p.size()>=strlen(x)&&p.compare(p.size()-strlen(x),strlen(x),x)==0;};
 // The modpack's own maps ship as they are; layouts anywhere else are a player's.
 const bool bundled=p.rfind("ue4ss/mods/fillapotplus/bundled-maps/",0)==0;
 require((!ends(".ini")||mode=="seed")&&(bundled||(!ends(".pl")&&!ends(".fapteleports")&&!ends(".faphidden")&&!ends(".fapcatalog")&&!ends(".shipparts"))),"personal file refused");
 for(auto part:{"/saved/","/savegames/","/privatecosmetics/","/creativeunlocks/","/presets/","/storage/","/config/"})require(p.find(part)==std::string::npos,"personal directory refused");
 for(auto word:{"mods.txt","mods.json","witchit-update-settings.json","witchit-update-request.json"})if(p==std::string("ue4ss/mods/")+word||p==std::string("ue4ss/")+word){require(pkg.id=="core"&&mode=="seed","registration/settings must be core seed");return;}
 if(p=="ue4ss/ue4ss-settings.ini"){require(pkg.id=="core"&&mode=="seed","INI must be seed");return;}
 if(pkg.id!="core"){require(p.rfind(lower(pkg.root),0)==0,"file outside package");return;}
 const std::vector<std::string> roots={"ue4ss/mods/core/","ue4ss/mods/uikit/","ue4ss/mods/witchitruntime/","ue4ss/mods/witchitnativeproducer/","ue4ss/mods/shared/","ue4ss/mods/keybinds/","ue4ss/mods/actorDumpermod/","ue4ss/mods/bpmodloaderMod/","ue4ss/mods/consolecommandsmod/","ue4ss/mods/consoleenablermod/","ue4ss/mods/split-screen/","ue4ss/mods/customgameconfigs/","ue4ss/mods/lineTraceMod/","ue4ss/mods/cheatmanagerenablermod/","ue4ss/mods/bpml_genericfunctions/","ue4ss/mods/splitscreenmod/","ue4ss/ue4ss_sdk_backends/","ue4ss/mods/luaActor/","ue4ss/sdKBackends/","ue4ss/updater/"};
 if(p=="dwmapi.dll"||p=="ue4ss/ue4ss.dll"||p=="ue4ss/license"||p=="ue4ss/native-build-receipt.tsv"||p=="ue4ss/witchitupdater.exe")return;
 for(auto& r:roots)if(p.rfind(lower(r),0)==0)return;
 throw std::runtime_error("file outside core payload: "+name);
}
static std::array<uint32_t,3> version(const std::string& s){require(std::regex_match(s,std::regex("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)")),"invalid stable version");std::stringstream in(s);std::string part;std::array<uint32_t,3> v{};for(auto& x:v){std::getline(in,part,'.');auto n=std::stoull(part);require(n<=2147483647,"version overflow");x=uint32_t(n);}return v;}
int version_compare(const std::string& a,const std::string& b){auto x=version(a),y=version(b);return x<y?-1:x>y?1:0;}
static std::string b64(const std::string& s){const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";require(s.size()%4==0&&!s.empty(),"invalid base64 length");std::string out;unsigned val=0;int bits=-8;bool padding=false;for(char c:s){if(c=='='){padding=true;continue;}require(!padding,"base64 padding");auto n=alphabet.find(c);require(n!=std::string::npos,"base64 character");val=(val<<6)|unsigned(n);bits+=6;if(bits>=0){out.push_back(char(val>>bits));bits-=8;}}return out;}
void signature(const std::string& bytes,const std::string& sig){
#ifdef WIP_TEST_PUBLIC_KEY
 const std::string pub=b64(WIP_TEST_PUBLIC_KEY);
#else
 const std::string pub=b64("RWREY0y19Re05jPOpntGcFh6GYCWAuyXtairJE8Y/9tn/xwqpidQs+YO");
#endif
 require(sig.size()<=4096&&pub.size()==42,"signature/key size");std::stringstream in(sig);std::string a,b,c,d,extra;
 require(bool(std::getline(in,a)&&std::getline(in,b)&&std::getline(in,c)&&std::getline(in,d)),"truncated minisign signature");
 for(auto* s:{&a,&b,&c,&d})if(!s->empty()&&s->back()=='\r')s->pop_back();
 while(std::getline(in,extra))require(extra.empty()||extra=="\r","signature trailing lines");
 require(a.rfind("untrusted comment: ",0)==0&&c.rfind("trusted comment: ",0)==0,"minisign comment format");
 auto raw=b64(b),global=b64(d);require(raw.size()==74&&global.size()==64&&raw.substr(0,2)=="ED"&&raw.substr(2,8)==pub.substr(2,8),"signature algorithm/key mismatch");
 uint8_t digest[64];crypto_blake2b(digest,64,reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size());
 require(crypto_ed25519_check(reinterpret_cast<const uint8_t*>(raw.data()+10),reinterpret_cast<const uint8_t*>(pub.data()+10),digest,64)==0,"Ed25519 signature rejected");
 auto signed_comment=raw.substr(10)+c.substr(17);
 require(crypto_ed25519_check(reinterpret_cast<const uint8_t*>(global.data()),reinterpret_cast<const uint8_t*>(pub.data()+10),reinterpret_cast<const uint8_t*>(signed_comment.data()),signed_comment.size())==0,"minisign trusted comment rejected");
}
static void digest_field(const Json& v){require(v.is_string()&&std::regex_match(v.get<std::string>(),std::regex("[0-9a-f]{64}")),"invalid SHA256");}
static uint64_t number(const Json& j,uint64_t max){require(j.is_number_unsigned()&&j.get<uint64_t>()<=max,"invalid unsigned number");return j.get<uint64_t>();}
static void basename(const Json& j){auto s=j.get<std::string>();safe_path(s);require(s.find('/')==s.npos,"asset is not basename");}
Json catalog(const Package& p,const std::string& bytes,const std::string& sig,uint64_t seen){
 signature(bytes,sig);auto j=parse(bytes);fields(j,{"schema","repository","sequence","releases"});require(j["schema"]==1&&j["repository"]==p.repo,"catalog identity");require(number(j["sequence"],INT64_MAX)>=seen,"catalog sequence rollback");require(j["releases"].is_array()&&j["releases"].size()<=4096,"catalog release cap");std::set<std::string> versions;
 for(auto& e:j["releases"]){fields(e,{"version","tag","manifest","signature","manifest_sha256"});version(e["version"]);require(versions.insert(e["version"]).second,"duplicate release");for(auto n:{"tag","manifest","signature"})basename(e[n]);digest_field(e["manifest_sha256"]);}return j;
}
Json manifest(const Package& p,const std::string& bytes,const std::string& sig,const Json& entry){
 require(hash(bytes)==entry["manifest_sha256"].get<std::string>(),"manifest SHA256 mismatch");signature(bytes,sig);auto j=parse(bytes);auto dependencies=j.is_object()&&j.contains("dependencies")?j["dependencies"]:Json::array();if(j.is_object())j.erase("dependencies");
 require(dependencies.is_array()&&dependencies.size()<=16&&(p.id=="core"||dependencies.empty()),"manifest dependencies");for(auto& d:dependencies)dependency(d);
 fields(j,{"schema","repository","package_id","version","source_commit","platform","updater_protocol","requires","asset","files","removals","migration_policy"});
 require(j["schema"]==1&&j["repository"]==p.repo&&j["package_id"]==p.id&&j["version"]==entry["version"]&&j["platform"]=="windows-x64"&&j["updater_protocol"]==1&&j["migration_policy"]=="payload-only-reversible","manifest identity/protocol");
 require(j["source_commit"].is_string()&&std::regex_match(j["source_commit"].get<std::string>(),std::regex("[0-9a-f]{40}")),"source commit");
 fields(j["asset"],{"name","bytes","sha256"});basename(j["asset"]["name"]);require(number(j["asset"]["bytes"],zip_cap)>0,"empty archive");digest_field(j["asset"]["sha256"]);
 if(p.id=="core")fields(j["requires"],{});else{fields(j["requires"],{"core","runtime_abi"});version(j["requires"]["core"]);require(j["requires"]["runtime_abi"].is_string(),"runtime ABI");}
 require(j["files"].is_array()&&!j["files"].empty()&&j["files"].size()<=4096,"file count");uint64_t total=0;std::set<std::string> paths;
 for(auto& f:j["files"]){fields(f,{"path","bytes","sha256","mode"});owned_path(p,f["path"],f["mode"]);require(paths.insert(lower(f["path"])).second,"case-colliding file");total+=number(f["bytes"],512*1024*1024);require(total<=1024ULL*1024*1024,"expanded archive cap");digest_field(f["sha256"]);}
 fields(j["removals"],{"files","packages"});require(j["removals"]["packages"]==Json::array()&&j["removals"]["files"].is_array()&&j["removals"]["files"].size()<=4096,"explicit file removals only");
 for(auto& f:j["removals"]["files"]){owned_path(p,f);require(paths.insert(lower(f)).second,"removal collision");}j["dependencies"]=dependencies;return j;
}
static fs::path home(const fs::path& r){return r/"ue4ss/.witchit-updater";}
static void remember(const Package& p){for(auto& known:registry())if(known.id==p.id||known.canonical==p.canonical){require(known.id==p.id&&known.canonical==p.canonical&&known.repo==p.repo,"dependency differs from a known package");return;}registry().push_back(p);}
static void known_dependencies(const fs::path& r){auto p=home(r)/"packages.json";if(!fs::exists(p))return;auto list=parse(read(p));require(list.is_array(),"dependency list");for(auto& d:list)remember(dependency(d));}
// Interprocess exclusion covers publication and recovery, including an updater
// left downloading after the preceding game process has exited.
struct Lock {
#ifdef _WIN32
 HANDLE handle{INVALID_HANDLE_VALUE};
 explicit Lock(const fs::path& p){handle=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);}
 explicit operator bool()const{return handle!=INVALID_HANDLE_VALUE;}
 ~Lock(){if(*this)CloseHandle(handle);}
#else
 int handle{-1};bool held{};
 explicit Lock(const fs::path& p){handle=::open(p.c_str(),O_CREAT|O_RDWR|O_NOFOLLOW,0600);held=handle>=0&&::flock(handle,LOCK_EX|LOCK_NB)==0;}
 explicit operator bool()const{return held;}
 ~Lock(){if(handle>=0)::close(handle);}
#endif
 Lock(const Lock&)=delete;
};

void log(const fs::path& r,const std::string& s){auto p=home(r)/"updater.log";fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::app);f<<std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())<<" "<<s<<std::endl;}
Json state(const fs::path& r){auto p=home(r)/"state.json";if(fs::exists(p))return parse(read(p));return Json{{"sequences",Json::object()},{"blocked",Json::array()}};}
Json installed(const fs::path& r){known_dependencies(r);auto p=home(r)/"installed.json";if(!fs::exists(p))p=r/"ue4ss/updater/installed.json";auto j=parse(read(p));fields(j,{"packages","runtime_abi"});require(j["packages"].is_object()&&j["packages"].contains("core"),"installed version receipt");for(auto& item:j["packages"].items()){package(item.key());version(item.value());}return j;}
// Core ships UIKit/Runtime together. Features require an explicit runtime
// enabled switch, never mere presence in the installed receipt. Old settings
// conservatively check Core only until the runtime publishes current switches.
static std::set<std::string> enabled_packages(const fs::path& r){
 known_dependencies(r);std::set<std::string> enabled{"core"};auto path=r/"ue4ss/witchit-update-settings.json";
 // A dependency is on unless the player switched it off.
 for(size_t i=4;i<packages().size();++i)enabled.insert(packages()[i].id);
 if(!fs::exists(path))return enabled;
 auto settings=parse(read(path,16384));
 if(!settings.contains("enabled_packages"))return enabled;
 require(settings["enabled_packages"].is_object(),"enabled package switches");
 for(auto& item:settings["enabled_packages"].items()){
  require(item.value().is_boolean(),"enabled package switch");bool known=false;
  for(auto& pkg:packages())if(item.key()==pkg.canonical){known=true;if(item.value().get<bool>())enabled.insert(pkg.id);else enabled.erase(pkg.id);}
  require(known,"unknown enabled package");
 }
 return enabled;
}
static std::string release_key(const Json& m){return m["package_id"].get<std::string>()+"@"+m["version"].get<std::string>();}
static void file_verify(const fs::path& p,const Json& f){require(fs::is_regular_file(p)&&fs::file_size(p)==f["bytes"].get<uint64_t>()&&file_hash(p)==f["sha256"].get<std::string>(),"payload hash/size mismatch: "+p.string());}
Json verified_plan(const fs::path& r,const fs::path& pending,const Json& st){
 auto index=parse(read(pending/"index.json"));fields(index,{"bundles"});require(index["bundles"].is_array()&&!index["bundles"].empty()&&index["bundles"].size()<=20,"pending bundle count");
 auto enabled=enabled_packages(r);auto old=installed(r),next=old;Json result={{"actions",Json::array()},{"releases",Json::array()}};std::set<std::string> all,ids;
 for(auto& id:index["bundles"]){auto& pkg=package(id);require(enabled.count(pkg.id),"pending package is disabled");require(ids.insert(pkg.id).second,"duplicate bundle");auto dir=pending/pkg.id;
  auto seq=st["sequences"].value(pkg.id,uint64_t(0));auto cat=catalog(pkg,read(dir/"catalog.json"),read(dir/"catalog.json.minisig",4096),seq);
  auto selected=parse(read(dir/"selected.json"));require(selected.is_string(),"selected version");Json entry;
  for(auto& e:cat["releases"])if(e["version"]==selected)entry=e;require(!entry.is_null(),"release absent from signed catalog");
  auto m=manifest(pkg,read(dir/"manifest.json"),read(dir/"manifest.json.minisig",4096),entry);
  require(!old["packages"].contains(pkg.id)||version_compare(m["version"],old["packages"][pkg.id])>0,"not a newer version");
  for(auto& bad:st["blocked"])require(bad!=release_key(m),"quarantined release");
  require(fs::file_size(dir/"payload.zip")==m["asset"]["bytes"].get<uint64_t>()&&file_hash(dir/"payload.zip")==m["asset"]["sha256"].get<std::string>(),"ZIP hash/size mismatch");
  // A Core release brings the packages it names: they arrive with it or the update stops.
  for(auto& d:m["dependencies"])require(old["packages"].contains(d["id"].get<std::string>())||std::find(index["bundles"].begin(),index["bundles"].end(),d["id"])!=index["bundles"].end(),"core update misses a dependency");
  result["releases"].push_back(m);next["packages"][pkg.id]=m["version"];if(pkg.id=="core")next["runtime_abi"]="witchit-"+m["source_commit"].get<std::string>();
  for(auto& f:m["files"]){auto path=f["path"].get<std::string>();require(all.insert(lower(path)).second,"cross-package file collision");file_verify(dir/"files"/path,f);
   auto dst=r/path;require(!fs::exists(dst)||fs::is_regular_file(dst),"destination not regular file");if(f["mode"]=="seed"&&fs::exists(dst))continue;
   result["actions"].push_back({{"path",path},{"source",pkg.id+"/files/"+path},{"sha256",f["sha256"]},{"existed",fs::exists(dst)}});
  }
  for(auto& f:m["removals"]["files"]){auto path=f.get<std::string>();require(all.insert(lower(path)).second,"cross-package removal collision");auto dst=r/path;require(!fs::exists(dst)||fs::is_regular_file(dst),"removal not regular file");if(fs::exists(dst))result["actions"].push_back({{"path",path},{"source",""},{"existed",true}});}
 }
 // Mixed old/new feature sets must still target the chosen host ABI/core. The
 // installed receipt records bindings, not just versions, to avoid ABI guesses.
 auto bindings=fs::exists(home(r)/"bindings.json")?parse(read(home(r)/"bindings.json")):Json::object();
 for(auto& m:result["releases"])if(m["package_id"]!="core")bindings[m["package_id"].get<std::string>()]=m["requires"];
 for(auto& b:bindings.items())if(enabled.count(b.key())&&next["packages"].contains(b.key()))require(b.value()["core"]==next["packages"]["core"]&&b.value()["runtime_abi"]==next["runtime_abi"],"release set not ABI-compatible");
 if(ids.count("core"))for(auto& p:old["packages"].items())if(p.key()!="core"&&enabled.count(p.key()))require(ids.count(p.key()),"core update requires matching enabled features");
 // The receipt binds every managed dependency after the transaction, including
 // untouched packages. A feature-only update cannot silently invalidate it.
 const std::string receipt_name="ue4ss/native-build-receipt.tsv";
 auto receipt_path=r/receipt_name;
 for(auto& a:result["actions"])if(a["path"]==receipt_name)receipt_path=pending/a["source"].get<std::string>();
 require(fs::is_regular_file(receipt_path),"complete native build receipt required");
 std::map<std::string,std::string> receipt;std::istringstream receipt_stream(read(receipt_path));std::string line;
 while(std::getline(receipt_stream,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();auto at=line.find('\t');require(at!=line.npos&&receipt.emplace(line.substr(0,at),line.substr(at+1)).second,"invalid native build receipt");}
 require(receipt["protocol"]=="1"&&receipt["profile"]=="public"&&receipt["linked_tests"]=="PASS","native build receipt not accepted public profile");
 require(receipt["core_graph"]=="PASS","Core graph acceptance absent");
 for(auto name:{"source","sdk","toolchain","configuration","linked_evidence","core_evidence"})digest_field(receipt[name]);
 std::size_t outputs=0;
 for(auto& [key,value]:receipt)if(key.rfind("output:",0)==0){
  auto name=key.substr(7);require(name=="dwmapi.dll"||name=="ue4ss/UE4SS.dll"||name=="ue4ss/WitchItUpdater.exe"||name=="ue4ss/Mods/WitchItNativeProducer/dlls/main.dll","unexpected native output");
  digest_field(value);require(receipt["file:"+name]==value,"native output inventory differs");++outputs;
 }
 require(outputs==4,"mandatory native output set incomplete");
 for(auto& a:result["actions"])if(!a["source"].get<std::string>().empty()&&a["path"]!=receipt_name&&!witchit::file_policy::seed(a["path"].get<std::string>())){
  auto path=a["path"].get<std::string>();auto key="file:"+path;
  require(receipt.count(key)&&receipt[key]==a["sha256"].get<std::string>(),"updated file absent from complete build closure: "+path);
 }
 for(auto name:{"dwmapi.dll","ue4ss/UE4SS.dll","ue4ss/WitchItUpdater.exe","ue4ss/Mods/WitchItNativeProducer/dlls/main.dll","ue4ss/Mods/WitchItRuntime/Scripts/main.lua","ue4ss/Mods/Core/package.lua","ue4ss/Mods/UIKit/package.lua","ue4ss/Mods/BaseWorldPlus/package.lua","ue4ss/Mods/FillAPotPlus/package.lua","ue4ss/Mods/MatchRulesPlusPlus/package.lua"})require(receipt.count(std::string("file:")+name),"incomplete public build closure");
 if(fs::exists(r/receipt_name)){
  std::istringstream previous_receipt(read(r/receipt_name));std::string previous_line;
  while(std::getline(previous_receipt,previous_line)){
   if(!previous_line.empty()&&previous_line.back()=='\r')previous_line.pop_back();
   require(previous_line!="profile\tprivate-full","Private-full installation requires a local rebuild/install; public updater refused before mutation");
   if(previous_line.rfind("file:",0)==0){
   auto at=previous_line.find('\t');require(at!=previous_line.npos,"invalid previous build closure");auto key=previous_line.substr(0,at);
   if(!receipt.count(key)&&!witchit::file_policy::seed(key.substr(5))){bool removed=false;for(auto& a:result["actions"])if(a["path"]==key.substr(5)&&a["source"]=="")removed=true;require(removed,"retired receipt dependency needs explicit removal: "+key.substr(5));}
  }}
 }
 for(auto& [key,digest]:receipt)if(key.rfind("file:",0)==0){
  auto name=safe_path(key.substr(5));require(!witchit::file_policy::seed(name),"seed in immutable build closure");digest_field(digest);auto path=r/name;
  for(auto& a:result["actions"])if(a["path"]==name){require(!a["source"].get<std::string>().empty(),"receipt dependency removed");path=pending/a["source"].get<std::string>();break;}
  require(fs::is_regular_file(path)&&file_hash(path)==digest,"release breaks complete source/native closure: "+name);
 }
 result["installed"]=next;result["bindings"]=bindings;result["notice_version"]=result["releases"].front()["version"];return result;
}
static void move_file(const fs::path& a,const fs::path& b){fs::create_directories(b.parent_path());
#ifdef _WIN32
 require(MoveFileExW(a.c_str(),b.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"rename replace failed (possibly locked): "+b.string());
#else
 fs::rename(a,b);
#endif
#ifdef WIP_TEST_PUBLIC_KEY
 static int moves=0;const char* fault=std::getenv("WIP_TEST_CRASH_AFTER_MOVE");if(fault&&++moves==std::atoi(fault))std::_Exit(75);
#endif
}
static void rollback(const fs::path& r,Json j){auto h=home(r);for(auto i=j["actions"].rbegin();i!=j["actions"].rend();++i){auto path=(*i)["path"].get<std::string>();safe_path(path);auto dst=r/path,backup=h/"backup/files"/path;
  if(fs::exists(backup)){if(fs::exists(dst))move_file(dst,h/("rollback-discard-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))/path);move_file(backup,dst);}else if(!(*i)["existed"].get<bool>()&&fs::exists(dst)){require(i->contains("sha256")&&file_hash(dst)==(*i)["sha256"].get<std::string>(),"rollback found modified new file");fs::remove(dst);}
 }
 atomic_write(h/"installed.json",j["previous"].dump());if(j.contains("previous_bindings"))atomic_write(h/"bindings.json",j["previous_bindings"].dump());
 auto st=state(r);for(auto& m:j["releases"])if(std::find(st["blocked"].begin(),st["blocked"].end(),release_key(m))==st["blocked"].end())st["blocked"].push_back(release_key(m));atomic_write(h/"state.json",st.dump());
 fs::remove(h/"trial.json");fs::remove(h/"notice.json");fs::remove(h/"journal.json");if(fs::exists(h/"pending"))fs::remove_all(h/"pending");log(r,"activation rolled back; failed releases quarantined");
}
// A damaged staged file must not leave a perpetual restart notice or be
// offered again. Quarantine only identities authenticated by the pinned signer.
static void quarantine_staged(const fs::path& r,const fs::path& pending){
 auto st=state(r);Json index;try{index=parse(read(pending/"index.json"));fields(index,{"bundles"});require(index["bundles"].is_array()&&index["bundles"].size()<=4,"quarantine bundle cap");}catch(const std::exception&){return;}
 for(auto& id:index["bundles"]){try{auto& p=package(id);auto dir=pending/p.id;auto c=catalog(p,read(dir/"catalog.json"),read(dir/"catalog.json.minisig",4096),0);auto selected=parse(read(dir/"selected.json"));Json entry;
   for(auto& e:c["releases"])if(e["version"]==selected)entry=e;require(!entry.is_null(),"quarantine release identity");auto m=manifest(p,read(dir/"manifest.json"),read(dir/"manifest.json.minisig",4096),entry);
   auto key=release_key(m);if(std::find(st["blocked"].begin(),st["blocked"].end(),key)==st["blocked"].end())st["blocked"].push_back(key);
  }catch(const std::exception&){/* unauthenticated identities cannot change policy */}}
 atomic_write(home(r)/"state.json",st.dump());
}
void activate(const fs::path& r){auto h=home(r);fs::create_directories(h);Lock lock(h/"check.lock");if(!lock){log(r,"previous worker still downloading; activation deferred");return;}
 if(fs::exists(h/"journal.json")){auto j=parse(read(h/"journal.json"));
  // The menu check writes this marker: the mods came up and the game reached its menu.
  if(fs::exists(h/"ready.marker")){fs::remove(h/"ready.marker");fs::remove(h/"journal.json");fs::remove(h/"trial.json");log(r,"trial accepted; previous version backup retained");}
  else rollback(r,j);
 }
 if(!fs::exists(h/"pending/index.json"))return;
 auto st=state(r);Json plan;
 try{plan=verified_plan(r,h/"pending",st);}catch(const std::exception& e){log(r,std::string("staged update refused: ")+e.what());quarantine_staged(r,h/"pending");fs::remove(h/"notice.json");fs::rename(h/"pending",h/("rejected-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())));return;}
 if(fs::exists(h/"backup"))fs::remove_all(h/"backup");fs::create_directories(h/"backup");
 plan["previous"]=installed(r);plan["previous_bindings"]=fs::exists(h/"bindings.json")?parse(read(h/"bindings.json")):Json::object();plan["phase"]="applying";
 atomic_write(h/"journal.json",plan.dump());
 try{for(auto& a:plan["actions"]){auto p=a["path"].get<std::string>();auto dst=r/p;
   if(a["existed"].get<bool>())move_file(dst,h/"backup/files"/p);
   if(!a["source"].get<std::string>().empty()){auto src=h/"pending"/a["source"].get<std::string>();require(file_hash(src)==a["sha256"].get<std::string>(),"staged file changed during activation");auto tmp=dst;tmp+=".witchit-new";fs::create_directories(dst.parent_path());fs::copy_file(src,tmp,fs::copy_options::overwrite_existing);require(file_hash(tmp)==a["sha256"].get<std::string>(),"activation copy hash");move_file(tmp,dst);}
  }
  atomic_write(h/"installed.json",plan["installed"].dump());atomic_write(h/"bindings.json",plan["bindings"].dump());
  fs::remove(h/"ready.marker");
  atomic_write(h/"trial.json","{}");
  atomic_write(h/"notice.json",Json{{"kind","updated"},{"version",plan["notice_version"]}}.dump());
  fs::remove_all(h/"pending");log(r,"activation complete; trial awaiting the game menu");
 }catch(...){rollback(r,plan);throw;}
}
// One payload at a time, for the Mods tab: which package, how far, of how much.
static fs::path progress_path;static Json progress_row;
void download_progress(uint64_t done){progress_row["done"]=done;atomic_write(progress_path,progress_row.dump());}
void check(const fs::path& r,const Fetch& fetch){auto h=home(r);fs::create_directories(h);Lock lock(h/"check.lock");require(bool(lock),"another updater is active");if(fs::exists(h/"pending"))return;
 auto settings=r/"ue4ss/witchit-update-settings.json";bool automatic=true;if(fs::exists(settings)){auto s=parse(read(settings,16384));require(s.is_object()&&s.contains("auto_update")&&s["auto_update"].is_boolean(),"update setting");automatic=s["auto_update"];}
 Json request;auto rp=r/"ue4ss/witchit-update-request.json";if(fs::exists(rp)){
  request=parse(read(rp,16384));fields(request,{"request","packages","created_utc"});require(request["request"]=="latest"||request["request"]=="exact","request kind");require(request["packages"].is_object(),"request package set");
  auto date=request["created_utc"].get<std::string>();require(std::regex_match(date,std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z")),"request date");std::tm time{};std::istringstream in(date);in>>std::get_time(&time,"%Y-%m-%dT%H:%M:%SZ");require(!in.fail(),"request UTC");
#ifdef _WIN32
  auto utc=_mkgmtime(&time);
#else
  auto utc=timegm(&time);
#endif
  auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());require(utc>=now-7*86400&&utc<=now+300,"expired/future request");
  if(request["request"]=="latest")require(request["packages"].empty(),"latest request must be empty");
  else {require(request["packages"].contains("Core"),"exact request requires Core");for(auto& item:request["packages"].items()){bool known=false;for(auto& pkg:packages())if(item.key()==pkg.canonical)known=true;require(known,"non-public request package");version(item.value());}}
 }
 // Without auto-update the check only reports what a download would bring.
 const bool download=automatic||!request.is_null();
 auto enabled=enabled_packages(r);auto old=installed(r),st=state(r);auto tmp=h/"download";if(fs::exists(tmp))fs::remove_all(tmp);fs::create_directories(tmp);Json index={{"bundles",Json::array()}};Json next=old;Json offered=Json::array();uint64_t bytes=0;
 std::set<std::string> needed;auto list=packages();
 try{for(size_t at=0;at<list.size();++at){const auto pkg=list[at];bool fresh=!old["packages"].contains(pkg.id);
   // Disabled or absent packages cause no catalog, manifest or payload request,
   // unless the Core release being downloaded needs them.
   if(!enabled.count(pkg.id)||(fresh&&!needed.count(pkg.id)))continue;
   auto base="https://github.com/"+pkg.repo+"/releases/";auto cb=fetch(base+"latest/download/catalog.json",metadata_cap);auto cs=fetch(base+"latest/download/catalog.json.minisig",4096);
   auto cat=catalog(pkg,cb,cs,st["sequences"].value(pkg.id,uint64_t(0)));st["sequences"][pkg.id]=cat["sequence"];atomic_write(h/"state.json",st.dump());
   Json chosen;
   for(auto& entry:cat["releases"]){if(!fresh&&version_compare(entry["version"],old["packages"][pkg.id])<=0)continue;
    if(!fresh&&!request.is_null()&&request["request"]=="exact"){require(request["packages"].contains(pkg.canonical),"exact request missing installed public package");if(request["packages"][pkg.canonical]!=entry["version"])continue;}
    if(chosen.is_null()||version_compare(entry["version"],chosen["version"])>0)chosen=entry;
   }
   require(!fresh||!chosen.is_null(),"dependency has no release");if(chosen.is_null())continue;bool blocked=false;for(auto& bad:st["blocked"])if(bad==pkg.id+"@"+chosen["version"].get<std::string>())blocked=true;if(blocked)continue;
   auto dir=tmp/pkg.id;fs::create_directories(dir);atomic_write(dir/"catalog.json",cb);atomic_write(dir/"catalog.json.minisig",cs);atomic_write(dir/"selected.json",chosen["version"].dump());
   auto release=base+"download/"+chosen["tag"].get<std::string>()+"/";auto mb=fetch(release+chosen["manifest"].get<std::string>(),metadata_cap);auto ms=fetch(release+chosen["signature"].get<std::string>(),4096);auto m=manifest(pkg,mb,ms,chosen);
   atomic_write(dir/"manifest.json",mb);atomic_write(dir/"manifest.json.minisig",ms);
   for(auto& d:m["dependencies"]){auto extra=dependency(d);remember(extra);if(std::none_of(list.begin(),list.end(),[&](const Package& p){return p.id==extra.id;}))list.push_back(extra);if(!old["packages"].contains(extra.id)){needed.insert(extra.id);enabled.insert(extra.id);}}
   offered.push_back(pkg.canonical+" "+m["version"].get<std::string>());bytes+=m["asset"]["bytes"].get<uint64_t>();if(!download)continue;
   progress_path=h/"progress.json";progress_row={{"package",pkg.canonical},{"total",m["asset"]["bytes"]}};download_progress(0);
   auto zip=fetch(release+m["asset"]["name"].get<std::string>(),zip_cap);fs::remove(progress_path);require(zip.size()==m["asset"]["bytes"].get<uint64_t>()&&hash(zip)==m["asset"]["sha256"].get<std::string>(),"download hash/size mismatch");atomic_write(dir/"payload.zip",zip);extract(dir/"payload.zip",dir/"files",m);index["bundles"].push_back(pkg.id);next["packages"][pkg.id]=m["version"];
  }
  if(!download&&!offered.empty()){fs::remove_all(tmp);atomic_write(h/"notice.json",Json{{"kind","available"},{"packages",offered},{"bytes",bytes}}.dump());log(r,"update available");return;}
  if(index["bundles"].empty()){fs::remove_all(tmp);if(!request.is_null())fs::remove(rp);log(r,"update check complete; no newer release");return;}
  atomic_write(tmp/"index.json",index.dump());
  Json extras=Json::array();for(size_t i=4;i<packages().size();++i)extras.push_back({{"id",packages()[i].id},{"canonical",packages()[i].canonical},{"repo",packages()[i].repo}});
  atomic_write(h/"packages.json",extras.dump());
  auto plan=verified_plan(r,tmp,st);move_file(tmp,h/"pending");
  // Features may update independently; show the actual downloaded version.
  auto shown=plan["releases"].front()["version"];atomic_write(h/"notice.json",Json{{"kind","downloaded"},{"version",shown},{"packages",offered},{"bytes",bytes}}.dump());if(!request.is_null())fs::remove(rp);log(r,"signed update staged; live files unchanged");
 }catch(...){if(!progress_path.empty())fs::remove(progress_path);if(fs::exists(tmp))fs::remove_all(tmp);throw;}
}
}
// Only a half-finished transaction may stop the game; any other failure just skips updates.
extern "C" int witchit_update_bootstrap(const wchar_t* root){const std::filesystem::path r(root);try{wip::activate(r);return 0;}catch(const std::exception& e){try{wip::log(r,std::string("bootstrap failed: ")+e.what());}catch(...){}std::error_code none;return std::filesystem::exists(r/"ue4ss/.witchit-updater/journal.json",none)?1:0;}}
