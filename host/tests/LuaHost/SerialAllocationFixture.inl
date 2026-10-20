using int32=int;using uint8=unsigned char;
constexpr int FNAME_Find=0;
#define STR(x) L##x
#define checkSlow(x) do{if(!(x))throw std::runtime_error("null item");}while(false)
struct FName {FName(const wchar_t*,int){}};
static int initialized,destroyed,converted,copies,serial;
static bool fail_call;static int result_size=32,result_offset=8;
struct UObject;struct UFunction;
struct FProperty {
 bool result{};
 int GetOffset_Internal(){return result?result_offset:0;}
 int GetSize(){return result?result_size:8;}
 void InitializeValue_InContainer(void* p){++initialized;std::memset(static_cast<char*>(p)+result_offset,0,result_size);}
 void DestroyValue_InContainer(void* p){++destroyed;auto* bytes=static_cast<unsigned char*>(p)+result_offset;if(!fail_call && bytes[0]!=0x09)throw std::runtime_error("engine return not preserved");}
 template<class T>T* ContainerPtrToValuePtr(void* p){return reinterpret_cast<T*>(static_cast<char*>(p)+GetOffset_Internal());}
};
static FProperty input{},output{true};
struct UFunction {FProperty* FindProperty(FName){return &input;}FProperty* GetReturnProperty(){return &output;}int GetParmsSize(){return 64;}};
static UFunction function;
struct UObject {
 void ProcessEvent(UFunction*,void* p){
  ++converted;auto* object=*reinterpret_cast<UObject**>(p);if(!object)throw std::runtime_error("input lost");
  if(fail_call)throw std::runtime_error("engine error");
  // Captured crash source: adjacent engine fields, not an SDK FString pointer.
  std::uint64_t poison=0x1000000009ull;std::memcpy(static_cast<char*>(p)+result_offset,&poison,8);serial=71;
 }
};
static UObject object,self;
struct UObjectGlobals {template<class T>static T StaticFindObject(void*,void*,const wchar_t*){if constexpr(std::is_same_v<T,UFunction*>)return &function;else return &self;}};
struct FUObjectItem {int& GetSerialNumber(){return serial;}UObject* GetUObject(){return &object;}};
static FUObjectItem item;
struct FUObjectArray {static FUObjectItem* IndexToObject(int){return &item;}static int AllocateSerialNumber(int);};
struct UKismetSystemLibrary {static void Conv_ObjectToSoftObjectReference(UObject*){++copies;throw std::runtime_error("FORBIDDEN_SOFT_REFERENCE_COPY");}};
#undef checkSlow
#define checkSlow(x) do{if(!(x))throw std::runtime_error("null item");}while(false)
