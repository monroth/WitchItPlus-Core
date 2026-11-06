native::PresetIndex preset_index_;
static std::wstring preset_text(const RC::Unreal::FString& value) {const auto length=value.Len();require(length>=0&&length<=32767,"preset native string size differs");return std::wstring(*value,static_cast<std::size_t>(length));}
Allocation preset_library_,preset_function_;
std::filesystem::path preset_root_;
std::filesystem::path preset_root() {
 using namespace RC::Unreal;
 if(!preset_root_.empty()&&usable(preset_library_)&&usable(preset_function_))return preset_root_;
 preset_root_.clear();
 auto* lib=UObjectGlobals::StaticFindObject<UObject*>(nullptr,nullptr,L"/Script/Engine.Default__KismetSystemLibrary");
 preset_library_=pin(lib);
 auto* fn=function(lib,L"GetProjectSavedDirectory",L"/Script/Engine.KismetSystemLibrary:GetProjectSavedDirectory");
 preset_function_=pin(fn);native::Parameters args(fn);
 auto* output=CastField<FStrProperty>(field(fn,L"ReturnValue",fn->GetParmsSize()));
 require(output&&output->HasAnyPropertyFlags(CPF_ReturnParm),"saved directory schema differs");
 lib->ProcessEvent(fn,args.data());
 preset_root_=std::filesystem::path(preset_text(*static_cast<FString*>(args.value(output))))/L"SaveGames"/L"GameRules";
 return preset_root_;
}
int preset_names(Stack& s,UObject* widget,std::size_t offset) {
 using namespace RC::Unreal;require(enabled_&&IsInGameThreadRaw());pin(widget);
 require(widget->GetClassPrivate()->GetPathName()==L"/Game/UserInterface/SaveLoad/WB_SaveLoadScreen.WB_SaveLoadScreen_C","preset screen schema differs");
 auto* folder=CastField<FStrProperty>(field(widget->GetClassPrivate(),L"FolderName",widget->GetClassPrivate()->GetStructureSize()));require(folder,"preset FolderName schema differs");
 require(preset_text(*folder->ContainerPtrToValuePtr<FString>(widget))==L"GameRules","preset folder differs");
 auto page=preset_index_.page(preset_root(),offset);s.table();auto table=s.top();s.string(page.version);s.set_field(table,"version");s.integer(page.names.size());s.set_field(table,"count");
 for(std::size_t n=0;n<page.names.size();++n){s.string(page.names[n]);s.set_field(table,std::to_string(n+1).c_str());}
 s.boolean(page.complete);return 2;
}
