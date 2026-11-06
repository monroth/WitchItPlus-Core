// Witch It+ signed update protocol. Engine-independent; shared by worker/bootstrap/tests.
#pragma once
#include "vendor/json.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace wip {
namespace fs=std::filesystem;
using Json=nlohmann::json;
constexpr size_t metadata_cap=4*1024*1024, zip_cap=128*1024*1024;
struct Package {std::string id,canonical,repo,root;};
const std::vector<Package>& packages();
const Package& package(const std::string& id);
void require(bool,const std::string&);
std::string lower(std::string);
Json parse(const std::string&);
void fields(const Json&,std::initializer_list<const char*>);
std::string hash(const std::string&);
std::string file_hash(const fs::path&);
std::string read(const fs::path&,size_t cap=metadata_cap);
void atomic_write(const fs::path&,const std::string&);
std::string safe_path(const std::string&);
void owned_path(const Package&,const std::string&,const std::string& mode="managed");
int version_compare(const std::string&,const std::string&);
void signature(const std::string&,const std::string&);
Json catalog(const Package&,const std::string&,const std::string&,uint64_t seen);
Json manifest(const Package&,const std::string&,const std::string&,const Json& entry);
void extract(const fs::path& zip,const fs::path& destination,const Json& manifest);
// Requires every signed catalog, signed manifest, archive, and extracted file again.
Json verified_plan(const fs::path& root,const fs::path& pending,const Json& state);
Json installed(const fs::path& root);
Json state(const fs::path& root);
void log(const fs::path& root,const std::string&);
// Called synchronously by the proxy BEFORE loading UE4SS.dll. No engine, network,
// thread creation, or process creation. Throws on incomplete recovery (fail closed).
void activate(const fs::path& root);
using Fetch=std::function<std::string(const std::string&,size_t)>;
void check(const fs::path& root,const Fetch& fetch);
void download_progress(uint64_t done);
}
extern "C" int witchit_update_bootstrap(const wchar_t* root);
