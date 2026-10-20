-- Local source scenery. File observations never admit/veto vanilla IO. Cooked
-- components are constructed centrally; Lua owns only layout/world lifetime.
local M={LIMIT=1100000}
local function clean(s) return type(s)=='string' and #s>0 and #s<=180 and not s:find('[%c/\\:]') and s~='.' and s~='..' end
local function text(v)if type(v)=='string'then return v end;return v and v:ToString()or ''end
local function leaf(s)return s:gsub('\\','/'):match('([^/]+)$')end
local function basename(s)return leaf(s):gsub('%.pl$','')end
function M.decode(json,bytes,binding)
 assert(type(bytes)=='string' and #bytes<=M.LIMIT,'source geometry exceeds limit')
 local d=json.decode(bytes,M.LIMIT)
 assert(d.format=='FAPP_SOURCE_GEOMETRY' and d.version==1,'unsupported source geometry version')
 local b=d.binding;assert(type(b)=='table' and clean(b.folder) and clean(b.name) and type(b.world)=='string' and b.world:match('^/Game/'),'source geometry binding invalid')
 if binding then for _,key in ipairs({'world','folder','name'})do assert(b[key]==binding[key],'source geometry binding differs: '..key)end end
 assert(type(d.actors)=='table' and #d.actors<=4096,'source geometry actor limit')
 local seen={}
 for _,row in ipairs(d.actors)do
  assert(type(row.source)=='string' and row.source:match('^/Game/[%w_/]+%.[%w_]+:PersistentLevel%.[%w_]+$')and not seen[row.source],'source geometry actor path invalid/duplicate')
  assert(row.asset==row.source:match('^(.-):PersistentLevel%.'),'source geometry asset differs')
  seen[row.source]=true
  if row.scene_components then
   assert(type(row.scene_components)=='table' and #row.scene_components>0 and #row.scene_components<4096,'source scene subobject limit')
   local names={}
   for _,name in ipairs(row.scene_components)do assert(type(name)=='string' and #name<256 and name:match('^[%w_]+$') and not names[name],'invalid source scene subobject');names[name]=true end
  end
  if row.transform then
   assert(#row.transform==9,'source geometry transform invalid')
   for _,v in ipairs(row.transform)do assert(type(v)=='number'and v==v and math.abs(v)<1e9,'source geometry transform nonfinite')end
  end
 end
 return d
end
function M.install(api,Core)
 local R=Core.runtime;local E=api.Native.scope(api.id)
 local Net=api.module('source_geometry_sync');local hash=api.module('content_hash');local wire
 local current,pending,context,worldKey,closed=nil,nil,nil,nil,false
 local serial=0;local lastError
 local frozen={};local active={};local releasing=false;local bindingPath
 local function log(s)api.log('SourceGeometry: '..s)end
 local function valid(o)return o and o:IsValid()end
 local function key(o)return o:GetAddress()end
 local function releasePhysics()
  local old=frozen;frozen={};releasing=true
  for _,c in ipairs(old)do if valid(c)then local ok,why=pcall(c.SetSimulatePhysics,c,true);if not ok then log('physics restore failed: '..tostring(why))end end end
  releasing=false
 end
 local function freeze()
  if not valid(context) or context:HasAuthority()~=true or #frozen>0 then return end
  local cls=StaticFindObject('/Script/Engine.PrimitiveComponent');if not valid(cls)then return end
  local props,why=E.find('/Game/Blueprints/Props/Bases/B_StaticMeshProp.B_StaticMeshProp_C',context);assert(props,why)
  for _,actor in ipairs(props)do
   for _,c in ipairs(R.values(actor:K2_GetComponentsByClass(cls)))do
    c=R.unwrap(c)
    if valid(c)and c:IsSimulatingPhysics(FName('None'))then
     -- Record before mutation so a later failure can restore every touched body.
     frozen[#frozen+1]=c;c:SetSimulatePhysics(false)
    end
   end
  end
 end
 local function clear(broadcast)
  if broadcast and wire then wire.publish(nil)end
  releasePhysics()
  for i=#active,1,-1 do local ok,why=E.destroy(active[i]);if not ok then error('source scenery cleanup: '..tostring(why),0)end;active[i]=nil end
  local old=(pending or current)
  if old then
   local assets={};for _,row in ipairs(old.data.actors)do assets[row.asset]=true end
   for asset in pairs(assets)do E.release_asset(asset)end
  end
  current=nil;pending=nil;bindingPath=nil;lastError=nil
 end
 local function failure(why)
  local message=tostring(why)
  if message~=lastError then log('unapplied: '..message);lastError=message end
  if pending then pending.error=message end
  releasePhysics() -- no engine IO or player input is vetoed by a broken file
 end
 local function localPath(b)return api.path..'/geometry/'..b.folder..'/'..b.name..'.pl.fapgeometry'end
 local function sourceFile(event)
  if not event.target_path or event.target_path==''then return end
  local path=event.target_path:gsub('\\','/');local folder=path:match('/([^/]+)/[^/]+$');local name=basename(path)
  if not clean(folder)or not clean(name)then return end
  local bytes=Core.companion_io.read(path..'.fapgeometry',M.LIMIT)
  -- Bundled repaired companions also work for workshop/received layouts, where
  -- Steam transmits the .pl but cannot transmit arbitrary companion extensions.
  if not bytes then bytes=Core.companion_io.read(localPath{folder=folder,name=name},M.LIMIT)end
  if not bytes then return end
  local d=M.decode(api.json,bytes)
  assert(d.binding.folder==folder and d.binding.name==name,'source geometry filename binding differs')
  if event.world and event.world~=''then assert(leaf(d.binding.world)==leaf(event.world),'source geometry world differs')end
  return d,path
 end
 local function request(data,path,fromHost,digest)
  local id=digest or hash(Net.compact(api.json,data))
  if current and current.id==id and not pending then return end
  if pending and pending.id==id then return end
  clear();pending={data=data,id=id,assets={},at=1,elapsed=0,failures={},fromHost=fromHost};bindingPath=path
  for _,row in ipairs(data.actors)do pending.assets[row.asset]=false end
  -- Start each streamable request once; subsequent ticks only inspect residency.
  for asset in pairs(pending.assets)do pending.assets[asset]=E.asset(asset)~=nil end
  log('loading '..data.binding.folder..'/'..data.binding.name..' actors='..#data.actors..(fromHost and ' (from host)'or ''))
  if wire and not fromHost then wire.publish(data)end
 end
 local function event(e)
  local connection=Core.package_sync.snapshot()
  if connection and connection.pc_key and not connection.host then return end -- host owns the played recipe
  if e.kind=='load' and (e.phase=='entry' or e.complete and e.success)then
   if e.phase=='entry'then clear(true)end
   local data,path=sourceFile(e)
   if data then request(data,path)
   elseif e.complete and e.success then clear(true)end
   if e.complete and e.success and pending then freeze()end
  elseif e.kind=='save' and e.complete and e.success then
   local d=(pending or current)and (pending or current).data
   if pending and pending.error or lastError and not d then log('save skipped: source geometry is unapplied; companion preserved');return end
   local path=e.target_path:gsub('\\','/');local folder=path:match('/([^/]+)/[^/]+$');local name=basename(path)
   assert(clean(folder)and clean(name),'source geometry save binding invalid')
   local world=d and d.binding.world
   if not world then
    local fullname=valid(context)and context:GetWorld():GetFullName()or WIR_RUNTIME and WIR_RUNTIME.world_key or ''
    world=fullname:match('(/Game/[%w_/]+)%.[%w_]+$')
   end
   assert(world,'source geometry current world unavailable')
   local copy={format='FAPP_SOURCE_GEOMETRY',version=1,binding={world=world,folder=folder,name=name},actors=d and d.actors or api.json.array()}
   -- Per-file publish. Never overwrite unread bytes, or bind the editor to an old
   -- name when vanilla Save As succeeded but this one file could not be written.
   local bytes=api.json.encode(copy,M.LIMIT)..'\n';serial=serial+1
   if not e.autosave then
    if d then d.binding=copy.binding end;bindingPath=path
    if current then current.id=hash(Net.compact(api.json,current.data))end
    if pending then pending.id=hash(Net.compact(api.json,pending.data))end
    if d and wire then wire.publish(d)end
   end
   local old=Core.companion_io.read(path..'.fapgeometry',M.LIMIT)
   if old then M.decode(api.json,old,copy.binding)end -- unread authored bytes are never replaced
   local ok,why=Core.companion_io.writeBytes(path..'.fapgeometry',bytes,function()return not closed end,serial,false)
   assert(ok,why);log('saved '..path..'.fapgeometry')
  end
 end
 local stop=Core.layout_files.observe(function(e)
  local ok,why=pcall(event,e)
  if not ok then if e.kind=='save'then log('save failed; companion preserved: '..tostring(why))else failure(why)end end
 end,true)
 -- The actual apply return is inside the same game-thread frame as actor birth,
 -- before the first physics simulation. No full UObject inventory is inspected.
 for _,path in ipairs({
  '/Game/Blueprints/Core/B_GameMode.B_GameMode_C:LoadPropLayoutFromSaveGame',
  '/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C:LoadPropLayoutFromSaveGame'})do
  E.on_call(path,'post',function(gm)
   context=gm;worldKey=key(gm:GetWorld())
   local ok,why=pcall(function()if pending then freeze()end end);if not ok then failure(why)end
  end)
 end
 E.on_call('/Script/Engine.PrimitiveComponent:SetSimulatePhysics','post',function(component,args)
  if releasing or not pending or pending.error or args[1]~=true or not valid(context)or context:HasAuthority()~=true then return end
  local owner=component:GetOwner()
  local prop=StaticFindObject('/Game/Blueprints/Props/Bases/B_StaticMeshProp.B_StaticMeshProp_C')
  if valid(owner)and valid(prop)and owner:IsA(prop)and owner:GetWorld()==context:GetWorld()then
   for _,c in ipairs(frozen)do if c:GetAddress()==component:GetAddress()then component:SetSimulatePhysics(false);return end end
   frozen[#frozen+1]=component;component:SetSimulatePhysics(false)
  end
 end)
 E.on_call('/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameMode.B_CreativeGameMode_C:NewLayout','post',function()clear(true)end)
 wire=Net.new(api,Core,{
  clear=function()clear()end,
  decode=function(bytes)return M.decode(api.json,bytes)end,
  cached=function(binding,digest)
   assert(type(binding)=='table'and clean(binding.folder)and clean(binding.name),'host scenery binding invalid')
   local bytes=Core.companion_io.read(localPath(binding),M.LIMIT)
   if not bytes then return end
   local data=M.decode(api.json,bytes,binding)
   if hash(Net.compact(api.json,data))==digest then return data end
  end,
  apply=function(pc,data,digest)
   local statics=StaticFindObject('/Script/Engine.Default__GameplayStatics')
   assert(leaf(data.binding.world)==text(statics:GetCurrentLevelName(pc,true)),'host scenery belongs to another world')
   context=pc;worldKey=key(pc:GetWorld());request(data,nil,true,digest)
  end,
 })
 -- Existing file replay may have requested geometry before the RPC observer mounted.
 if pending or current then wire.publish((pending or current).data)end
 local function advance(delta,tick)
  if not tick or not valid(tick.world)then return end
  local nextWorld=key(tick.world)
  if worldKey and worldKey~=nextWorld then clear();wire.reset();context=nil end
  worldKey=nextWorld
  context=valid(context)and context:GetWorld()==tick.world and context or tick.controller
  local p=pending
  if p and not p.error then
   p.elapsed=p.elapsed+delta;assert(p.elapsed<60,'source geometry asset load exceeded 60 seconds')
   local ready=true
   for asset,resident in pairs(p.assets)do if not resident then p.assets[asset]=E.asset(asset)~=nil end;ready=ready and p.assets[asset]end
   if ready then
    freeze()
    local last=math.min(#p.data.actors,p.at+31)
    for i=p.at,last do
     local row=p.data.actors[i];local called,actor,why=pcall(E.scene_actor,row,context)
     if not called or not actor then
      local reason=tostring(called and why or actor);p.failures[#p.failures+1]={source=row.source,reason=reason};log('actor unapplied: '..row.source..': '..reason)
     elseif actor~=true then active[#active+1]=actor end
     p.at=i+1
    end
    if p.at>#p.data.actors then
     current={id=p.id,data=p.data,failures=p.failures,fromHost=p.fromHost};pending=nil;releasePhysics()
     log('restored '..#active..(p.fromHost and ' actors (from host)'or ' actors in current world')..'; unapplied='..#p.failures)
    end
   end
  end
  wire.advance(delta)
 end
 E.on_tick(function(delta,tick)local ok,why=pcall(advance,delta,tick);if not ok then failure(why)end end)
 for _,name in ipairs({'load_map_pre','world_changed'})do
  api.on(name,function()clear();wire.reset();context=nil;worldKey=nil end,{ungated=true})
 end
 api.own(function()closed=true;stop();wire.reset();clear();return true end)
 return {event=event,request=request,advance=advance,receive=wire.receive,clear=clear,status=function()return current,pending,#active end}
end
return M
