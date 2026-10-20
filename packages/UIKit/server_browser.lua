-- Session data is authoritative for the Modded filter; names are never parsed.
local M={}
local LIST='/Game/UserInterface/Play/WB_ServerList.WB_ServerList_C'
local BASE='/Game/UserInterface/Play/WB_ServerListBase.WB_ServerListBase_C'
local ROW='/Game/UserInterface/Play/WB_ServerRow.WB_ServerRow_C'
function M.install(api,Kit)
 local E=api.module('engine');local ui=Kit.new(api);local N=assert(api.Native.scope(api.id))
 local service=assert(api.shared('Core').modpack);local screens={}
 local function metadata(data)
  if not E.valid(data)then return end
  local entries=E.read(data,'ExtraSettings');if not entries then return end
  local ok,result,present=pcall(service.setting,entries)
  if ok then return result,present end
  api.log('Modded sessions: settings unavailable: '..tostring(result))
 end
 -- The native getter reads SanitizedName; normalize only this display copy,
 -- after native result/profanity processing. Keep Join metadata unchanged.
 local function displayName(data)
  local name=E.text(E.read(data,'SanitizedName'))
  local display=service.display_name(name)
  if display~=name then data.SanitizedName=display end
 end
 local function message(value,screen)
  api.log('Modded sessions: '..value)
  if E.valid(screen)then
   local gp=E.class('/Script/Engine.Default__GameplayStatics')
   local gi=E.call(gp,'GetGameInstance',screen)
   local manager=E.valid(gi)and E.read(gi,'MenuUIManager')
   if E.valid(manager)then E.call(manager,'ShowMessageDialog','Modpack',value)end
  end
 end
 local function owner(row)
  local data=E.read(row,'ServerRowDataObject')
  if not E.valid(data)then return end
  for _,state in pairs(screens)do
   if E.valid(state.screen)and E.contains(state.screen.FoundServers,data)then return state,data end
  end
 end
 local function clear(state)
  state.data=nil;state.host=nil;state.compatible=false;state.description=nil
  E.call(state.info,'SetText',FText('Select a server to see its mods.'))
  E.call(state.join.widget,'SetIsEnabled',false);E.call(state.prepare.widget,'SetIsEnabled',false)
 end
 local function selectRow(state,data,host)
  host=host or metadata(data)
  if not host then clear(state);return end
  displayName(data)
  state.data=data;state.host=host
  local rows,compatible=service.details(host.packages);state.compatible=compatible
  local title=E.text(E.call(data,'GetServerName'))
  local lines={title,host.require_modpack and 'Modpack required. Mod versions must match.' or 'Players without mods can join. Mod versions must match.',''}
  for _,r in ipairs(rows)do lines[#lines+1]=r.mark..' '..r.name..'\n  host '..(r.host or 'disabled')..' / local '..(r.local_version or 'missing')end
  lines[#lines+1]='';lines[#lines+1]=compatible and 'Versions match.' or 'Choose Sync mods with host to use this server’s mods.'
  state.description=table.concat(lines,'\n')
  E.call(state.info,'SetText',FText(state.description))
  E.call(state.join.widget,'SetIsEnabled',compatible);E.call(state.prepare.widget,'SetIsEnabled',true)
  api.log('Modded sessions: selected '..title..'; '..(compatible and 'compatible' or 'version mismatch'))
 end
 -- Keep UI action errors local: a transient join/request failure must not
 -- increment the runtime's package-error threshold and remove the whole tab.
 local function action(state,label,fn)
  return function()
   api.log('Modded sessions: '..label..' pressed')
   local ok,decision=pcall(fn)
   state.internal=false
   if ok then api.log('Modded sessions: '..label..' decision='..tostring(decision))
   else
    local why=label..' failed: '..tostring(decision)
    api.log('Modded sessions: '..why)
    pcall(function()E.call(state.info,'SetText',FText('Could not complete this action. Refresh the list and try again.'))end)
   end
  end
 end
 local function selected(state)
  local data=state.data
  if not E.valid(data)or not E.contains(state.screen.FoundServers,data)then clear(state);return nil,'refused: select a current server' end
  local host=metadata(data)
  if not host then clear(state);return nil,'refused: session modpack unavailable' end
  selectRow(state,data,host);return data,host
 end
 local function join(state)
  local data,host=selected(state);if not data then return host end
  local refusal=service.refusal(host.packages)
  if refusal then message(refusal,state.screen);return 'refused: versions differ' end
  local session=assert(E.read(data,'OnlineSession'),'selected OnlineSession unavailable')
  local gp=E.class('/Script/Engine.Default__GameplayStatics')
  local gi=E.need(E.call(gp,'GetGameInstance',state.screen),'game instance')
  local entries=E.read(data,'ExtraSettings');local password=FName('IsPasswordProtected')
  local protected=entries:Contains(password)and E.unwrap(entries:Find(password)).Data.AsBool==true
  -- Same reflected endpoints as WB_ServerRow.ProceedJoin, using the selected
  -- B_ServerRowData.OnlineSession directly. No virtualized row widget required.
  if protected then
   E.call(E.need(E.read(gi,'MenuUIManager'),'menu UI manager'),'ShowPasswordDialog',session)
   return 'native password dialog'
  end
  E.call(gi,'JoinFromServerList',session)
  return 'native JoinFromServerList'
 end
 -- Rebuild from the complete current result set after every vanilla sort or
 -- refresh. Do not rely on a Blueprint OUT parameter hook to filter ListView.
 local function results(state,reason)
  local all=E.values(state.screen.FoundServers);local modded,present={},0
  for _,data in ipairs(all)do
   local host,hasKey=metadata(data)
   if hasKey then present=present+1 end
   if host then displayName(data);modded[#modded+1]=data end
  end
  if state.data then selected(state)end
  api.log('Modded sessions: results seen='..#all..' key='..present..' valid='..#modded..' source='..reason)
  if state.modded then
   E.call(state.screen.ServerListView,'ClearListItems')
   for _,data in ipairs(modded)do E.call(state.screen.ServerListView,'AddItem',data)end
  end
 end
 local function style(state)
  for _,name in ipairs({'All','Official','Custom','Creative'})do
   E.call(state.screen['Button_Filter_'..name],'SetActiveState',not state.modded and state.screen.CurrentFilter==({All=0,Official=1,Custom=2,Creative=3})[name])
  end
  E.call(state.tab.widget,'SetActiveState',state.modded)
  E.call(state.panel,'SetVisibility',state.modded and 0 or 1)
  E.call(state.body.Slot,'SetOffsets',state.modded and state.narrow or state.offsets)
 end
 local function construct(screen)
  local id=E.key(screen);if not id or screens[id]then return end
  local s=ui.scope(screen);local state={screen=screen,scope=s,modded=false};screens[id]=state
  local root=E.need(screen.WidgetTree.RootWidget,'server-list root')
  assert(E.text(E.call(E.call(root,'GetClass'),'GetFName'))=='CanvasPanel','server-list root must be CanvasPanel')
  -- Resize the original root branch: rows, column headers and tabs share its
  -- layout. Scaling just ListView left the headers over the details panel.
  local body=screen.ServerListView
  for _=1,32 do
   local parent=E.call(body,'GetParent')
   assert(E.valid(parent),'server-list content has no root branch')
   if E.same(parent,root)then break end
   body=parent
  end
  assert(E.same(E.call(body,'GetParent'),root),'server-list content root not found')
  state.body=body
  local offsets=E.call(body.Slot,'GetOffsets');local anchors=E.call(body.Slot,'GetAnchors')
  state.offsets={Left=offsets.Left,Top=offsets.Top,Right=offsets.Right,Bottom=offsets.Bottom}
  state.narrow={Left=offsets.Left,Top=offsets.Top,Right=offsets.Right+(anchors.Maximum.X>anchors.Minimum.X and 460 or -460),Bottom=offsets.Bottom}
  s.own(function()screens[id]=nil end)
  local template=E.need(screen.Button_Filter_All,'All filter')
  local tabbar=E.call(template,'GetParent')
  state.tab=ui.button(s,tabbar,{class=E.call(template,'GetClass'),label='Modded',single_line=true,on_click=action(state,'Modded',function()
   state.modded=true;state.internal=true
   E.call(screen,'ChangeFilter',0)
   state.internal=false;clear(state);style(state);return 'selected'
  end)})
  local source=template.Slot
  E.call(state.tab.widget.Slot,'SetPadding',{Left=source.Padding.Left,Top=source.Padding.Top,Right=source.Padding.Right,Bottom=source.Padding.Bottom})
  E.call(state.tab.widget.Slot,'SetSize',{Value=source.Size.Value,SizeRule=source.Size.SizeRule})
  local panel=s.native('Border',root);state.panel=panel
  E.call(panel,'SetBrushColor',{R=.025,G=.035,B=.05,A=.97})
  E.call(panel,'SetPadding',{Left=18,Top=16,Right=18,Bottom=16})
  E.call(panel.Slot,'SetAnchors',{Minimum={X=1,Y=.28},Maximum={X=1,Y=.90}})
  E.call(panel.Slot,'SetOffsets',{Left=-450,Top=0,Right=440,Bottom=0})
  E.call(panel.Slot,'SetZOrder',20)
  local content=ui.section(s,panel)
  local scroll=ui.scroll(s,content,{height=450})
  state.info=s.text_copy(E.need(template.Label,'filter label template'),scroll.rows)
  E.call(state.info,'SetAutoWrapText',true);E.call(state.info,'SetJustification',0)
  E.call(state.info.Slot,'SetHorizontalAlignment',0)
  state.join=ui.button(s,content,{label='Join',on_click=action(state,'Join',function()return join(state)end)})
  state.prepare=ui.button(s,content,{label='Sync mods with host',on_click=action(state,'Sync mods with host',function()
   local data,host=selected(state);if not data then return host end
   local result=service.prepare('exact',host.packages)
   E.call(state.notice,'SetText',FText(result));return 'request saved; restart required'
  end)})
  state.notice=s.text_copy(E.need(template.Label,'filter label template'),content)
  E.call(state.notice,'SetAutoWrapText',true);E.call(state.notice,'SetJustification',0)
  E.call(state.notice,'SetText',FText(''))
  clear(state);style(state)
  s.own(function()if E.valid(screen)then
   E.call(state.body.Slot,'SetOffsets',state.offsets)
  end end)
  api.log('Modded sessions: tab installed')
 end
 assert(N.on_call(LIST..':AddServersSorted','post',function(screen)
  local state=screens[E.key(screen)];if state then results(state,'sorted/refresh')end
 end))
 -- Count raw results too, before version/UGC/profanity processing creates rows.
 assert(N.on_call(BASE..':HandleServersFoundEOS','post',function(screen,args)
  local total,present,valid=0,0,0
  for _,result in ipairs(E.values(args[1]))do
   total=total+1
   local host,hasKey=service.setting(result.Session.SessionSettings.Settings)
   if hasKey then present=present+1 end;if host then valid=valid+1 end
  end
  api.log('Modded sessions: search results seen='..total..' key='..present..' valid='..valid)
 end))
 assert(N.on_call(LIST..':ChangeFilter','post',function(screen)
  local state=screens[E.key(screen)];if not state then return end
  if not state.internal then state.modded=false;clear(state)end
  style(state)
 end))
 assert(N.on_call(LIST..':UpdateTabButtonStyles','post',function(screen)
  local state=screens[E.key(screen)];if state then style(state)end
 end))
 local function intercept(row,proceed)
  local state,data=owner(row);if not state then return false end
  if state.modded then selectRow(state,data);return true end
  local host=metadata(data)
  local refusal=host and service.refusal(host.packages)
  if refusal then message(refusal,state.screen);return true end
  return false
 end
 -- ClickedServer also covers keyboard/gamepad activation and password routes.
 local function dispatch(row,proceed)
  local ok,consume=pcall(intercept,row,proceed)
  if ok then return consume end
  -- A failed details/compatibility read must never turn a selection into a join
  -- or retire the host's interception subscription.
  api.log('Modded sessions: activation refused: '..tostring(consume));return true
 end
 assert(N.intercept(ROW..':BndEvt__Button_534_K2Node_ComponentBoundEvent_280_OnButtonClickedEvent__DelegateSignature',function(row)return dispatch(row,false)end))
 assert(N.intercept(ROW..':ClickedServer',function(row)return dispatch(row,false)end))
 assert(N.intercept(ROW..':ProceedJoin',function(row)return dispatch(row,true)end))
 assert(N.on_call(LIST..':RemoveAllServers','post',function(screen)
  local state=screens[E.key(screen)];if state then clear(state)end
 end))
 assert(N.on_call('/Script/UMG.UserWidget:Destruct','post',function(screen)
  local state=screens[E.key(screen)];if state then state.scope.close()end
 end))
 assert(N.on_construct(LIST,construct,{existing=true}))
 return {screens=screens}
end
return M
