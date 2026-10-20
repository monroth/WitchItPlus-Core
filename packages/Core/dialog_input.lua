-- Native UMG owns showing/closing dialogs. Restore what its UI-only switch
-- displaced; MenuUIManager.CloseDialog never restores input, and Creative
-- IngameUIManager.CloseScreen skips RefreshFocus. No polling or world scans.
local M={}
local UMG='/Script/UMG.WidgetBlueprintLibrary:'
local MANAGERS='/Game/Blueprints/UserInterface/'
local dialogs={WB_MessageDialog_C=true,WB_QuestionDialog_C=true}
function M.install(api,Core)
 local R=Core.runtime
 local N=assert(api.Native.scope(api.id),'dialog input scope unavailable')
 local records,last,stopRestore={},nil,nil
 local restoring=false;local generation=0
 local function valid(o)return R.isObject(o)and R.call(o,'IsValid')==true end
 local function same(a,b)return valid(a)and valid(b)and R.call(a,'GetAddress')==R.call(b,'GetAddress')end
 local function call(o,name,...)
  local args=table.pack(...)
  -- The existing SDK accepts nil UObject parameters (viewport focus). The
  -- named Native.call facade requires every argument to be non-nil.
  return R.access('dialog input '..name,function()return o[name](o,table.unpack(args,1,args.n))end)
 end
 local function object(path)
  local o=StaticFindObject(path);assert(valid(o),'dialog input object unavailable: '..path);return o
 end
 local function library()return object('/Script/UMG.Default__WidgetBlueprintLibrary')end
 local function cancelRestore()if stopRestore then stopRestore();stopRestore=nil end end
 local function clear()cancelRestore();records={};last=nil;generation=generation+1 end
 local function dialog(w)
  local c=valid(w)and R.call(w,'GetClass')
  return valid(c)and dialogs[R.text(R.call(c,'GetFName'))]==true
 end
 local function inputMode(pc)
  local mode=call(object('/Script/PropWitchHuntModule.Default__BpInputSettingsLibrary'),'GetInputMode',pc)
  local name=R.text(call(object('/Script/PropWitchHuntModule.EInputMode'),'GetNameByValue',mode)):match('([^:]+)$')
  assert(name=='IM_GameOnly'or name=='IM_UIOnly'or name=='IM_GameAndUI','dialog input mode unavailable: '..tostring(name))
  return name
 end
 local function snapshot(pc,w)
  local mode=inputMode(pc)
  local world=call(pc,'GetWorld')
  local previous=last and same(last.pc,pc)and same(last.world,world)and last.mode==mode and last or{}
  local gi=call(object('/Script/Engine.Default__GameplayStatics'),'GetGameInstance',pc)
  local ingame,menu=R.read(gi,'IngameUIManager'),R.read(gi,'MenuUIManager')
  local master=R.read(menu,'MasterScreen')
  local focus=mode~='IM_GameOnly'and valid(previous.focus)and previous.focus or nil
  -- Existing native menus may predate Core installation. Use their current
  -- focus owner instead of discovering arbitrary widgets.
  if mode=='IM_UIOnly'and not focus then
   local top=R.read(ingame,'TopMostFocusedWidget')
   focus=valid(top)and not same(top,w)and top or R.read(menu,'CurrentWidget')
   if not valid(focus)or same(focus,w)then focus=nil end
  end
  -- WB_MasterScreen already remembers the exact focused control per screen.
  local remembered=R.read(master,'FocusedElements')
  local cls=valid(focus)and R.call(focus,'GetClass')
  if valid(cls)and R.call(remembered,'Contains',cls)==true then
   local child=R.unwrap(R.call(remembered,'Find',cls));if valid(child)then focus=child end
  end
  local cursor=R.read(pc,'bShowMouseCursor');assert(type(cursor)=='boolean','dialog input cursor state unavailable')
  return {widget=w,pc=pc,world=world,master=master,mode=mode,cursor=cursor,focus=focus,
   lock=previous.lock or 0,hide=previous.hide==true}
 end
 local function capture(pc,w)
  for _,r in ipairs(records)do if same(r.widget,w)then return end end
  records[#records+1]=snapshot(pc,w)
 end
 local function setMode(r)
  if not valid(r.pc)or not same(R.call(r.pc,'GetWorld'),r.world)then return false end
  local focus=valid(r.focus)and r.focus or nil
  local lib=library()
  if r.mode=='IM_GameOnly'then call(lib,'SetInputMode_GameOnly',r.pc,false)
  elseif r.mode=='IM_GameAndUI'then call(lib,'SetInputMode_GameAndUIEx',r.pc,focus,r.lock,r.hide,false)
  else call(lib,'SetInputMode_UIOnlyEx',r.pc,focus,r.lock,false)end
  r.pc.bShowMouseCursor=r.cursor
  -- Native subordinate close queues a MasterScreen refocus even when the
  -- returning Creative menu uses viewport input. Our explicit return wins.
  if valid(r.master)then r.master.RefreshInitialFocus=false end
  if focus then call(focus,'SetUserFocus',r.pc);call(focus,'SetKeyboardFocus')
  elseif r.mode=='IM_UIOnly'and valid(r.master)then call(r.master,'InitialFocus')
  else
   call(lib,'SetFocusToGameViewport')
  end
  return true
 end
 local function restore(r,log)
  restoring=true
  local ok,applied=pcall(setMode,r)
  restoring=false
  if not ok then api.log('Dialog input restore failed: '..tostring(applied))
  elseif applied and log then api.log('Dialog input restored: '..r.mode..' cursor='..tostring(r.cursor))end
 end
 local function closed(w)
  local found
  for i,r in ipairs(records)do if same(r.widget,w)then found=i;break end end
  if not found then return end
  local r=table.remove(records,found)
  local nextDialog=records[found]
  if nextDialog then
   -- If a lower dialog closes first, its eventual return target belongs to the
   -- remaining dialog. Closing it later must never focus the removed widget.
   for _,k in ipairs({'mode','cursor','focus','lock','hide'})do nextDialog[k]=r[k]end
  end
  local top=records[#records]
  if top and valid(top.widget)then
   r={pc=top.pc,world=top.world,mode='IM_UIOnly',cursor=true,focus=top.widget,lock=0,hide=false}
  end
  cancelRestore()
  restore(r,false)
  -- Close delegates can still run Blueprint focus work after CloseDialog.
  -- One deferred restoration follows that work; a new input action cancels it.
  stopRestore=api.delay(1,function()stopRestore=nil;restore(r,true)end)
 end
 for _,mode in ipairs({'GameOnly','GameAndUIEx','UIOnlyEx'})do
  assert(N.on_call(UMG..'SetInputMode_'..mode,'pre',function(_,a)
   local pc,w=a[1],mode~='GameOnly'and a[2]or nil
   if not valid(pc)or R.call(pc,'IsLocalPlayerController')~=true then return end
   if not restoring then
    cancelRestore()
    if dialog(w)then capture(pc,w)end
   end
   last={pc=pc,world=R.call(pc,'GetWorld'),mode=({GameOnly='IM_GameOnly',UIOnlyEx='IM_UIOnly',GameAndUIEx='IM_GameAndUI'})[mode],
    focus=valid(w)and w or nil,lock=mode~='GameOnly'and a[3]or 0,hide=mode=='GameAndUIEx'and a[4]==true}
  end),'dialog input mode hook unavailable')
 end
 for _,entry in ipairs({{'B_MenuUIManager','CloseDialog'},{'B_IngameUIManager','CloseScreen'}})do
  assert(N.on_call(MANAGERS..entry[1]..'.'..entry[1]..'_C:'..entry[2],'post',function(_,a)closed(a[1])end),'dialog input close hook unavailable')
 end
 api.on('load_map_pre',clear,{ungated=true})
 api.own(clear,clear,'dialog input snapshots')
 -- A full native sub-menu can displace input too. Its owner closes/routes the
 -- cooked screen, then invokes this once after the Blueprint close body.
 local function save(pc)
  local r=snapshot(pc);local captured=generation
  return function()
   local value=r;r=nil
   if value and captured==generation then cancelRestore();restore(value,true)end
  end
 end
 return {clear=clear,save=save}
end
return M
