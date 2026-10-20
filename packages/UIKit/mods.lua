-- The native Settings session owns screen/input/Back/Esc. Content exists only
-- while Mods is open; no discovery, geometry polling or input polling is added.
local M={}
local SETTINGS='/Game/UserInterface/Settings/'
local descriptions={
 Core='Shared mod services.', UIKit='Mod settings and hotkeys.',
 FillAPotPlus='Fill a Pot+ rules and map tools.',
 MatchRulesPlusPlus='Rule presets and extra match options.',
 BaseWorldPlus='Teleporters and scenery editing.',
 PrivateCosmetics='Outfit previews and saved sets.',
}
local panels={FillAPotPlus='Settings: Custom Match → Game Rules → Fill a Pot+.',
 MatchRulesPlusPlus='Settings: Custom Match → Game Rules → Match Rules +.',
 PrivateCosmetics='Settings: Customize → Private Cosmetics / Custom Sets.'}
function M.install(api,Kit)
 local ui=Kit.new(api)
 local E=api.module('engine')
 local hotkeys=api.module('hotkeys')
 local session
 local function focus(w,screen)
  E.call(w,'SetUserFocus',E.call(screen,'GetOwningPlayer')); E.call(w,'SetKeyboardFocus')
 end
 local function restoreEntry(r)
  if ui.transition or not E.open(r.origin) then return end
  local record=r.handle.ready(r.origin)
  if record and record.entry then focus(record.entry.widget,r.origin) end
 end
 local function buildView(r)
  local screen=r.screen
  local s=ui.scope(screen); r.scope=s
  s.own(function() session.forget(r) end)
  local switcher=E.need(screen.SettingsSwitcher,'native SettingsSwitcher')
  local previous=E.call(switcher,'GetActiveWidget')
  -- The existing centered Settings frame/blur, title and bottom Back stay in
  -- their cooked tree. The owned page is a full-screen sub-screen's content,
  -- never a child of the main/escape menu button column.
  local page=s.native('ScaleBox',switcher); r.page=page
  E.call(page,'SetStretch',2) -- ScaleToFit; native DPI scaling handles resolution.
  local bounds=s.native('SizeBox',page)
  E.call(bounds,'SetWidthOverride',1380); E.call(bounds,'SetMinDesiredWidth',1380)
  E.call(bounds,'SetHeightOverride',720)
  local content=ui.section(s,bounds)
  local template=s.widget('WB_CheckBoxWithLabel',content)
  E.call(template,'SetVisibility',1)
  r.text_template=E.need(template.DescriptiveLabelText,'Custom Match label')
  local textWidths=setmetatable({},{__mode='k'})
  local function text(parent,value,owner,small,width)
   owner=owner or s
   width=width or textWidths[parent] or 1350
   if small then
    -- Scale the cooked font in Slate; never marshal a pointer-owning Font struct.
    local scale=owner.native('ScaleBox',parent)
    E.call(scale,'SetStretch',7) -- UserSpecified (6 is ScaleBySafeZone)
    E.call(scale,'SetUserSpecifiedScale',.85)
    E.call(scale.Slot,'SetHorizontalAlignment',0)
    parent=scale
   end
   local w=owner.text_copy(r.text_template,parent,math.max(1,(width-12)/(small and .85 or 1)))
   -- Justification controls lines, not the ScaleBox's child arrangement.
   -- Its default centered slot can paint the start of long text outside a cell.
   E.call(w.Slot,'SetHorizontalAlignment',0) -- Fill the bounded column
   E.call(w.Slot,'SetVerticalAlignment',1) -- Top
   E.call(w.Slot,'SetPadding',{Left=0,Top=0,Right=12,Bottom=0})
   E.call(w,'SetText',FText(value)); E.call(w,'SetAutoWrapText',true)
   E.call(w,'SetJustification',0) -- Left; the cooked label is right justified.
   E.call(w,'SetTextOverflowPolicy',0) -- Wrap complete explanatory text; no ellipsis.
   return w
  end
  r.text=text
  local function cell(parent,width,height,owner)
   local box=(owner or s).native('SizeBox',parent)
   E.call(box,'SetWidthOverride',width);textWidths[box]=width
   if height then E.call(box,'SetHeightOverride',height) end
   -- Keep wrapped text and cooked controls within their column.
   E.call(box,'SetClipping',1) -- ClipToBounds, including cooked control children.
   E.call(box.Slot,'SetHorizontalAlignment',0)
   E.call(box.Slot,'SetVerticalAlignment',1) -- Top
   return box
  end
  r.cell=cell
  local function padding(w,top,bottom,right)
   E.call(w.Slot,'SetPadding',{Left=0,Top=top or 0,Right=right or 0,Bottom=bottom or 0})
  end
  r.padding=padding
  local function separator(parent,owner)
   owner=owner or s
   local line=owner.native('SizeBox',parent); E.call(line,'SetHeightOverride',1)
   local image=owner.native('Image',line); E.call(image,'SetOpacity',.2)
  end
  r.separator=separator
  local function checkbox(parent,label,value,on_change,owner,width)
   owner=owner or s
   local row=owner.native('HorizontalBox',parent)
   text(cell(row,width-52,nil,owner),label,owner,true)
   local control=ui.checkbox(owner,cell(row,52,48,owner),{asset='WB_CheckBoxWithLabel',
    label='',value=value,on_change=on_change})
   -- The cooked label's ScaleBox shrinks it to unreadability in narrow columns.
   -- Keep the cooked checkbox/focus/events, with a separately sized native label.
   E.call(control.widget.DescriptiveLabelText,'SetVisibility',1)
   E.call(control.widget.DescriptiveLabelText:GetParent(),'SetVisibility',1)
   local glyph=E.need(control.widget.CheckBox,'native checkbox glyph')
   local grid=E.need(glyph:GetParent(),'native checkbox grid')
   -- Custom Match's grid reserves label columns and 36-unit side padding.
   -- Normalize only this owned instance to fit the 52x48 glyph cell, keeping
   -- its cooked brushes, state, focus target and Blueprint delegate intact.
   for column=0,2 do E.call(grid,'SetColumnFill',column,0) end
   E.call(glyph.Slot,'SetColumn',0)
   E.call(glyph.Slot,'SetPadding',{Left=0,Top=0,Right=0,Bottom=0})
   E.call(glyph.Slot,'SetHorizontalAlignment',1)
   E.call(glyph.Slot,'SetVerticalAlignment',2) -- Center
   E.call(glyph,'SetVisibility',0); E.call(grid,'SetVisibility',0)
   return control
  end
  r.checkbox=checkbox
  local function hide(w)
   s.restore(function() return E.call(w,'GetVisibility') end,function(v) E.call(w,'SetVisibility',v) end,1)
  end
  -- The title TextBlock is not a BP variable and GetWidgetFromName is not
  -- reflected live; find the TextBlock reading "Settings" in the widget tree.
  -- Cosmetic only: without it the native title stays.
  local function findTitle(w,depth)
   if not E.valid(w) or depth>12 then return end
   if E.text(w:GetClass():GetFName())=='TextBlock' then
    local ok,t=pcall(function() return E.text(E.call(w,'GetText')) end)
    if ok and t=='Settings' then return w end
    return
   end
   local kids={}
   if E.read(w,'GetAllChildren') then pcall(function() kids=E.values(E.call(w,'GetAllChildren')) end)
   elseif E.read(w,'GetContent') then pcall(function() kids={E.call(w,'GetContent')} end) end
   for _,c in ipairs(kids) do local hit=findTitle(c,depth+1); if hit then return hit end end
  end
  local tree=E.read(screen,'WidgetTree')
  local title=tree and findTitle(E.read(tree,'RootWidget'),0)
  if title then
   s.restore(function() return E.text(E.call(title,'GetText')) end,
    function(v) E.call(title,'SetText',FText(v)) end,'Mods')
  end
  local nativeTab=E.need(screen.TabButton_Game,'native Settings tab')
  local tabbar=E.call(nativeTab,'GetParent')
  for _,w in ipairs(E.values(E.call(tabbar,'GetAllChildren'))) do hide(w) end
  hide(E.need(screen.Button_Apply,'native Apply button'))
  local packages=ui.scroll(s,content,{height=684}); r.packages=packages
  padding(text(packages.rows,'Restart the game to apply changes.'),0,12)
  local sync=api.shared('Core').package_sync
  local versions=api.module('versions')
  local versionText=text(packages.rows,'',s,true)
  local function versionStatus(snapshot)
   local message=versions.describe(snapshot)
   E.call(versionText,'SetText',FText(message))
   E.call(versionText:GetParent(),'SetVisibility',message=='' and 1 or 0)
  end
  versionStatus(sync.snapshot())
  s.own(sync.observe(api,versionStatus))
  local modpack=assert(api.shared('Core').modpack)
  local notice=text(packages.rows,'',s,true)
  local function setting(fn,value)
   local ok,why=pcall(fn,value)
   if not ok then api.log('Setting failed: '..tostring(why));E.call(notice,'SetText',FText('Could not save this setting. Try again.'))end
  end
  local kickDelay
  checkbox(packages.rows,'Kick for mod mismatch',modpack.require_modpack(),function(v)
   setting(modpack.set_require_modpack,v)
   E.call(kickDelay.widget,'SetIsEnabled',modpack.require_modpack())
  end,s,950)
  kickDelay=ui.slider(s,cell(packages.rows,950),{label='Kick after (s)',kind='int',
   min=5,max=20,step=1,default=10,value=modpack.kick_after(),
   on_change=function(v)setting(modpack.set_kick_after,v)end})
  E.call(kickDelay.widget,'SetIsEnabled',modpack.require_modpack())
  checkbox(packages.rows,'Auto-update',modpack.auto_update(),function(v)setting(modpack.set_auto_update,v)end,s,950)
  ui.button(s,packages.rows,{label='Update to latest',on_click=function()
   local ok,result=pcall(modpack.prepare,'latest',{})
   if not ok then api.log('Update request failed: '..tostring(result))end
   E.call(notice,'SetText',FText(ok and result or 'Could not request an update. Try again.'))
  end})
  separator(packages.rows)
  local packageControls={}
  for _,p in ipairs(api.mods.list()) do
   local row=s.native('HorizontalBox',packages.rows)
   padding(row,12,12)
   local details=ui.section(s,cell(row,995))
   padding(details,0,0,24)
   text(details,(descriptions[p.id] and ({Core='Core',UIKit='UIKit',FillAPotPlus='Fill a Pot+',MatchRulesPlusPlus='Match Rules +',BaseWorldPlus='Editor tweaks',PrivateCosmetics='Private Cosmetics'})[p.id] or p.name)..'  '..p.version..'  ·  '..(({loaded='Enabled',disabled='Disabled',failed='Unavailable'})[p.state]or 'Unavailable'),s,false,947)
   local description=p.description~='' and p.description or descriptions[p.id] or 'Mod.'
   text(details,description,s,true,947)
   if p.state=='failed' and p.error then text(details,'Could not start this mod. Restart the game.',s,true,947)
   elseif panels[p.id] then text(details,panels[p.id],s,true,947) end
   local toggle=ui.section(s,cell(row,355))
   if p.core_component then
    text(toggle,'Core component',s,true,355)
   else
    local restart
    local control=checkbox(toggle,'Enabled',p.enabled,function(value)
      api.mods.set_enabled(p.id,value); E.call(restart,'SetText',FText('Pending'))
      E.call(restart:GetParent(),'SetVisibility',0)
     end,s,355)
    restart=text(toggle,p.restart_required and 'Pending' or '',s,true)
    E.call(restart:GetParent(),'SetVisibility',p.restart_required and 0 or 1)
    packageControls[#packageControls+1]=control
   end
   separator(packages.rows)
  end
  ui.navigate(packageControls,packages.widget)
  local keys=ui.scroll(s,content,{height=684}); r.keys=keys
  local keyScope
  local function refreshKeys()
   if keyScope then keyScope.close() end
   keyScope=ui.scope(screen,{parent=s})
   local keyRows=ui.section(keyScope,keys.rows)
   hotkeys.build_compact(api,ui,keyScope,keyRows,screen,r)
  end
  refreshKeys()
  s.own(api.on('hotkeys_changed',function()
   -- Key state changes repaint the compact rows. Rebuild only declarations.
   local ids={}; for _,e in ipairs(api.hotkeys.list()) do ids[#ids+1]=e.id end
   local signature=table.concat(ids,'\n')
   if signature~=r.key_signature then refreshKeys() end
  end,{ungated=true}))
  local function select(which)
   if r.cancel_capture then r.cancel_capture() end
   r.selected=which
   E.call(packages.widget:GetParent(),'SetVisibility',which=='packages' and 0 or 1)
   E.call(keys.widget:GetParent(),'SetVisibility',which=='keys' and 0 or 1)
   for _,tab in ipairs(r.tabs) do E.call(tab.widget,'SetActiveState',tab.which==which) end
  end
  r.select=select
  r.tabs={}
  for _,spec in ipairs({{'Mods','packages'},{'Hotkeys','keys'}}) do
   local tab=ui.button(s,tabbar,{class=E.call(nativeTab,'GetClass'),label=spec[1],single_line=true,
    on_click=function() select(spec[2]) end})
   tab.which=spec[2]; r.tabs[#r.tabs+1]=tab
   local source=nativeTab.Slot
   E.call(tab.widget.Slot,'SetPadding',{Left=source.Padding.Left,Top=source.Padding.Top,
    Right=source.Padding.Right,Bottom=source.Padding.Bottom})
   E.call(tab.widget.Slot,'SetSize',{Value=source.Size.Value,SizeRule=source.Size.SizeRule})
   E.call(tab.widget.Slot,'SetHorizontalAlignment',source.HorizontalAlignment)
   E.call(tab.widget.Slot,'SetVerticalAlignment',source.VerticalAlignment)
  end
  select('packages')
  s.own(function()
   if E.valid(switcher) and E.same(E.call(switcher,'GetActiveWidget'),page) and E.valid(previous) then
    E.call(switcher,'SetActiveWidget',previous)
   end
  end)
  E.call(switcher,'SetActiveWidget',page)
  r.initial_focus=r.tabs[1].widget
  focus(r.initial_focus,screen)
 end
 session=api.module('mods_settings').new(api,ui,E,buildView,restoreEntry)
 api.module('mods_button').install(api,session.open)
 ui.own(api.hook(E.path('WB_SettingsTabs','OnBack',SETTINGS),function(self)
  local r=session.current()
  if r and E.same(E.unwrap(self),r.screen) and not E.open(r.screen) then session.retire(true) end
 end))
 ui.own(api.hook(E.path('WB_SettingsTabs','GetInitialFocusWidget',SETTINGS),function(self,out)
  session.reconcile(E.unwrap(self))
  local r=session.current()
  if r and r.initial_focus and E.same(E.unwrap(self),r.screen) then out:set(r.initial_focus) end
 end,{out_params={2}}))
 ui.own(api.hook(E.path('WB_SettingsTabs','FocusStoredWidget',SETTINGS),function(self)
  session.reconcile(E.unwrap(self))
  local r=session.current()
  if r and r.initial_focus and E.same(E.unwrap(self),r.screen) then focus(r.initial_focus,r.screen) end
 end))
 ui.own(api.hook(E.path('WB_SettingsTabs','SetActiveSettingsTab',SETTINGS),function(self)
  -- Native shoulders enumerate the cooked Settings pages. Keep those pages
  -- untouched and route that request back to the owned two-tab content.
  local r=session.current()
  if r and E.same(E.unwrap(self),r.screen) then
   E.call(r.screen.SettingsSwitcher,'SetActiveWidget',r.page)
   r.select(r.selected=='packages' and 'keys' or 'packages')
  end
 end))
 local native=assert(api.Native.scope(api.id),'Mods input scope unavailable')
 assert(native.intercept('/Game/Blueprints/UserInterface/B_MenuUIManager.B_MenuUIManager_C:ChangeMenu',function(manager)
  local r=session.current()
  if not r or not r.capture_active or not r.capture_active() then return false end
  r.cancel_capture()
  api.log('Mods capture: consumed ChangeMenu before Blueprint body')
  return true
 end),'Mods capture Back interception unavailable')
 ui.own(api.hook(E.path('WB_SettingsTabs','OnPreviewKeyDown',SETTINGS),function(self,geometry,event)
  local r=session.current()
  if not r or not r.capture_active or not r.capture_active() or not E.same(E.unwrap(self),r.screen) then return end
  local key=E.call(E.class('/Script/Engine.Default__KismetInputLibrary'),'GetKey',E.unwrap(event))
  local name=E.text(key.KeyName)
  if name~='Escape' and r.reject_reserved_key(name) then
   return E.call(E.class('/Script/UMG.Default__WidgetBlueprintLibrary'),'Handled')
  end
 end,{override=true}))
end
return M
