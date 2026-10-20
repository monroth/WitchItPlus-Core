-- Native Settings owns routing/input. Reconcile real widgets on events, never timers.
local M={}
local CLASS='/Game/UserInterface/Settings/WB_SettingsTabs.WB_SettingsTabs_C'
local MANAGER='/Game/Blueprints/UserInterface/B_MenuUIManager.B_MenuUIManager_C:'
function M.new(api,ui,E,build,restore)
 local N=assert(api.Native.scope(api.id),'Mods Settings: Native scope unavailable')
 local S={}
 local opened,pending,returning,closed,building
 function S.current() return opened end
 function S.forget(r) if opened==r then opened=nil end end
 function S.retire(refocus)
  pending=nil; returning=nil
  local r=opened; opened=nil
  if r then
   if r.scope then r.scope.close() end
   if refocus and not ui.transition then restore(r) end
  end
  return r
 end
 function S.reconcile(constructed)
  if closed or ui.transition or building then return end
  local r=pending or opened
  if not r or not E.valid(r.manager) or E.read(r.manager,'CurrentMenuState')~=11 then return end
  if opened then
   -- ChangeMenu may reset the active native tab after Construct returns.
   if E.valid(r.screen) and E.valid(r.page) then
    local switcher=E.read(r.screen,'SettingsSwitcher')
    if E.valid(switcher) and not E.same(E.call(switcher,'GetActiveWidget'),r.page) then E.call(switcher,'SetActiveWidget',r.page) end
   end
   return r
  end
  local screen
  local cache=E.read(r.manager,'MenuStateWidgets')
  if cache then cache:ForEach(function(k,v) if E.unwrap(k)==11 then screen=E.unwrap(v) end end) end
  if not E.valid(screen) then screen=constructed end
  if not E.valid(screen) then return end
  -- Explicit cache adoption needs a shown screen. Construct itself is the event
  -- that permits building even before the manager attaches it to the viewport.
  if not E.same(screen,constructed) and E.call(screen,'IsVisible')~=true then return end
  pending=nil; opened=r; r.screen=screen; building=true
  local ok,err=xpcall(function() build(r) end,debug.traceback); building=nil
  if not ok then
   S.retire(false)
   if E.valid(screen) then E.call(screen,'OnBack') end
   error('Mods Settings: '..tostring(err),0)
  end
  return opened
 end
 function S.open(origin,handle)
  if closed or ui.transition or opened or pending then return end
  local game=E.call(E.class('/Script/Engine.Default__GameplayStatics'),'GetGameInstance',origin)
  local manager=E.need(game.MenuUIManager,'MenuUIManager')
  local r={origin=origin,handle=handle,manager=manager}; pending=r
  local ok,err=pcall(function()
   E.call(manager,'ChangeMenu',11,true) -- the existing native Settings route
   if pending~=r and opened~=r then return end -- Back/travel during routing
   api.retry_hooks() -- the first route may load the Settings Blueprint
   S.reconcile()
   if pending==r then api.log('Mods Settings: awaiting Settings Construct/focus event') end
  end)
  if not ok then S.retire(false); error('Mods Settings route: '..tostring(err),0) end
 end
 assert(N.on_construct(CLASS,S.reconcile),'Mods Settings: Construct subscription failed')
 assert(N.on_call(MANAGER..'ChangeMenu','post',function(manager)
  local r=pending or opened or returning
  if not r or not E.same(manager,r.manager) then return end
  if E.read(manager,'CurrentMenuState')~=11 then
   S.retire(false); if not ui.transition then restore(r) end
  else S.reconcile() end
 end),'Mods Settings: ChangeMenu subscription failed')
 assert(N.on_call('/Script/UMG.UserWidget:Destruct','pre',function(screen)
  if opened and E.same(screen,opened.screen) then returning=S.retire(false) end
 end),'Mods Settings: Destruct subscription failed')
 ui.own(api.on('load_map_pre',function() S.retire(false) end,{ungated=true}))
 ui.own(api.on('world_changed',function() S.retire(false) end,{ungated=true}))
 ui.own(function() closed=true; S.retire(false); N.close() end)
 return S
end
return M
