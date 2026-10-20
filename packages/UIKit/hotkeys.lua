-- Central runtime controls, composed only when the native rules window is ready.
local M={}
function M.build_compact(api,ui,scope,rows,screen,view)
    local E=api.module('engine')
    local function text(parent,value) return view.text(parent,value,scope,true) end
    local function cell(parent,width,height) return view.cell(parent,width,height,scope) end
    -- Shared by headers and rows; total 1350 leaves room for the native scrollbar.
    local widths={310,240,320,200,95,185}
    local header=scope.native('HorizontalBox',rows)
    for i,value in ipairs({'Action','Mod','Available in','Shortcut','Reset','Disabled'}) do
        text(cell(header,widths[i],44),value)
    end
    local controls,ids,columns={},{},{{},{},{}}
    local captureCancel,captureRow
    local supported={}; for _,key in ipairs(api.hotkeys.keys()) do supported[key]=true end
    -- Modifier presses precede the chord's base key; the polling capture also
    -- excludes these six keys. A reserved Ctrl/Alt binding must not end capture.
    for _,key in ipairs({'LeftShift','RightShift','LeftControl','RightControl','LeftAlt','RightAlt'}) do supported[key]=nil end
    local function showStatus(row,message)
        E.call(row.status,'SetText',FText(message))
        E.call(row.status,'SetToolTipText',FText(''))
        E.call(row.status:GetParent(),'SetVisibility',message~='' and 0 or 1)
    end
    local function cancelCapture()
        if captureCancel then captureCancel(); captureCancel=nil end
        if captureRow then
            captureRow.capture.set_label(captureRow.chord)
            showStatus(captureRow,captureRow.message or '')
            captureRow=nil
        end
    end
    view.cancel_capture=cancelCapture; scope.own(cancelCapture)
    view.capture_active=function() return captureRow~=nil end
    scope.own(function() view.capture_active=nil end)
    local function rejection(chord,reason)
        if reason:sub(1,6)=='game: ' then return chord..' — reserved by the game: '..reason:sub(7) end
        return 'Rejected: '..reason
    end
    -- Slate preview sees engine keys even when UIOnly/fullscreen handling keeps
    -- them out of PlayerController's polled key state. Never persist a rejection.
    view.reject_reserved_key=function(key)
        if not captureRow or not supported[key] or not scope.live(captureRow.capture.widget) then return false end
        local row=captureRow
        local good,reason=api.hotkeys.check(row.id,key)
        if good or reason:sub(1,6)~='game: ' then return false end
        cancelCapture(); showStatus(row,rejection(key,reason)); return true
    end
    scope.own(function() view.reject_reserved_key=nil end)
    for _,entry in ipairs(api.hotkeys.list()) do
        ids[#ids+1]=entry.id
        local block=ui.section(scope,rows)
        view.padding(block,8,8)
        local row=scope.native('HorizontalBox',block)
        local context=table.concat(entry.context,', ')
        if #entry.context>2 then
            context=table.concat(entry.context,', ',1,2)..'\n'..table.concat(entry.context,', ',3)
        end
        text(cell(row,widths[1]),entry.description)
        text(cell(row,widths[2]),({FillAPotPlus='Fill a Pot+',MatchRulesPlusPlus='Match Rules +',BaseWorldPlus='Editor tweaks',PrivateCosmetics='Private Cosmetics'})[entry.package]or entry.package)
        text(cell(row,widths[3]),context)
        local status=text(cell(block,1350),'')
        local capture
        capture=ui.button(scope,cell(row,widths[4],48),{label=entry.chord,single_line=true,on_click=function()
            cancelCapture()
            captureRow=controls[entry.id]
            capture.set_label('Press a key')
            showStatus(captureRow,'Ctrl, Shift and Alt can be combined.')
            captureCancel=api.hotkeys.capture(function(chord)
                local activeRow=captureRow
                captureCancel=nil; captureRow=nil
                capture.set_label(activeRow.chord)
                local good,reason=api.hotkeys.check(entry.id,chord)
                if good then api.hotkeys.set(entry.id,chord)
                else showStatus(activeRow,rejection(chord,reason)) end
            end,function() return scope.live(capture.widget) end)
        end})
        local reset=ui.button(scope,cell(row,widths[5],48),{label='Reset',single_line=true,tooltip='Reset to '..entry.default_chord,
            on_click=function() cancelCapture(); api.hotkeys.reset(entry.id) end})
        for _,button in ipairs({capture,reset}) do
            E.call(button.widget.Label.Slot,'SetHorizontalAlignment',0)
            E.call(button.widget.Label,'SetJustification',0)
            E.call(button.widget.Label,'SetTextOverflowPolicy',1)
        end
        local disabled=view.checkbox(cell(row,widths[6],48),'Disabled',entry.chord=='Disabled',function(value)
                cancelCapture()
                if value then api.hotkeys.set(entry.id,'Disabled') else api.hotkeys.reset(entry.id) end
            end,scope,widths[6])
        controls[entry.id]={id=entry.id,status=status,capture=capture,disabled=disabled,chord=entry.chord}
        view.separator(rows,scope)
        local buttons={capture,reset,disabled}
        for i,button in ipairs(buttons) do
            columns[i][#columns[i]+1]=button
            for _,widget in ipairs({button.widget,E.read(button.widget,'FocusableWidget')}) do
                if E.valid(widget) then
                    if buttons[i-1] then E.call(widget,'SetNavigationRuleExplicit',0,buttons[i-1].widget) end
                    if buttons[i+1] then E.call(widget,'SetNavigationRuleExplicit',1,buttons[i+1].widget) end
                end
            end
        end
    end
    for _,column in ipairs(columns) do ui.navigate(column,view.keys.widget) end
    view.key_signature=table.concat(ids,'\n')
    local function refresh()
        for _,entry in ipairs(api.hotkeys.list()) do
            local row=controls[entry.id]
            if row then
                row.chord=entry.chord
                row.capture.set_label(captureRow==row and 'Press a key' or entry.chord)
                row.disabled.set(entry.chord=='Disabled')
                local message=entry.conflict and 'Shortcut conflict: '..entry.conflict or ''
                row.message=message
                if captureRow~=row then showStatus(row,message) end
            end
        end
    end
    scope.own(api.on('hotkeys_changed',refresh,{ungated=true})); refresh()
end
function M.build(api,ui,scope,rows,screen,tab)
        ui.header(scope,rows,'Hotkeys')
        local controls={}
        local refresh
        local keyOptions={{value='Disabled',label='Disabled'}}
        for _,key in ipairs(api.hotkeys.keys()) do keyOptions[#keyOptions+1]={value=key,label=key} end
        local modOptions={}
        for _,chord in ipairs({'','Ctrl+','Shift+','Alt+','Shift+Ctrl+','Ctrl+Alt+','Shift+Alt+','Shift+Ctrl+Alt+'}) do
            modOptions[#modOptions+1]={value=chord,label=chord=='' and 'None' or chord:sub(1,-2)}
        end
        for _,entry in ipairs(api.hotkeys.list()) do
            local section=ui.section(scope,rows,(({FillAPotPlus='Fill a Pot+',MatchRulesPlusPlus='Match Rules +',BaseWorldPlus='Editor tweaks',PrivateCosmetics='Private Cosmetics'})[entry.package]or entry.package)..': '..entry.description)
            local status=ui.header(scope,section,'')
            local key=entry.chord:match('([^+]+)$'); local mods=entry.chord:sub(1,#entry.chord-#key)
            local keyRow,modRow
            local function save()
                local selected=keyRow.get()
                local chord=selected=='Disabled' and selected or modRow.get()..selected
                local good,reason=api.hotkeys.check(entry.id,chord)
                if good then api.hotkeys.set(entry.id,chord) else refresh(); status:SetText(FText('Rejected: '..reason)) end
            end
            keyRow=ui.dropdown(scope,section,{label='Key',value=key,options=keyOptions,on_change=save})
            modRow=ui.dropdown(scope,section,{label='Modifiers',value=mods,options=modOptions,on_change=save})
            local captureCancel
            local capture=ui.button(scope,section,{label='Press a key',on_click=function()
                if captureCancel then captureCancel() end
                status:SetText(FText('Ctrl, Shift and Alt can be combined.'))
                captureCancel=api.hotkeys.capture(function(chord)
                    local good,reason=api.hotkeys.check(entry.id,chord)
                    if good then api.hotkeys.set(entry.id,chord)
                    else status:SetText(FText('Rejected: '..reason)) end
                end,function() return scope.live() end)
            end})
            scope.own(function() if captureCancel then captureCancel() end end)
            local reset=ui.button(scope,section,{label='Restore default: '..entry.default_chord,on_click=function() api.hotkeys.reset(entry.id) end})
            controls[entry.id]={status=status,key=keyRow,mods=modRow}
            tab.initial_focus=tab.initial_focus or keyRow.widget
        end
        refresh=function()
            for _,entry in ipairs(api.hotkeys.list()) do
                local row=controls[entry.id]
                if row then
                    local key=entry.chord:match('([^+]+)$')
                    row.key.set(key); row.mods.set(entry.chord:sub(1,#entry.chord-#key))
                    row.status:SetText(FText((entry.conflict and 'Shortcut conflict: '..entry.conflict or entry.chord)
                        ..' | '..table.concat(entry.context,', ')..''))
                end
            end
        end
        scope.own(api.on('hotkeys_changed',refresh,{ungated=true})); refresh()
end
function M.install(api,Kit)
    local ui=Kit.new(api)
    ui.bind.match_rules_tab({label='Mod hotkeys',build=function(...) M.build(api,ui,...) end})
end
return M
