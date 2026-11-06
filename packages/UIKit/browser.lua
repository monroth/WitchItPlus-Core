return function(R, C, U, I)
    local list = I.play..'WB_ServerList.WB_ServerList_C'
    local row = I.play..'WB_ServerRow.WB_ServerRow_C'
    local mods = C.mods
    local unreadable = 'Update the mods and restart the game. This server uses unreadable or unsupported mod information.'
    -- The modpack advert of a listed server, and whether the server carries one at all.
    local function advertisement(data)
        local entries, key = data.ExtraSettings, FName('WitchItModpack')
        if not entries:Contains(key) then return nil, entries:Contains(FName('WitchItModpackSession')) end
        local bytes = I.string(entries:Find(key):get().Data.AsString)
        if #bytes > 16384 then return nil, true end
        return mods.parse_advert(bytes), true
    end
    local function describe(host)
        local local_versions, lines, compatible = {}, {}, true
        for _, package in ipairs(I.packages()) do
            local wire = mods.wires[package.id]
            if wire and package.original then local_versions[wire] = package.version end
        end
        for _, id in ipairs({'core', 'fillapotplus', 'matchrulesplusplus', 'baseworldplus'}) do
            local remote, own = host.packages[id], local_versions[id]
            local required = host.required[id]
            if remote or own then
                local name = mods.names[mods.packageOf[id]]
                lines[#lines + 1] = name..': host '..(remote or 'disabled')..' / local '..(own or 'disabled')
                if required and own ~= remote then
                    compatible = false
                    if own and remote then
                        local older = mods.compare(own, remote) < 0
                        lines[#lines + 1] = (older and 'This player' or 'The host')..' must update '..name..'.'
                    else lines[#lines + 1] = 'Enable matching mods and restart the game.' end
                end
            end
        end
        return table.concat(lines, '\n'), compatible
    end
    local rowViews={}
    local function row_state(widget)
        local data=widget.ServerRowDataObject
        for _,owner in pairs(I.screens) do
            local state=owner.browser
            -- The list can close while a search is still reporting rows.
            if state and R.valid(state.screen) then
                for _,found in ipairs(I.values(state.screen.FoundServers)) do
                    if I.id(found)==I.id(data) then return state end
                end
            end
        end
    end
    local function enable_row(record)
        local widget=record.widget;local state=row_state(widget)
        if state and state.modded then
            widget.SelectServer:SetIsEnabled(true);record.overridden=true
        elseif record.overridden then
            widget.SelectServer:SetIsEnabled(widget:Get_SelectServer_bIsEnabled_0());record.overridden=false
        end
    end
    local function refresh_rows()for _,record in pairs(rowViews)do enable_row(record)end end
    R.post(row..':SetWidgetValues',function(widget)
        local record=rowViews[I.id(widget)] or {widget=widget}
        rowViews[I.id(widget)]=record;enable_row(record)
    end)
    R.post(row..':BP_OnEntryReleased',function(widget)
        local record=rowViews[I.id(widget)]
        if record and record.overridden then widget.SelectServer:SetIsEnabled(widget:Get_SelectServer_bIsEnabled_0()) end
        rowViews[I.id(widget)]=nil
    end)
    R.pre('/Script/UMG.UserWidget:Destruct',function(widget)rowViews[I.id(widget)]=nil end)
    I.resets[#I.resets+1]=function()rowViews={}end
    local function clear(state)
        state.selected, state.host = nil, nil
        state.info('Select a server to see its mods.')
        state.join:SetIsEnabled(false)
        state.sync:SetIsEnabled(false)
    end
    local function select(state, data)
        local host = advertisement(data)
        state.selected, state.host = data, host
        local text, compatible = unreadable, false
        if host then
            text, compatible = describe(host)
            local policy = host.vanilla and 'Vanilla server. Plus modes, map edits and extra rules are off for everyone.'
                or host.require_modpack and 'Modpack required. Mod versions must match.' or 'Players without mods can join.'
            text = policy..'\n\n'..text..'\n\n'..(compatible and 'Versions match.' or 'Choose Sync mods with host to use this server’s mods.')
        end
        state.info(I.string(data:GetServerName())..'\n\n'..text)
        state.join:SetIsEnabled(compatible)
        state.sync:SetIsEnabled(host ~= nil)
    end
    local function results(state)
        local selected
        for _, data in ipairs(I.values(state.screen.FoundServers)) do
            if state.selected and I.id(state.selected) == I.id(data) then selected=data end
        end
        if selected then select(state, selected) else clear(state) end
        refresh_rows()
    end
    R.post(list..':ServerPassesCurrentFilter',function(screen,data,out)
        local owner=I.screens[I.id(screen)]
        if owner and owner.browser and owner.browser.modded then
            local _,present=advertisement(data)
            out:set(out:get() and present)
        end
    end)
    local function construct(screen)
        local state = I.state(screen)
        if state.browser then return end
        state.browser = {screen=screen}
        state = state.browser
        local root = screen.WidgetTree.RootWidget
        local body = screen.ServerListView
        while I.id(body:GetParent()) ~= I.id(root) do body = body:GetParent() end
        state.body = body
        local offsets, anchors = body.Slot:GetOffsets(), body.Slot:GetAnchors()
        state.wide = {Left=offsets.Left, Top=offsets.Top, Right=offsets.Right, Bottom=offsets.Bottom}
        state.narrow = {Left=offsets.Left, Top=offsets.Top,
            Right=offsets.Right + (anchors.Maximum.X > anchors.Minimum.X and 460 or -460), Bottom=offsets.Bottom}
        local panel = I.native(root, 'Border')
        state.panel = panel
        panel:SetBrushColor({R=.025,G=.035,B=.05,A=.97})
        panel:SetPadding({Left=18,Top=16,Right=18,Bottom=16})
        panel.Slot:SetAnchors({Minimum={X=1,Y=.28},Maximum={X=1,Y=.90}})
        panel.Slot:SetOffsets({Left=-450,Top=0,Right=440,Bottom=0})
        panel.Slot:SetZOrder(20)
        local content = U.section(panel, {})
        local info = U.scroll(content, {height=450})
        local info_rows = U.section(info, {})
        local info_widget
        info_widget, state.info = U.text(info_rows, {text='',width=400})
        state.join = U.button(content, {label='Join', on_click=function()
            local data, gi = state.selected, I.object('/Script/Engine.Default__GameplayStatics'):GetGameInstance(screen)
            local entries, key = data.ExtraSettings, FName('IsPasswordProtected')
            if entries:Contains(key) and entries:Find(key):get().Data.AsBool then gi.MenuUIManager:ShowPasswordDialog(data.OnlineSession)
            else gi:JoinFromServerList(data.OnlineSession) end
        end})
        local notice
        state.sync = U.button(content, {label='Sync mods with host', on_click=function()
            local packages = {}
            for wire, value in pairs(state.host.packages) do packages[mods.packageOf[wire]] = value end
            local ok,why=C.mods.request('exact',packages)
            if not ok then notice(why);return end
            notice('Sync requested. Restart the game to apply.')
        end})
        local notice_widget
        notice_widget, notice = U.text(content, {text='',width=400})
        state.tab = U.button(screen.Button_Filter_All:GetParent(), {class=screen.Button_Filter_All:GetClass(), label='Modded', on_click=function()
            state.modded = true
            state.internal = true
            screen:ChangeFilter(0)
            state.internal = nil
            screen.ServerListView:SetVisibility(0)
            body.Slot:SetOffsets(state.narrow)
            panel:SetVisibility(0)
            state.tab:SetActiveState(true)
            results(state)
        end})
        I.slot_apply(state.tab.Slot, I.slot_values(screen.Button_Filter_All.Slot))
        panel:SetVisibility(1)
        clear(state)
    end
    I.destructors[#I.destructors + 1] = function(state)
        if state.browser then
            state.browser.modded=false;refresh_rows()
            state.browser.body.Slot:SetOffsets(state.browser.wide)
            state.screen.ServerListView:SetVisibility(0)
        end
    end
    R.post(list..':Construct', construct)
    R.post(list..':AddServersSorted',function(screen)
        local state=I.screens[I.id(screen)]
        if state and state.browser then results(state.browser) end
    end)
    R.post(list..':RemoveAllServers',function(screen)
        local state=I.screens[I.id(screen)]
        if state and state.browser then clear(state.browser) end
    end)
    R.post(list..':UpdateTabButtonStyles', function(screen)
        local state = I.screens[I.id(screen)]
        if state and state.browser then state.browser.tab:SetActiveState(state.browser.modded == true) end
    end)
    R.post(list..':ChangeFilter', function(screen)
        local state = I.screens[I.id(screen)]
        if not state or not state.browser then return end
        state = state.browser
        if state.internal then return end
        state.modded = false
        refresh_rows()
        state.panel:SetVisibility(1)
        state.tab:SetActiveState(false)
        screen.ServerListView:SetVisibility(0)
        state.body.Slot:SetOffsets(state.wide)
    end)
    for _, event in ipairs({'ClickedServer', 'ProceedJoin', 'BndEvt__Button_534_K2Node_ComponentBoundEvent_280_OnButtonClickedEvent__DelegateSignature'}) do
        R.pre(row..':'..event, function(widget)
            local data = widget.ServerRowDataObject
            for _, owner in pairs(I.screens) do
                local state = owner.browser
                if state and state.modded then
                    for _, found in ipairs(I.values(state.screen.FoundServers)) do
                        if I.id(found) == I.id(data) then select(state, data); return true end
                    end
                end
            end
            local host, present = advertisement(data)
            if not present then return end
            if not host then U.message('Mods', unreadable); return true end
            local text, compatible = describe(host)
            if not compatible then U.message('Mods', text); return true end
        end)
    end
end
