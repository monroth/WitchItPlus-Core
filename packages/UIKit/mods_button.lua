-- The only insertion entry is WB_MainMenu Construct (plus its explicit replay).
local M={}
function M.install(api,open)
 local N=assert(api.Native.scope(api.id),'Native scope unavailable')
 local base=api.module('engine')
 local O=api.module('ownership')
 local records={}
 local H={}
 local function call(o,name,...)
  return assert(N.call(o,name,table.pack(...)),'Mods button: '..name..' failed').ReturnValue
 end
 local function key(o) return base.key(o) end
 local function navigation(scope,w,direction,target)
  for _,control in ipairs({w,N.get(w,'ButtonRef')}) do
   if base.valid(control) then
    local nav=N.get(control,'Navigation')
    local data=base.valid(nav) and N.get(nav,direction==2 and 'Up' or 'Down')
    local rule=data and data.rule or 0
    local name=data and FName(data.name) or FName('None')
    local previous=data and data.target
    scope.own(function()
     if not base.valid(control) then return end
     if base.valid(previous) then call(control,'SetNavigationRuleExplicit',direction,previous)
     else call(control,'SetNavigationRule',direction,rule,name) end
    end)
    call(control,'SetNavigationRuleExplicit',direction,target)
   end
  end
 end
 local function construct(screen)
  local id=assert(key(screen),'Mods button: invalid main menu')
  local prior=records[id]
  if prior and base.valid(prior.widget) and base.same(call(prior.widget,'GetParent'),prior.parent) then return end
  if prior then prior.scope.close(); records[id]=nil end
  local scope=O.new(api)
  local ok,err=pcall(function()
   local settings=assert(N.get(screen,'Settings'),'Mods button: Settings missing')
   local parent=assert(call(settings,'GetParent'),'Mods button: Settings parent missing')
   local w=assert(N.new(call(settings,'GetClass'),screen),'Mods button: widget creation failed')
   scope.own(function() if base.valid(w) then call(w,'RemoveFromParent') end end)
   call(parent,'AddChild',w) -- Native attachment rebuilds bound cooked children.
   local value=FText('Mods')
   assert(N.set(w,'LabelText',value)); call(assert(N.get(w,'Label'),'Mods button: Label missing'),'SetText',value)
   if base.text(call(call(w,'GetClass'),'GetFName'))=='WB_Button_C' then assert(N.set(w,'Text',value)) end
   assert(N.set(w,'bIsFocusable',true))
   call(assert(N.get(w,'Label'),'Mods button: Label missing'),'SetAutoWrapText',false)
   local children=base.values(call(parent,'GetAllChildren')); local index
   for i,child in ipairs(children) do if base.same(child,settings) then index=i+1; break end end
   assert(index,'Mods button: Settings not in parent')
   -- PanelWidget has no reflected insertion: preserve and reattach the tail.
   local tail={}
   for i=index,#children-1 do
    local child=children[i]; local slot=N.get(child,'Slot')
    local p,z=slot.Padding,slot.Size
    tail[#tail+1]={widget=child,padding={Left=p.Left,Top=p.Top,Right=p.Right,Bottom=p.Bottom},
     size={Value=z.Value,SizeRule=z.SizeRule},h=slot.HorizontalAlignment,v=slot.VerticalAlignment}
   end
   for _,saved in ipairs(tail) do call(saved.widget,'RemoveFromParent') end
   -- Restore every detached native child even if a slot operation fails.
   local restoreError
   for _,saved in ipairs(tail) do
    local restored,why=pcall(function()
     local slot=assert(call(parent,'AddChild',saved.widget),'Mods button: native child reattach failed')
     call(slot,'SetPadding',saved.padding); call(slot,'SetSize',saved.size)
     call(slot,'SetHorizontalAlignment',saved.h); call(slot,'SetVerticalAlignment',saved.v)
    end)
    if not restored then restoreError=restoreError or why end
   end
   if restoreError then error(restoreError,0) end
   children=base.values(call(parent,'GetAllChildren'))
   local before,after=children[index-1],children[index+1]
   if before then navigation(scope,before,3,w); navigation(scope,w,2,before) end
   if after then navigation(scope,after,2,w); navigation(scope,w,3,after) end
   records[id]={screen=screen,widget=w,parent=parent,scope=scope,entry={widget=w}}
   api.log('Mods button inserted after Settings')
  end)
  if not ok then scope.close(); error(err,0) end
 end
 function H.ready(screen) return records[key(screen)] end -- Lookup only; never inserts.
 function H.close()
  local errors={}
  for id,r in pairs(records) do
   local done,why=r.scope.close()
   if done~=false then records[id]=nil else errors[#errors+1]='Mods button '..id..': '..tostring(why) end
  end
  if #errors==0 then N.close() end
  return #errors==0,table.concat(errors,' | ')
 end
 N.on_call('/Game/UserInterface/UIElements/WB_BaseControl.WB_BaseControl_C:ButtonClicked','post',function(w)
  for _,r in pairs(records) do if base.same(w,r.widget) then open(r.screen,H); return end end
 end)
 N.on_call('/Script/UMG.UserWidget:Destruct','post',function(w)
  local id=key(w); local r=id and records[id]
  if r then records[id]=nil; r.scope.close() end
 end)
 assert(N.on_construct('/Game/UserInterface/WB_MainMenu.WB_MainMenu_C',construct,{existing=true}),
  'Mods button: Construct subscription failed')
 api.own(H.close,function() records={}; N.close() end)
 return H
end
return M
