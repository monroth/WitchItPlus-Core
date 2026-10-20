-- Public session metadata, explicit updater requests and the host join policy.
local M={KEY='WitchItModpack',VERSION=1,SUFFIX=' [MODDED]',GRACE=15}
local VANILLA_NOTICE='This server requires the Witch It+ modpack. Get it at github.com/monroth/WitchItPlus-Core'
function M.new(api,Core)
 local Public=api.module('public_packages');local E=assert(api.Native.scope(api.id))
 local config=api.config({RequireModpack=true,KickAfterSeconds=10});local S={};local peers={}
 if type(config.RequireModpack)~='boolean'then config.RequireModpack=true end
 local function validDelay(v)return type(v)=='number'and v==v and v%1==0 and v>=5 and v<=20 end
 if not validDelay(config.KickAfterSeconds)then config.KickAfterSeconds=10 end
 local function unwrap(v)
  local ok,get=pcall(function()return v.get end)
  return ok and type(get)=='function' and get(v) or v
 end
 local refreshSession=false;local refreshError
 local refused,pendingRefusal
 local auto=false
 local bytes=E.update_file('read_settings')
 if bytes then local ok,v=pcall(api.json.decode,bytes);if ok and type(v)=='table'then auto=v.auto_update==true end end
 local activeMap=Public.map((api.packages.snapshot()or{}).packages)
 local function map()return activeMap end
 function S.local_packages()return map()end
 function S.require_modpack()return config.RequireModpack end
 function S.kick_after()return config.KickAfterSeconds end
 function S.set_kick_after(value)
  assert(validDelay(value),'kick delay must be an integer from 5 to 20 seconds')
  config.KickAfterSeconds=value;api.save_settings(config)
  api.log('Join policy: kick delay='..value..'s');return true
 end
 function S.auto_update()return auto end
 function S.set_auto_update(value)
  assert(type(value)=='boolean');local path,why=E.update_file('settings',api.json.encode({auto_update=value}));assert(path,why)
  auto=value;api.log('Auto-update '..(value and 'ON' or 'OFF'));return true
 end
 function S.set_require_modpack(value)
  assert(type(value)=='boolean');config.RequireModpack=value;api.save_settings(config);refreshSession=true
  if Core.package_sync then Core.package_sync.policy_changed()end
  api.log('Require modpack '..(value and 'ON' or 'OFF'));return true
 end
 function S.prepare(kind,packages)
  assert(kind=='exact'or kind=='latest','invalid update request')
  local canonical=Public.canonical(packages,kind=='latest')
  local payload=api.json.encode({request=kind,packages=canonical,created_utc=os.date('!%Y-%m-%dT%H:%M:%SZ')})
  assert(#payload<=16384,'update request too large')
  local path,why=E.update_file('request',payload);assert(path,why)
  api.log('Update request saved: '..path..'; restart the game to prepare versions. No automatic join.')
  return 'Restart the game to apply.'
 end
 function S.advertisement()
  return api.json.encode({version=M.VERSION,packages=map(),require_modpack=config.RequireModpack})
 end
 function S.parse(raw)
  if type(raw)~='string'or #raw>4096 then return end
  local ok,v=pcall(function()
   local v=api.json.decode(raw);assert(type(v)=='table'and v.version==M.VERSION and type(v.require_modpack)=='boolean')
   for k in pairs(v)do assert(k=='version'or k=='packages'or k=='require_modpack')end
   Public.validate(v.packages);return v
  end)
  return ok and v or nil
 end
 function S.details(host)return Public.details(host,map())end
 function S.refusal(host)return Public.refusal(host,map())end
 function S.refuse_host(pc,host)
  local message=S.refusal(host)
  if not message then return false end
  if not pc or not pc:IsValid()or not pc:IsLocalPlayerController()or pc:HasAuthority()then return false end
  local identity=tostring(pc:GetAddress())..':'..pc:GetFullName()..'|'..message
  if refused==identity then return true end
  api.log('Client join refused: '..message)
  -- Scalar notice survives travel; the game's own client-return RPC closes the
  -- connection. Show it again on the destination menu even if NetError hid it.
  pendingRefusal=message
  local result,why=E.call(pc,'ClientReturnToMainMenuWithTextReason',{FText(message)})
  if not result then api.log('Client return failed: '..tostring(why));return false end
  refused=identity
  return true
 end
 function S.setting(entries)
  -- FName lookup is case-insensitive, unlike comparing its display spelling.
  -- Steam/EOS may give a name a different display case on the other machine.
  local key=FName(M.KEY)
  if not entries:Contains(key)then return nil,false end
  local value=unwrap(entries:Find(key))
  local raw=unwrap(value.Data.AsString)
  return S.parse(type(raw)=='string'and raw or raw:ToString()),true
 end
 function S.display_name(name)
  while name:sub(-#M.SUFFIX)==M.SUFFIX do name=name:sub(1,-#M.SUFFIX-1)end
  return name
 end
 local function advertise(settings)
  local payload=S.advertisement()
  local nameKey=FName('SessionName')
  if settings.Settings:Contains(nameKey)then
   local setting=unwrap(settings.Settings:Find(nameKey))
   local value=unwrap(setting.Data.AsString)
   local name=S.display_name(type(value)=='string'and value or value:ToString())..M.SUFFIX
   settings.Settings:Add(nameKey,{Data={Type=6,AsString=name},AdvertisementType=3,ID=setting.ID or 0})
   api.log('Session name advertised: '..name)
  else api.log('Session name advertisement unavailable: SessionName setting missing')end
  -- SDK table-to-struct marshaling builds a fresh setting. Never borrow or
  -- modify GameMode's FString, or keep a TMap element across reallocation.
  settings.Settings:Add(FName(M.KEY),{Data={Type=6,AsString=payload},AdvertisementType=3,ID=0})
  local readback=assert(S.setting(settings.Settings),'Session modpack setting readback failed')
  assert(api.json.encode(readback)==payload,'Session modpack setting readback differs')
  api.log('Session modpack advertised: '..Public.summary(map())..'; key='..M.KEY..' bytes='..#payload..' advertisement=ViaOnlineServiceAndPing')
 end
 local SESSION='/Script/OnlineSubsystemBlueprints.'
 api.hook(SESSION..'OnlineSessionSubsystemCreateSession:CreateSession',function(_,_,_,_,settings)
  advertise(settings:get());settings:set(settings:get())
 end,{phase='pre',out_params={5},ungated=true})
 api.hook(SESSION..'OnlineSessionSubsystemUpdateSession:UpdateSession',function(_,_,_,settings)
  advertise(settings:get());settings:set(settings:get())
 end,{phase='pre',out_params={4},ungated=true})
 local function key(pc)return tostring(pc:GetAddress())..':'..pc:GetFullName()end
 function S.joined(pc)
  if not pc or not pc:IsValid()or not pc:HasAuthority()or pc:IsLocalPlayerController()then return end
  local id=key(pc);if not peers[id]then peers[id]={pc=pc,remaining=config.KickAfterSeconds}end
 end
 -- Retirement already knows the route key: never inspect a dying controller.
 function S.left(pc,id)id=id or pc and key(pc);if id then peers[id]=nil end end
 function S.retire_peers(retired)
  for id in pairs(peers)do if not retired or retired[id]then peers[id]=nil end end
 end
 function S.packet(pc)
  S.joined(pc);local id=key(pc);local p=peers[id]
  if not p then return end
  p.remaining=M.GRACE;p.fresh=true;p.modded=true
  api.log('Join policy: WIR2 packet peer='..id..' remaining='..p.remaining..'s')
 end
 function S.reply(pc,packages,problems)
  S.joined(pc);local id=key(pc);local p=peers[id]
  api.log('Join policy: reply received peer='..id..' matched='..tostring(p~=nil))
  if not p then return end
  Public.validate(packages);p.packages=packages;p.problems=problems
  api.log('Join policy: peer='..id..' versions='..(select(2,Public.details(map(),packages))and 'matching' or 'mismatch'))
 end
 function S.client_features_allowed(host,remotePolicy)
  -- A guest's saved host preference has no authority in another host's world.
  if host then return config.RequireModpack end
  return remotePolicy==true -- unknown policy cannot admit client-only features
 end
 local function vanilla_chat(p)
  if p.chatNotice or p.packages or p.modded then return end
  local gp=StaticFindObject('/Script/Engine.Default__GameplayStatics');local state=E.call(gp,'GetGameState',{p.pc})
  if not state then return end
  -- No cooked owning-client chat RPC exists. Use the game's reliable chat path;
  -- Player is the sender's PlayerState, not a destination (this is multicast).
  local result=E.call(state.ReturnValue,'SendChatMessageMulticast',{FText(VANILLA_NOTICE),p.pc.PlayerState,0,{}})
  if result then p.chatNotice=true;api.log('Join policy: vanilla chat queued peer='..key(p.pc))end
 end
 local function kick(p,reason)
  local gp=StaticFindObject('/Script/Engine.Default__GameplayStatics');local gm=gp:GetGameMode(p.pc)
  local lib=StaticFindObject('/Script/PropWitchHuntModule.Default__ProjectVersionBlueprint')
  if not p.packages and not p.modded and not p.kickNotice then
   -- Match B_GameMode.KickPlayer: this reliable owning-client RPC takes an
   -- FName, not free text/an enum. None selects the normal string-table reason.
   local result=E.call(p.pc,'ClientSetWasKicked',{FName('None')})
   if result then p.kickNotice=true;api.log('Join policy: vanilla kick dialog queued peer='..key(p.pc))end
  end
  local result,why=E.call(lib,'KickPlayer',{gm,p.pc,FText(reason)})
  assert(result and result.ReturnValue~=false,'KickPlayer: '..tostring(why or 'native call refused'));p.kicked=true;api.log('Join policy: '..reason..' peer='..key(p.pc))
 end
 api.packages.observe(function(snapshot)
  if snapshot then activeMap=Public.map(snapshot.packages)end
  refreshSession=true
 end)
 assert(E.on_tick(function(delta,context)
  if pendingRefusal and context and context.controller then
   local ok,shown=pcall(function()
    local pc=context.controller
    if not pc:IsValid()or not pc:IsA('/Game/Blueprints/Player/B_MenuPlayerController.B_MenuPlayerController_C')then return false end
    local gp=StaticFindObject('/Script/Engine.Default__GameplayStatics');local gi=gp:GetGameInstance(pc)
    if not gi or not gi:IsValid()then return false end
    local manager=gi.MenuUIManager;if not manager or not manager:IsValid()then return false end
    local result=E.call(manager,'ShowMessageDialog',{'Mods differ from host',pendingRefusal})
    return result~=nil
   end)
   if ok and shown then pendingRefusal=nil;refused=nil end
  end
  if refreshSession and context and context.controller then
   local attemptedMap,attemptedPolicy=activeMap,config.RequireModpack
   local ok,updated,why=pcall(function()
    local pc=context.controller;if not pc:IsValid()then return false,'controller unavailable' end
    if not pc:HasAuthority()then return true end -- explicit guest role; retry on travel
    local gp=StaticFindObject('/Script/Engine.Default__GameplayStatics')
    local gi=gp:GetGameInstance(pc);if not gi or not gi:IsValid()then return false,'GameInstance unavailable' end
    local manager=gi.SessionManager;if not manager or not manager:IsValid()then return false,'SessionManager unavailable' end
    manager.SessionIsDirty=true
    local result,reason=E.call(manager,'UpdateSessionProperties',{})
    assert(result and result.ReturnValue~=false,reason or 'UpdateSessionProperties refused')
    return true
   end)
   if ok and updated then
    refreshSession=activeMap~=attemptedMap or config.RequireModpack~=attemptedPolicy
    refreshError=nil
   else
    local reason=tostring(ok and why or updated)
    if refreshError~=reason then refreshError=reason;api.log('Session modpack refresh pending: '..reason)end
   end
  end
  for id,p in pairs(peers)do
   local ok,why=pcall(function()
   if not p.pc:IsValid()then peers[id]=nil
   elseif not p.kicked and (config.RequireModpack or p.packages)then
    -- PostLogin may precede controller initialization. Start/count only while
    -- this controller belongs to the ticking host world and has a PlayerState.
    local world=p.pc:GetWorld();local state=p.pc.PlayerState
    local present=context and context.world and context.world:IsValid() and world and world:IsValid()
     and world:GetAddress()==context.world:GetAddress() and state and state:IsValid()
    if present then
     if not p.started then
      p.started=true;api.log('Join policy: deadline start peer='..id..' world='..world:GetFullName()..' remaining='..p.remaining..'s')
     elseif not p.paused and not p.fresh then
      -- A stalled host frame must not consume an entire loading allowance before
      -- the queued client RPCs can run. Paused/out-of-world time never counts.
      p.remaining=p.remaining-math.max(0,math.min(1,delta or 0))
     end
     p.fresh=nil
     if p.paused then p.paused=nil;api.log('Join policy: deadline resumed peer='..id..' remaining='..p.remaining..'s')end
     if not p.packages then
      local bucket=math.ceil(p.remaining/5)
      if p.bucket~=bucket then p.bucket=bucket;api.log('Join policy: waiting peer='..id..' remaining='..math.max(0,math.ceil(p.remaining))..'s')end
     end
     if p.packages then
      if not select(2,Public.details(map(),p.packages))then
       kick(p,p.problems and #p.problems>0 and table.concat(p.problems,'; ')or 'Update modpack to '..Public.summary(map()))
      end
     else
      -- Spend the existing final two in-world seconds on the notice; never add
      -- a second timer or shorten/extend the configured/WIR2 grace deadline.
      if p.remaining<=2 then vanilla_chat(p)end
      if p.remaining<=0 then kick(p,'This server requires the Witch It+ modpack')end
     end
    elseif p.started and not p.paused then
     p.paused=true;api.log('Join policy: deadline paused peer='..id..' remaining='..math.max(0,math.ceil(p.remaining))..'s reason=controller outside current host world')
    end
   end
   end)
   if not ok and p.error~=tostring(why)then p.error=tostring(why);api.log('Join policy: '..p.error)end
  end
 end),'join-policy tick unavailable')
 assert(E.on_call('/Game/Blueprints/Core/B_GameMode.B_GameMode_C:K2_PostLogin','post',function(_,args)S.joined(args[1])end))
 assert(E.on_call('/Game/Blueprints/Core/B_GameMode.B_GameMode_C:K2_OnLogout','post',function(_,args)S.left(args[1])end))
 api.on('world_changed',function()S.retire_peers();refreshSession=true;refreshError=nil end,{ungated=true})
 S.public=Public
 return S
end
return M
