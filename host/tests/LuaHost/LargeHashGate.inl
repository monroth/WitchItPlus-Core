// Real LuaMod VM -> registered C ABI closure -> protected_native_callback ->
// production HostStack -> production Native.sha256 dispatch -> Windows CNG.
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL p76_hash_callback(ue4ss_lua_context_handle) {
 try {
  ::witchit::uikit::HostStack stack{&LuaHost::HostApiRuntime::table()};
  if(stack.get_string(1)=="boot") {
   const auto bytes=stack.get_string(2);if(!bytes)return UE4SS_LUA_HOST_INTERNAL_ERROR;
   const auto controls=::witchit::boot::Controls::parse(*bytes);int enabled=0;
   for(const auto& name : ::witchit::boot::Controls::names)if(controls.enabled(name.c_str()))++enabled;
   stack.integer(enabled);return stack.api()->callback_return(stack.info().context,1);
  }
  if(stack.get_string(1)=="debug") {
   auto csv=stack.get_string(2);if(!csv)return UE4SS_LUA_HOST_INTERNAL_ERROR;
   ::witchit::debug::configure(*csv);bool disabled=!::witchit::debug::enabled("native_source_hooks");
   ::witchit::debug::configure(*csv,true); // unchanged activation is idempotent
   bool refused=false;try{::witchit::debug::configure("native_metadata",true);}catch(const std::exception&){refused=true;}
   ::witchit::debug::configure("");stack.boolean(disabled&&refused&&::witchit::debug::enabled("native_source_hooks"));
   return stack.api()->callback_return(stack.info().context,1);
  }
  if(stack.get_string(1)=="authored") {
   auto bytes=stack.get_string(2,1250000);if(!bytes)return UE4SS_LUA_HOST_INTERNAL_ERROR;
   auto value=::witchit::actors::recipe::authored_state(::witchit::actors::recipe::Parser(*bytes).parse());
   stack.boolean(value.at("p").list().size()==1&&value.at("p").list()[0].list()[0].text()=="TargetList"&&value.at("components").list()[0].at("p").list().size()==1);
   return stack.api()->callback_return(stack.info().context,1);
  }
  if(stack.get_string(1)=="brush") {
   auto bytes=stack.get_string(2,1250000);if(!bytes)return UE4SS_LUA_HOST_INTERNAL_ERROR;
   auto state=::witchit::actors::recipe::authored_state(::witchit::actors::recipe::Parser(*bytes).parse());
   const auto& convex=state.at("objects").at("o5830").at("p").list()[0].list()[2].at("ConvexElems").list()[0];
   stack.boolean(convex.at("ElemBox").at("IsValid").logical()&&convex.at("CollisionEnabled").numeric()==3
    &&convex.at("VertexData").list().size()==8&&convex.at("IndexData").list().size()==36);
   return stack.api()->callback_return(stack.info().context,1);
  }
  if(stack.get_string(1)=="recipe") {
   auto bytes=stack.get_string(2,1250000);if(!bytes)return UE4SS_LUA_HOST_INTERNAL_ERROR;
   auto value=::witchit::actors::recipe::Parser(*bytes).parse();stack.string(value.at("binding").at("name").text());
   return stack.api()->callback_return(stack.info().context,1);
  }
  if(stack.get_string(1)!="sha256")return UE4SS_LUA_HOST_INTERNAL_ERROR;
  return stack.api()->callback_return(stack.info().context,::witchit::native::sha256_stack(stack));
 }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
// Actual Lua stack -> production diagnostic entry, including named failures.
static ue4ss_lua_host_status UE4SS_LUA_HOST_CALL p81_debug_callback(ue4ss_lua_context_handle) {
 try {
  ::witchit::uikit::HostStack stack{&LuaHost::HostApiRuntime::table(),UE4SS_LUA_HOST_CAP_SCALARS_V1};
  return stack.api()->callback_return(stack.info().context,::witchit::debug::configure_stack(stack));
 }catch(...){return UE4SS_LUA_HOST_INTERNAL_ERROR;}
}
static void p76_hash_gate() {
 using namespace LuaHost;
 Review4Vm fixture{L"P76LargeCompanion"};auto& lua=fixture.lua();auto& host=HostBindings::instance();
 auto vm=host.find_vm(lua.get_root_lua_state());auto module=host.attach_module(code_image(reinterpret_cast<const void*>(&p76_hash_callback)));
 review4_check(vm.status==Status::ok&&module.status==Status::ok,"P76 hash module/VM unavailable");
 {
  ModLuaEntry entry{lua};auto stack=host.stack_binding(lua.get_root_lua_state(),lua.get_lua_state());
  review4_check(stack.status==Status::ok,"P76 actual stack unavailable");
  auto status=host.api().dispatch_lua(vm.value,module.value,stack.value,[&](auto){
   const auto& api=HostApiRuntime::table();ue4ss_lua_callback_handle callback{};
   ::witchit::uikit::host_require(api.register_callback(p76_hash_callback,&callback));
   ::witchit::uikit::HostStack values{&api};values.callback(callback,0);lua_setglobal(lua.get_lua_state(),"P76Native");
   ue4ss_lua_callback_handle debugCallback{};
   ::witchit::uikit::host_require(api.register_callback(p81_debug_callback,&debugCallback));
   values.callback(debugCallback,0);lua_setglobal(lua.get_lua_state(),"P81Debug");
   lua.set_string(to_utf8_string(round6_source));lua_setglobal(lua.get_lua_state(),"P76Source");
   lua.execute_string(R"(
local json=dofile(P76Source..'/runtime-source/WitchItRuntime/Scripts/json.lua')
local bytes='{"format":"FAPP_SOURCE_GEOMETRY","version":2,"binding":{"world":"/Game/Maps/GameMaps/Tower/Tower","folder":"Tower","name":"Large"},"actors":[],"padding":"'..string.rep('a',410000)..'"}'
assert(#bytes>400000 and json.decode(bytes,4194304).binding.name=='Large')
assert(P76Native('sha256',bytes)=='06a39d6bc54444019df30e72b351f4d11596fd40203680fb41facf26c4c70968')
assert(P76Native('sha256',string.rep('a',1000000))=='cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0')
assert(P76Native('recipe',bytes)=='Large')
local legacy='{"p":[["OnDestroyed",0,{"delegates":[{"target":{"ref":"ChildActor"},"function":"OnChildActorDestroyed"}]}],["TargetList",0,[{"ref":"shelf"}]]],"components":[{"p":[["StreamingTextureData",0,[1,2]],["LightGuid",0,{"A":1}],["StaticMesh",0,{"asset":"/Game/Mesh.M"}]]}],"objects":{}}'
for i=1,300 do assert(P76Native('authored',legacy))end
local brush='{"assets":[],"components":[{"class":"/Script/Engine.BrushComponent","name":"BrushComponent0","p":[["BrushBodySetup",0,{"object":"o5830"}]],"parent":"","render_defaults":{},"root":true}],"external":{},"objects":{"o5830":{"class":"/Script/Engine.BodySetup","outer":{"ref":"/Game/Maps/GameMaps/PropLayouts/Tower02.Tower02:PersistentLevel.BlockingVolume_0.BrushComponent0"},"p":[["AggGeom",0,{"ConvexElems":[{"CollisionEnabled":3,"ElemBox":{"IsValid":1,"Max":{"X":25.0,"Y":157.5,"Z":175.0},"Min":{"X":-25.0,"Y":-157.5,"Z":-175.0}},"IndexData":[5,1,2,3,0,6,0,2,1,7,5,2,5,0,1,4,5,6,7,6,5,7,2,6,3,2,0,3,6,2,4,0,5,4,6,0],"Name":"None","RestOffset":0.0,"Transform":{"Rotation":{"W":1.0,"X":0.0,"Y":0.0,"Z":0.0},"Scale3D":{"X":1.0,"Y":1.0,"Z":1.0},"Translation":{}},"VertexData":[{"X":-25.0,"Y":157.5,"Z":-175.0},{"X":-25.0,"Y":157.5,"Z":175.0},{"X":-25.0,"Y":-157.5,"Z":175.0},{"X":-25.0,"Y":-157.5,"Z":-175.0},{"X":25.0,"Y":157.5,"Z":-175.0},{"X":25.0,"Y":157.5,"Z":175.0},{"X":25.0,"Y":-157.5,"Z":-175.0},{"X":25.0,"Y":-157.5,"Z":175.0}],"bContributeToMass":true}]}],["bGenerateMirroredCollision",0,false],["CollisionTraceFlag",0,2]]}},"p":[["BrushType",0,1],["BrushComponent",0,{"ref":"/Game/Maps/GameMaps/PropLayouts/Tower02.Tower02:PersistentLevel.BlockingVolume_0.BrushComponent0"}],["bHidden",0,false],["SpawnCollisionHandlingMethod",0,1]]}'
for i=1,300 do assert(P76Native('brush',brush))end
local invalid_brush=json.decode(brush);invalid_brush.objects.o5830.p[1][3].ConvexElems[1].ElemBox.IsValid=2
assert(not pcall(P76Native,'brush',json.encode(invalid_brush)))
assert(P76Native('brush',brush))
assert(P76Native('boot','{}')==11)
assert(P76Native('boot',json.encode({producer_disable=json.array{'all'},core_boot='manifest_only'}))==0)
assert(P76Native('boot',json.encode({producer_disable=json.array{'viewport','ship_binding'}}))==9)
assert(not pcall(P76Native,'boot',json.encode({producer_disable=json.array{'native_boundaries'}})))
assert(P76Native('boot','{}')==11)
assert(P76Native('debug','native_source_hooks,native_tick'))
local ok,why=P81Debug('native_boundaries');assert(ok==false and why=='unknown native diagnostic switch: native_boundaries')
assert(P81Debug(''))
assert(#P76Native('sha256',string.rep('x',4194304))==64)
assert(not pcall(P76Native,'sha256',string.rep('x',4194305)))
assert(P76Native('sha256','abc')=='ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
)");
   return ue4ss_lua_host_status{UE4SS_LUA_HOST_OK};
  });review4_check(status==UE4SS_LUA_HOST_OK,"P76 actual native hash callback failed");
 }
 fixture.close();
 review4_check(host.claim_module_retire(module.value)==Status::ready,"P76 module retire failed");
 review4_check(host.cleanup_module(module.value)==Status::ok&&host.finish_module_retire(module.value)==Status::ok,"P76 module cleanup failed");
 std::cout<<"P86 BRUSH ABI PASS real LuaMod VM: 300 exact Tower02 AggGeom bool-byte conversions; numeric collision/8 vertices/36 indices retained\n";
 std::cout<<"P82 BOOT ABI PASS production Producer parser, 11 guards + named rejection; P81 DEBUG ABI PASS production named rejection; P80 DEBUG ABI PASS production activation flags; P79 AUTHORED ABI PASS 300 legacy delegate/render-state rows; P76 HASH ABI PASS real LuaMod/protected_native_callback/HostStack/CNG: 410156-byte companion/property parser, 1000000, 4194304, cap+1 rejected, callback remains usable\n";
}
