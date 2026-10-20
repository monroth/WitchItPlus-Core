local M={}
local SLIDER='WB_SliderWithEditableLabel'
local CHECK='WB_CheckBoxWithEditableLabel'
local CHANGE='BndEvt__ValueSlider_K2Node_ComponentBoundEvent_0_OnFloatValueChangedEvent__DelegateSignature'
local COMMIT='BndEvt__WB_SliderWithEditableLabel_InputField_K2Node_ComponentBoundEvent_3_OnEditableTextBoxCommittedEvent__DelegateSignature'
local RESET='BndEvt__WB_SliderWithEditableLabel_ResetButton_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature'
function M.new(api,E,O)
    local ui={scopes={},entries={},transition=false}
    local function snapshot(scopes,topOnly)
        local list={}
        for s in pairs(scopes) do
            if not topOnly or not s.parent_scope or not scopes[s.parent_scope] then list[#list+1]=s end
        end
        return list
    end
    local function close_scopes(scopes,topOnly)
        local done,errors=true,{}
        for _,s in ipairs(snapshot(scopes,topOnly)) do
            local ok,result,why=pcall(s.close)
            if not ok or result==false then done=false end
            if not ok or result==false then errors[#errors+1]=tostring(ok and why or result) end
        end
        return done,table.concat(errors,' | ')
    end
    local function abandon_scopes()
        for _,s in ipairs(snapshot(ui.scopes)) do s.abandon() end
        ui.scopes={}; ui.entries={}
    end
    local root=O.new(api)
    local abandon=root.abandon
    root.abandon=function() abandon_scopes(); abandon() end
    O.attach(api,root)
    ui.close=root.close
    ui.own=root.own
    root.own(function()
        local done,why=close_scopes(ui.scopes,true)
        if done then ui.entries={} end
        return done,why
    end)
    function ui.scope(screen,options)
        assert(not root.closed,'UIKit instance closed')
        assert(not ui.transition,'UI construction during travel forbidden')
        E.need(screen,'screen'); assert(not screen:GetFullName():find('Default__',1,true),'CDO is not a screen')
        local s=O.new(api); s.screen=screen; s.enabled=options and options.enabled; s.children={}
        ui.scopes[s]=true
        local parentScope=options and options.parent
        if parentScope then
            assert(not parentScope.closed,'parent scope closed')
            parentScope.children[s]=true; s.parent_scope=parentScope
        end
        local close,abandonScope=s.close,s.abandon
        local function unlink()
            ui.scopes[s]=nil; s.screen=nil; s.enabled=nil
            if parentScope then parentScope.children[s]=nil end
            s.parent_scope=nil
        end
        s.close=function()
            local done,why=close()
            if done~=false then unlink() end
            return done,why
        end
        s.abandon=function() abandonScope(); unlink() end
        s.own(function() return close_scopes(s.children) end)
        function s.live(widget)
            return not s.closed and not ui.transition and api.is_active() and E.open(s.screen)
                and (not widget or E.open(widget)) and (not s.enabled or s.enabled())
        end
        function s.native(kind,parent)
            assert(not s.closed,'UI scope closed')
            local w=E.native(s.screen,kind)
            s.own(function() if E.valid(w) then E.call(w,'RemoveFromParent') end end)
            if parent then E.call(parent,'AddChild',w) end
            return w
        end
        function s.find_native(kind,parent)
            assert(not s.closed,'UI scope closed')
            local class=E.class('/Script/UMG.'..kind);local found
            for _,w in ipairs(E.values(E.call(E.need(parent,'field parent'),'GetAllChildren'))) do
                if E.valid(w) and E.call(w,'IsA',class) then
                    assert(not found,'ambiguous UI scope field: '..kind);found=w
                end
            end
            return E.need(found,'UI scope field '..kind)
        end
        function s.text_copy(template,parent,wrap_width)
            -- UE4SS's eight-argument native template overload copies the cooked
            -- TextBlock in the engine. No Font/SlateBrush crosses the Lua bridge.
            assert(not s.closed,'UI scope closed')
            local w=E.need(StaticConstructObject(E.class('/Script/UMG.TextBlock'),
                E.need(s.screen.WidgetTree,'WidgetTree'),FName('None'),0,0,false,false,
                E.need(template,'native text template')),'native text copy')
            -- UTextLayoutWidget exposes WrapTextAt as a property, not a
            -- reflected setter. Set it before AddChild builds/synchronizes Slate.
            if wrap_width then w.WrapTextAt=wrap_width end
            s.own(function() if E.valid(w) then E.call(w,'RemoveFromParent') end end)
            E.call(parent,'AddChild',w)
            return w
        end
        function s.widget(asset,parent)
            return s.clone(E.elements..asset..'.'..asset..'_C',parent)
        end
        function s.clone(classOrPath,parent)
            assert(not s.closed,'UI scope closed')
            local w=E.clone(s.screen,classOrPath)
            s.own(function() if E.valid(w) then E.call(w,'RemoveFromParent') end end)
            -- Attachment rebuilds Slate and bound children before any child read.
            E.call(parent,'AddChild',w)
            return w
        end
        function s.restore(read,write,value)
            -- Scalars only; conditional restoration preserves later external changes.
            local original=read()
            local kind=type(value)
            assert((kind=='boolean' or kind=='number' or kind=='string') and type(original)==kind,
                'restore requires same-type non-nil scalar state')
            s.own(function() if read()==value then write(original) end end)
            write(value)
        end
        return s
    end
    local function register(s,w,entry)
        local key=assert(E.key(w),'widget identity unavailable')
        entry.widget=w; entry.scope=s; ui.entries[key]=entry
        s.own(function() ui.entries[key]=nil end)
        return entry
    end
    local function label(w,text)
        local value=FText(text)
        if E.valid(E.read(w,'DescriptiveLabelText')) then
            w.DescriptiveLabel=value; E.call(w.DescriptiveLabelText,'SetText',value)
        else w.LabelText=value; E.call(E.need(w.Label,'Label'),'SetText',value) end
    end
    local function tooltip(w,spec)
        w.bIsFocusable=true
        if spec.tooltip then E.call(w,'SetToolTipText',FText(spec.tooltip)) end
    end
    local function changed(r,value)
        if r.value==value then return end
        r.value=value
        if r.on_change then r.on_change(value,r) end
    end
    function ui.header(s,parent,text)
        local w=s.native('TextBlock',parent)
        E.call(w,'SetText',FText(text)); E.call(w,'SetAutoWrapText',true)
        return w
    end
    function ui.section(s,parent,title)
        local box=s.native('VerticalBox',parent)
        if title then ui.header(s,box,title) end
        return box
    end
    function ui.checkbox(s,parent,spec)
        assert(type(spec.value)=='boolean','checkbox value must be boolean')
        local asset=spec.asset or CHECK
        local w=s.widget(asset,parent); label(w,spec.label); tooltip(w,spec)
        if spec.default==nil then w.DefaultBoolValue=spec.value
        else assert(type(spec.default)=='boolean'); w.DefaultBoolValue=spec.default end
        local r=register(s,w,{kind='checkbox',value=spec.value,on_change=spec.on_change})
        function r.set(value)
            assert(not s.closed,'UI scope closed')
            assert(type(value)=='boolean'); r.value=value
            E.call(w,'SetIsCheckedWithoutEvent',value)
        end
        function r.get() return r.value end
        r.set(spec.value); ui.install_events('checkbox:'..asset); return r
    end
    function ui.slider(s,parent,spec)
        local lo,hi,step=spec.min,spec.max,spec.step or (spec.kind=='int' and 1 or .1)
        assert(type(lo)=='number' and type(hi)=='number' and type(step)=='number'
            and math.abs(lo)<math.huge and math.abs(hi)<math.huge and step<math.huge
            and lo<hi and step>0,'invalid slider range')
        local inputMax=spec.input_max or hi
        assert(type(inputMax)=='number' and inputMax<math.huge and inputMax>=hi,'input_max must cover slider range')
        local function parse(value,source)
            if spec.infinite and (value=='infinite' or type(value)=='string' and
                (value:match('^%s*(.-)%s*$'):lower()=='inf' or value:match('^%s*(.-)%s*$')=='∞')) then return 'infinite' end
            local number=tonumber(value)
            if not number or number~=number or math.abs(number)==math.huge then return end
            if spec.kind=='int' and number~=math.floor(number) then return end
            if source=='slide' then
                number=math.max(lo,math.min(hi,number))
                if spec.kind=='int' then number=math.floor(number+.5) end
                if spec.infinite and number>=hi then return 'infinite' end
            end
            if number<lo or number>inputMax then return end
            return number
        end
        local value=assert(parse(spec.value,'input'),'invalid slider value')
        local default=spec.default==nil and value or assert(parse(spec.default,'input'),'invalid default')
        local w=s.widget(SLIDER,parent); label(w,spec.label); tooltip(w,spec)
        w.MinimumValue=lo; w.MaximumValue=hi; w.StepSize=step
        w.bRoundToIntegers=spec.kind=='int'; w.UIMultiplyFactor=1
        w.bTimeValueInSeconds=false; w.ValueSuffix=FText(spec.suffix or '')
        w.DefaultFloatValue=default=='infinite' and hi or default
        E.call(E.need(w.DefaultValueIndicator,'DefaultValueIndicator'),'SetValue',
            math.max(0,math.min(1,(w.DefaultFloatValue-lo)/(hi-lo))))
        local r=register(s,w,{kind='slider',value=value,default=default,on_change=spec.on_change})
        local function paint()
            r.painting=true
            local ok,err=pcall(function()
                E.call(w,'SetRealValue',r.value=='infinite' and hi or math.min(hi,r.value))
                E.call(w,'CheckDefaultToUpdateUI')
                E.call(E.need(w.InputField,'InputField'),'SetText',FText(r.value=='infinite' and '∞' or tostring(r.value)))
            end)
            r.painting=false; if not ok then error(err) end
        end
        function r.set(v)
            assert(not s.closed,'UI scope closed')
            r.value=assert(parse(v,'input'),'invalid slider value'); paint()
        end
        function r.get() return r.value end
        function r.edit(v,source)
            assert(not s.closed,'UI scope closed')
            local parsed=parse(v,source)
            if parsed~=nil then changed(r,parsed) end
            paint() -- Invalid commits restore the last valid value.
        end
        r.slide=function(v) r.edit(lo+(hi-lo)*v,'slide') end
        r.drag=function() r.edit(w.RealValue,'slide') end
        r.commit=function(t) r.edit(E.text(E.unwrap(t)),'input') end
        r.reset=function() r.edit(default,'input') end
        r.repaint=function()
            if not E.call(w.InputField,'HasFocusedDescendants') then
                E.call(w.InputField,'SetText',FText(r.value=='infinite' and '∞' or tostring(r.value)))
            end
        end
        paint(); ui.install_events('slider'); return r
    end
    function ui.button(s,parent,spec)
        local w=spec.class and s.clone(spec.class,parent) or s.widget('WB_TabButton',parent)
        local nativeText=spec.class and E.text(E.call(E.call(w,'GetClass'),'GetFName'))=='WB_Button_C'
        label(w,spec.label); tooltip(w,spec)
        if spec.single_line then E.call(E.need(w.Label,'button Label'),'SetAutoWrapText',false) end
        -- WB_Button's cooked Tick paints Text back into LabelText.
        if nativeText then w.Text=FText(spec.label) end
        local r=register(s,w,{kind='button',click=spec.on_click or function() end,
            set_label=function(text)
                assert(not s.closed,'UI scope closed'); label(w,text)
                if nativeText then w.Text=FText(text) end
            end})
        ui.install_events('button'); return r
    end
    function ui.scroll(s,parent,options)
        options=options or {}
        local host=parent
        if options.height then
            host=s.native('SizeBox',parent); E.call(host,'SetHeightOverride',options.height)
        end
        local w=s.native('ScrollBox',host); local rows=s.native('VerticalBox',w)
        E.call(w,'SetAlwaysShowScrollbar',true)
        return {widget=w,rows=rows}
    end
    function ui.dropdown(s,parent,spec)
        assert(#spec.options>0,'empty dropdown')
        local box=ui.section(s,parent)
        local selected=spec.value
        local choices
        local r,expanded
        local function caption(value)
            for _,option in ipairs(spec.options) do if option.value==value then return option.label end end
            error('dropdown value absent from options')
        end
        local b=ui.button(s,box,{label=spec.label..': '..caption(selected),tooltip=spec.tooltip,on_click=function()
            if expanded then choices.close(); expanded=nil; return end
            choices=ui.scope(s.screen,{enabled=s.enabled,parent=s})
            expanded=ui.section(choices,box)
            for _,option in ipairs(spec.options) do
                ui.button(choices,expanded,{label=option.label,on_click=function()
                    local value=option.value; local differs=selected~=value
                    selected=value; r.set(value); choices.close(); expanded=nil
                    if differs and spec.on_change then spec.on_change(value,r) end
                end})
            end
        end})
        r={widget=b.widget,kind='dropdown'}
        function r.set(v)
            assert(not s.closed,'UI scope closed')
            local text=caption(v); selected=v; b.set_label(spec.label..': '..text)
        end
        function r.get() return selected end
        return r
    end
    function ui.dialog(s,parent,spec)
        local d=ui.scope(s.screen,{enabled=s.enabled,parent=s})
        local box=ui.section(d,parent,spec.title or 'Message')
        ui.header(d,box,spec.message)
        ui.button(d,box,{label=spec.close_label or 'OK',on_click=function()
            d.close(); if spec.on_close then spec.on_close() end
        end})
        return {widget=box,close=d.close}
    end
    function ui.list(s,parent,spec)
        spec=spec or {}; local view=ui.scroll(s,parent,spec)
        local content; local result={widget=view.widget,rows=view.rows}
        function result.set(items)
            if content then content.close() end
            content=ui.scope(s.screen,{enabled=s.enabled,parent=s})
            local panel=content.native(spec.columns and 'UniformGridPanel' or 'VerticalBox',view.rows)
            for i,item in ipairs(items) do
                local row=ui.button(content,panel,{label=spec.label and spec.label(item,i) or tostring(item),
                    on_click=function() if spec.on_select then spec.on_select(item,i) end end})
                if spec.columns then
                    assert(spec.columns>=1 and spec.columns%1==0,'invalid tile columns')
                    local slot=E.need(row.widget.Slot,'UniformGridSlot')
                    E.call(slot,'SetRow',math.floor((i-1)/spec.columns))
                    E.call(slot,'SetColumn',(i-1)%spec.columns)
                end
            end
        end
        result.set(spec.items or {}); return result
    end
    ui.tiles=ui.list
    function ui.navigate(rows,scroll)
        if scroll then assert(E.call(scroll,'IsA',E.class('/Script/UMG.ScrollBox')),'navigation scroll must be ScrollBox') end
        for i,row in ipairs(rows) do
            local entry=assert(ui.entries[E.key(row.widget)],'navigation needs owned UIKit rows')
            entry.scroll=scroll
            local targets={{2,rows[i-1]},{3,rows[i+1]}}
            for _,target in ipairs(targets) do
                if target[2] then
                    E.call(row.widget,'SetNavigationRuleExplicit',target[1],target[2].widget)
                    for _,key in ipairs({'FocusableWidget','CheckBox','ValueSlider','ButtonRef'}) do
                        local inner=E.read(row.widget,key)
                        if E.valid(inner) then E.call(inner,'SetNavigationRuleExplicit',target[1],target[2].widget); break end
                    end
                end
            end
        end
        ui.install_events('focus')
    end
    function ui.view_items(s,view,items)
        -- Optional native ListView/TileView adapter: genuine UObject items only.
        -- Keeps existing native entries. No SetListItems Lua-array marshalling.
        local added={}; s.own(function()
            if E.valid(view) then for _,item in ipairs(added) do if E.valid(item) then E.call(view,'RemoveItem',item) end end end
        end)
        for _,item in ipairs(items) do
            E.need(item,'ListView item'); local exists=false
            for _,native in ipairs(E.values(E.call(view,'GetListItems'))) do if E.same(native,item) then exists=true end end
            if not exists then E.call(view,'AddItem',item); added[#added+1]=item end
        end
    end
    function ui.tab(s,screen,spec)
        local switcher=E.need(screen.RulesSwitcher,'RulesSwitcher')
        local previousActive=E.call(switcher,'GetActiveWidget')
        local bar=E.call(E.need(screen.TabButton_Mods,'TabButton_Mods'),'GetParent')
        local page=ui.scroll(s,switcher)
        local button=ui.button(s,bar,{label=spec.label,on_click=function()
            E.call(screen,'SetActiveTab',page.widget)
        end})
        -- The tab button remains interactive while its page is inactive.
        button.live=function(w)
            return not s.closed and api.is_active() and E.open(screen) and E.open(w)
        end
        -- Restoration filters current arrays, so later tabs from other mods survive.
        s.own(function()
            if E.valid(screen) and E.valid(switcher) then
                E.remove(screen.Buttons,button.widget); E.remove(screen.RuleTabs,page.widget)
                if E.same(E.call(switcher,'GetActiveWidget'),page.widget) then
                    local remaining=E.values(screen.RuleTabs)
                    local target=E.valid(previousActive) and E.contains(screen.RuleTabs,previousActive) and previousActive or remaining[1]
                    if target then E.call(screen,'SetActiveTab',target) end
                end
                E.call(screen,'UpdateTabButtonStyles')
            end
        end)
        local tab={widget=page.widget,rows=page.rows,button=button.widget}
        function tab.reconcile()
            assert(not s.closed,'UI scope closed')
            -- Rebuild arrays only while our actual children are still attached.
            assert(E.same(E.call(page.widget,'GetParent'),switcher) and E.same(E.call(button.widget,'GetParent'),bar),
                'native tab tree rebuilt; discard old scope, never reparent')
            local changed=false
            for _,pair in ipairs({{screen.Buttons,button.widget},{screen.RuleTabs,page.widget}}) do
                if not E.contains(pair[1],pair[2]) then
                    local values=E.values(pair[1]); values[#values+1]=pair[2]; E.replace(pair[1],values);changed=true
                end
            end
            if changed then E.call(screen,'UpdateTabButtonStyles') end
        end
        function tab.focus_target()
            -- Native HandleRefocus asks GetInitialFocusWidget on ordinary clicks.
            -- Return the currently focused cooked row instead of the first row.
            if E.call(page.widget,'HasFocusedDescendants') then
                for _,entry in pairs(ui.entries)do
                    if entry.scope==s and E.valid(entry.widget) and
                        (E.call(entry.widget,'HasKeyboardFocus') or E.call(entry.widget,'HasFocusedDescendants'))then
                        return entry.widget
                    end
                end
            end
            return tab.initial_focus
        end
        function tab.focus()
            if s.closed or E.call(page.widget,'HasFocusedDescendants') then return end
            if tab.initial_focus and E.valid(tab.initial_focus) then
                E.call(tab.initial_focus,'SetUserFocus',E.call(screen,'GetOwningPlayer'))
                E.call(tab.initial_focus,'SetKeyboardFocus')
            end
        end
        tab.reconcile(); return tab
    end
    local installed={}
    function ui.install_events(kind)
        if installed[kind] then return end; installed[kind]=true
        local function hook(asset,name,fn)
            root.own(api.hook(E.path(asset,name),function(self,...)
                local w=E.unwrap(self); local r=ui.entries[E.key(w)]
                if r and not r.painting then
                    local live
                    if r.live then live=r.live(w) else live=r.scope.live(w) end
                    if live then fn(r,...) end
                end
            end))
        end
        if kind=='button' then
            hook('WB_BaseControl','ButtonClicked',function(r) if r.click then r.click() end end)
            return
        end
        if kind=='focus' then
            hook('WB_BaseControl','OnFocusReceived',function(r)
                if E.valid(r.scroll) then E.call(r.scroll,'ScrollWidgetIntoView',r.widget,true,0,0) end
            end)
            return
        end
        local checkAsset=kind:match('^checkbox:(.+)$')
        if checkAsset then
            local function check(r) if r.kind=='checkbox' then changed(r,E.call(r.widget.CheckBox,'IsChecked')) end end
            hook(checkAsset,'SetIsChecked',check)
            hook(checkAsset,'BndEvt__CheckBox_K2Node_ComponentBoundEvent_0_OnCheckBoxComponentStateChanged__DelegateSignature',check)
            return
        end
        assert(kind=='slider','unknown event group')
        hook(SLIDER,CHANGE,function(r,v) if r.slide then r.slide(E.unwrap(v)) end end)
        hook(SLIDER,'ChangeRealValue',function(r) if r.drag then r.drag() end end)
        hook(SLIDER,COMMIT,function(r,t) if r.commit then r.commit(t) end end)
        hook(SLIDER,RESET,function(r) if r.reset then r.reset() end end)
        hook(SLIDER,'CheckDefaultToUpdateUI',function(r) if r.repaint then r.repaint() end end)
    end
    -- Consumers with a verified destination producer can handle travel without LoadMap POST.
    function ui.travel_begin()
        ui.transition=true
        -- Failed live rollback must not be retried after a native world boundary.
        for _,s in ipairs(snapshot(ui.scopes)) do if s.closed then s.abandon() end end
        local ok,err=pcall(close_scopes,ui.scopes,true)
        abandon_scopes()
        if not ok then api.log('UI travel cleanup: '..tostring(err)) end
    end
    function ui.destination_ready() ui.transition=false end
    root.own(api.on('load_map_pre',ui.travel_begin,{ungated=true}))
    root.own(api.on('load_map_post',ui.destination_ready,{ungated=true}))
    root.own(api.on('world_changed',ui.travel_begin,{ungated=true}))
    root.own(api.on('world_ready',ui.destination_ready,{ungated=true}))
    return ui
end
return M
