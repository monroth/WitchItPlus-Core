return function(R, C, U, I)
    local settings_class = '/Game/UserInterface/Settings/WB_SettingsTabs.WB_SettingsTabs_C'
    local directory = debug.getinfo(1, 'S').source:match('^@(.+[\\/])')..'../'
    local descriptions = {Core='Shared mod services.', UIKit='Mod settings and shortcuts.',
        FillAPotPlus='Fill a Pot+ rules and map tools.', MatchRulesPlusPlus='Rule presets and extra match options.',
        BaseWorldPlus='Teleporters and scenery editing.', PrivateCosmetics='Outfit previews and saved sets. Unowned items require your own custom game without anti-cheat; advertising is allowed.', CreativeUnlocks='All props available in the local Creative palette.'}
    local locations = {FillAPotPlus='Settings: Custom Match → Game Rules → Fill a Pot+.',
        MatchRulesPlusPlus='Settings: Custom Match → Game Rules → Match Rules ++.',
        PrivateCosmetics='Settings: Customize → Private Cosmetics / Custom Sets.'}
    local policy = C.mods.config
    -- The first mod failure of a run, as a window in the main menu: in a match it would
    -- take the controls.
    local failure
    -- Options tab: each mod's options under its name. build(rows) returns the controls it added.
    local option_groups = {}
    function U.mod_options(name, build) option_groups[#option_groups + 1] = {name=name, build=build} end
    local function showFailure()
        if not failure then return end
        U.message('Mod error',failure)
        failure=nil
    end
    R.on('failure',function(why)
        local id=tostring(why):match('(%w+)[/\\][%w_]+%.lua:%d+')
        failure=(C.mods.names[id] or 'A mod')..' stopped working. Restart the game. If it happens again, send UE4SS.log to the modpack author.'
        local gi=C.session().gi
        if gi and not gi.IsCurrentlyInMatch then R.next_frame(showFailure) end
    end)
    R.post('/Game/UserInterface/WB_MainMenu.WB_MainMenu_C:Construct', function()
        if C.mods.refusal then
            local reason=C.mods.refusal;C.mods.refusal=nil
            R.next_frame(function() U.message('Could not join',reason) end)
        end
        R.next_frame(showFailure)
    end)
    local opened, pending, mainMenu
    local installed
    local stop_capture
    function I.packages()
        if installed then return installed end
        installed = C.mods.status().packages
        for _, row in ipairs(installed) do
            row.original, row.enabled, row.path = row.enabled, row.next_enabled, directory..row.id..'/manifest.lua'
        end
        return installed
    end
    local update_texts={unavailable='Updates are unavailable.',available='An update is available.',downloaded='Update downloaded. Restart the game to install it.',updated='Update installed.'}
    -- What an update brings, for its window and the Mods tab: names, versions and size.
    local function update_summary(notice)
        local rows={}
        for _,row in ipairs(notice.packages or {}) do
            local id,version=row:match('^(%S+) (%S+)$')
            rows[#rows+1]=(C.mods.names[id] or id)..' '..version
        end
        return table.concat(rows,'\n')..string.format('\n\n%.1f MB',(notice.bytes or 0)/1048576)
    end
    local function build(record, screen)
        record.screen, record.hidden = screen, {}
        local function title(widget)
            if widget:IsA(I.object('/Script/UMG.TextBlock')) then
                if I.string(widget:GetText())=='Settings' then return widget end
            elseif widget:IsA(I.object('/Script/UMG.PanelWidget')) then
                for _,child in ipairs(I.values(widget:GetAllChildren())) do local found=title(child);if found then return found end end
            end
        end
        record.title=title(screen.WidgetTree.RootWidget)
        if record.title then record.title:SetText(FText('Mods')) end
        local switcher = screen.SettingsSwitcher
        record.previous = switcher:GetActiveWidget()
        local page = I.native(switcher, 'ScaleBox')
        page:SetStretch(2)
        record.page = page
        local bounds = I.native(page, 'SizeBox')
        bounds:SetWidthOverride(1380)
        bounds:SetHeightOverride(720)
        local root = U.section(bounds, {})
        local function checkbox(parent,opts,width)
            width=width or 950
            local cells=U.row(parent,{width-52,52})
            U.text(cells[1],{text=opts.label,width=width-64})
            opts.label='';opts.editable=false
            local widget,set=U.checkbox(cells[2],opts)
            widget.DescriptiveLabelText:GetParent():SetVisibility(1)
            local glyph=widget.CheckBox;local grid=glyph:GetParent()
            for column=0,2 do grid:SetColumnFill(column,0) end
            glyph.Slot:SetColumn(0);glyph.Slot:SetPadding({Left=0,Top=0,Right=0,Bottom=0})
            glyph.Slot:SetHorizontalAlignment(1);glyph.Slot:SetVerticalAlignment(2)
            cells[2]:SetHeightOverride(48)
            return widget,set
        end
        local function separator(parent)
            local box=I.native(parent,'SizeBox');box:SetHeightOverride(1)
            I.native(box,'Image'):SetOpacity(.2)
        end
        local tabbar = screen.TabButton_Game:GetParent()
        for _, widget in ipairs(I.values(tabbar:GetAllChildren())) do
            record.hidden[#record.hidden + 1] = {widget=widget, visibility=widget:GetVisibility()}
            widget:SetVisibility(1)
        end
        record.hidden[#record.hidden + 1] = {widget=screen.Button_Apply, visibility=screen.Button_Apply:GetVisibility()}
        screen.Button_Apply:SetVisibility(1)
        local packages = U.scroll(root, {height=684})
        local shortcuts = U.scroll(root, {height=684})
        local options = U.scroll(root, {height=684})
        local downloads = U.scroll(root, {height=684})
        if #option_groups == 0 then U.text(U.section(options, {}), {text='No mod options yet.'}) end
        local controls = {}
        for _, group in ipairs(option_groups) do
            local rows = U.section(options, {})
            U.text(rows, {text=group.name})
            for _, control in ipairs(group.build(rows)) do controls[#controls + 1] = control end
        end
        U.navigate(controls, options)
        U.text(U.section(downloads, {}), {text='Mod downloads are not available yet.'})
        local report = U.scroll(root, {height=684})
        local report_rows = U.section(report, {})
        U.text(report_rows, {text='Something went wrong? Save your logs, then tell us what you saw and attach the file. It contains your game logs and mod settings.'})
        U.button(report_rows, {label='Save logs', on_click=function()
            U.message('Bug report','Logs saved to:\n'..C.save_report()..'\n\nTell us what you saw and attach this file.')
        end})
        local rows = U.section(packages, {})
        U.text(rows, {text='Mods — restart the game to apply package changes.'})
        local update = C.mods.status()
        local _, update_status = U.text(rows, {text=''})
        local download = U.button(rows, {label='Download update', on_click=function()
            C.mods.request('latest');record.refresh()
        end})
        checkbox(rows, {label='Auto-update', value=policy.auto_update or false, enabled=not update.private,
            tooltip='Install available updates when the game restarts.', on_change=function(value)
                policy.auto_update=value;R.save_settings('Core');C.mods.status();if record.refresh then record.refresh() end
            end})
        local _,dashboard=U.text(rows,{text=''})
        local polling=false
        record.refresh=function()
            local facts=C.mods.status();local lines={}
            local notice=facts.update
            local file=C.mods.downloading and C.mods.progress()
            update_status(file and string.format('Downloading %s: %.1f of %.1f MB',C.mods.names[file.package] or file.package,
                    (file.done or 0)/1048576,file.total/1048576)
                or facts.private and 'Private build: update through the local installer.'
                or notice and notice.kind=='available' and 'Update available:\n'..update_summary(notice)
                or notice and update_texts[notice.kind] or '')
            download:SetVisibility(notice and notice.kind=='available' and not C.mods.downloading and 0 or 1)
            -- Only while a download runs and this page is open.
            if C.mods.downloading and not polling then
                polling=true
                R.after(1000,function()
                    polling=false
                    if opened==record then record.refresh() end
                end)
            end
            if facts.message then lines[#lines+1]=facts.message end
            if facts.restart_required then lines[#lines+1]='Restart the game to apply changes.' end
            if facts.rehost_required then lines[#lines+1]='Host a new game to apply the Vanilla server setting.' end
            local peers={};for _,peer in pairs(facts.peers) do peers[#peers+1]=peer end
            table.sort(peers,function(a,b)return a.name<b.name end)
            for _,peer in ipairs(peers) do
                lines[#lines+1]=peer.name..': '..(peer.message or (peer.compatible and 'Mod versions match.' or 'Checking mods…'))
            end
            if #peers==0 then lines[#lines+1]=facts.host and 'Host mod information received.' or 'No other players connected.' end
            dashboard(table.concat(lines,'\n'))
        end
        record.refresh()
        local delay
        checkbox(rows, {label='Kick for mod mismatch', value=policy.RequireModpack, on_change=function(value)
            policy.RequireModpack=value;R.save_settings('Core');delay:SetIsEnabled(value)
        end})
        delay=U.slider(rows, {label='Kick after (s)', kind='int', min=5, max=20, step=1, default=10, value=policy.KickAfterSeconds,
            enabled=policy.RequireModpack, on_change=function(value)
                policy.KickAfterSeconds=value;R.save_settings('Core')
            end})
        for _, package in ipairs(I.packages()) do
            local cells,packageRow = U.row(rows, {995,355})
            packageRow.Slot:SetPadding({Left=0,Top=12,Right=0,Bottom=12})
            local section = U.section(cells[1], {})
            local toggle = U.section(cells[2], {})
            U.text(section, {text=package.name..'  '..package.version..'  ·  '..(package.original and 'Enabled' or 'Disabled'),wrap=false})
            U.text(section, {text=descriptions[package.id],width=947})
            if locations[package.id] then U.text(section, {text=locations[package.id],width=947}) end
            if package.id == 'Core' or package.id == 'UIKit' then U.text(toggle, {text='Core component'})
            else
                local _, pending_text = U.text(toggle, {text=package.enabled ~= package.original and 'Pending' or ''})
                checkbox(toggle, {label='Enabled', value=package.enabled, on_change=function(value)
                    local bytes = assert(R.native.file_read(package.path))
                    local count
                    bytes, count = bytes:gsub('(enabled%s*=%s*)%a+', '%1'..tostring(value), 1)
                    assert(count == 1, 'Package manifest has no enabled preference')
                    assert(R.native.file_write(package.path, bytes))
                    package.enabled = value
                    pending_text(value ~= package.original and 'Pending' or '')
                    if record.refresh then record.refresh() end
                end},355)
            end
            separator(rows)
        end
        local key_rows = U.section(shortcuts, {})
        U.text(key_rows, {text='Shortcuts'})
        U.text(key_rows,{text='Select a shortcut and press a key. Ctrl, Shift and Alt can be combined. F5 is reserved.'})
        local names={[32]='Space',[9]='Tab',[13]='Enter',[45]='Insert',[46]='Delete',[36]='Home',[35]='End',[33]='PageUp',[34]='PageDown'}
        for i=48,90 do names[i]=string.char(i) end
        for i=1,24 do names[111+i]='F'..i end
        local widths={310,240,320,200,95,185}
        local headers=U.row(key_rows,widths)
        for i,text in ipairs({'Action','Mod','Available in','Shortcut','Reset','Disabled'}) do U.text(headers[i],{text=text,wrap=false}) end
        local columns={{},{},{}}
        for _,binding in ipairs(R.hotkeys()) do
            local section=U.section(key_rows,{})
            local cells=U.row(section,widths)
            U.text(cells[1],{text=binding.label,width=300})
            U.text(cells[2],{text=binding.mod,width=230})
            U.text(cells[3],{text=binding.context or 'Creative',width=310})
            local _, status=U.text(section,{text=''})
            local button,caption,disable
            local function apply(chord)
                local ok,why=R.set_hotkey(binding.id,chord)
                if not ok then status('Rejected: '..why);return end
                binding.current=chord;caption(chord);disable(chord=='Disabled');status('Shortcut saved.')
            end
            button,caption=U.button(cells[4],{label=binding.current,tooltip='Default: '..binding.default,on_click=function()
                if stop_capture then stop_capture() end
                caption('Press a key');status('Ctrl, Shift and Alt can be combined. Esc cancels.')
                local cancel=R.capture_hotkey(function(code,mask)
                    -- Native Back cancels capture and consumes its own transition.
                    if code==27 then return end
                    local name=names[code];if not name then return end
                    local prefix=(mask & 2 ~= 0 and 'Ctrl+' or '')..(mask & 1 ~= 0 and 'Shift+' or '')..(mask & 4 ~= 0 and 'Alt+' or '')
                    stop_capture();apply(prefix..name)
                end)
                stop_capture=function() cancel();stop_capture=nil;caption(binding.current);status('') end
            end})
            local reset=U.button(cells[5],{label='Reset',tooltip='Reset to '..binding.default,on_click=function() apply(binding.default) end})
            local disabled
            disabled,disable=checkbox(cells[6],{label='Disabled',value=binding.current=='Disabled',on_change=function(value)
                apply(value and 'Disabled' or binding.default)
            end},185)
            local buttons={button,reset,disabled}
            for column,widget in ipairs(buttons) do
                columns[column][#columns[column]+1]=widget
                local function link(target)
                    if buttons[column-1] then target:SetNavigationRuleExplicit(0,buttons[column-1]) end
                    if buttons[column+1] then target:SetNavigationRuleExplicit(1,buttons[column+1]) end
                end
                link(widget)
                for _,key in ipairs({'FocusableWidget','CheckBox','ValueSlider','ButtonRef'}) do
                    local inner=R.valid(widget[key])
                    if inner then link(inner);break end
                end
            end
        end
        for _,column in ipairs(columns) do U.navigate(column,shortcuts) end
        record.tabs = {}
        local pages = {packages, shortcuts, options, downloads, report}
        local function select(chosen)
            if stop_capture then stop_capture() end
            for index, page in ipairs(pages) do
                page:GetParent():SetVisibility(index == chosen and 0 or 1)
                record.tabs[index]:SetActiveState(index == chosen)
            end
        end
        for index, text in ipairs({'Mods', 'Hotkeys', 'Options', 'Downloads', 'Report'}) do
            record.tabs[index] = U.button(tabbar, {class=screen.TabButton_Game:GetClass(), label=text,
                on_click=function() select(index) end})
            I.slot_apply(record.tabs[index].Slot, I.slot_values(screen.TabButton_Game.Slot))
        end
        select(1)
    end
    local function finish(manager)
        local record = pending or opened
        if not record or manager.CurrentMenuState ~= 11 then return end
        local screen
        manager.MenuStateWidgets:ForEach(function(key, value) if key:get() == 11 then screen=value:get() end end)
        if not R.valid(screen) then return end
        if pending then
            opened, pending = record, nil
            build(record, screen)
        end
        screen.SettingsSwitcher:SetActiveWidget(record.page)
        I.focus(record.tabs[1], screen)
    end
    local function close()
        if stop_capture then stop_capture();stop_capture=nil end
        local record = opened
        if not record then pending=nil; return end
        opened = nil
        if record.title then record.title:SetText(FText('Settings')) end
        for _, entry in ipairs(record.hidden) do entry.widget:SetVisibility(entry.visibility) end
        for _, tab in ipairs(record.tabs) do
            I.actions[I.id(tab)] = nil
            I.remove(tab)
        end
        record.screen.SettingsSwitcher:SetActiveWidget(record.previous)
        I.remove(record.page)
        I.restore_after_close(record.saved)
    end
    U.menu_entry('WB_MainMenu', 'Settings', 'Mods', function(origin)
        local manager = I.object('/Script/Engine.Default__GameplayStatics'):GetGameInstance(origin).MenuUIManager
        local saved = I.snapshot(origin)
        pending = {saved=saved}
        I.change_menu(manager,11,function() finish(manager) end)
    end)
    local function banner(screen)
        local state=I.state(screen)
        if not state.update_notice then
            local widget,set=U.text(screen.Settings:GetParent(),{text='',width=400})
            state.update_notice={widget=widget,set=set}
        end
        local update=C.mods.status()
        state.update_notice.set(update.restart_required and 'Restart the game to apply changes.'
            or update.update and update_texts[update.update.kind] or '')
    end
    -- A finished download says so once, in the main menu.
    local shown_update
    local function show_update()
        local notice=C.mods.status().update
        if not mainMenu or not notice or notice.kind==shown_update then return end
        if notice.kind=='downloaded' then
            shown_update=notice.kind
            U.message('Update downloaded',update_summary(notice)..'\n\nRestart the game to install it.')
        end
    end
    R.post('/Game/UserInterface/WB_MainMenu.WB_MainMenu_C:Construct',function(screen)
        mainMenu=screen;banner(screen);R.next_frame(show_update)
    end)
    C.mods.on_change(function()
        if opened and opened.refresh then opened.refresh() end
        if mainMenu then banner(mainMenu);show_update() end
    end)
    R.pre('/Script/UMG.UserWidget:Destruct',function(screen)
        if mainMenu and I.id(screen)==I.id(mainMenu) then mainMenu=nil end
    end)
    R.on('world_end',function()mainMenu=nil end)
    R.pre(I.path('B_MenuUIManager', 'ChangeMenu', I.managers), function()
        if stop_capture then stop_capture();return true end
    end)
    R.post(I.path('B_MenuUIManager', 'ChangeMenu', I.managers), function(manager)
        if manager.CurrentMenuState == 11 then finish(manager) elseif opened or pending then close() end
    end)
    R.post(settings_class..':FocusStoredWidget', function(screen)
        finish(I.object('/Script/Engine.Default__GameplayStatics'):GetGameInstance(screen).MenuUIManager)
    end)
    R.post(settings_class..':GetInitialFocusWidget', function(screen, out)
        finish(I.object('/Script/Engine.Default__GameplayStatics'):GetGameInstance(screen).MenuUIManager)
        if opened then out:set(opened.tabs[1]) end
    end)
    R.post(settings_class..':SetActiveSettingsTab', function(screen)
        if opened then screen.SettingsSwitcher:SetActiveWidget(opened.page) end
    end)
    R.post('/Game/UserInterface/CreativeMode/WB_CreativeGame.WB_CreativeGame_C:Construct', function(screen)
        local state = I.state(screen)
        if not state.creative_vanilla then
            local widget, set = U.checkbox(screen.CheckBox_Public:GetParent(), {
                editable=false,label='Vanilla server', value=policy.CreativeVanillaServer, default=false,
                on_change=function(value) policy.CreativeVanillaServer=value; R.save_settings('Core') end})
            widget.Slot:SetPadding(screen.CheckBox_Public.Slot.Padding)
            state.creative_vanilla = {widget=widget, set=set}
        end
        state.creative_vanilla.set(policy.CreativeVanillaServer)
    end)
    local custom = I.play..'WB_Custommatch.WB_Custommatch_C'
    local function custom_match(screen)
        local state = I.state(screen)
        if not state.vanilla then
            local widget, set = U.checkbox(screen.CheckBox_DisableStartTimer:GetParent(), {
                editable=false,label='Vanilla server', value=policy.VanillaServer, default=true,
                on_change=function(value) policy.VanillaServer=value; R.save_settings('Core') end})
            state.vanilla = {widget=widget, set=set}
        end
        local session = C.session()
        local mode = I.string(screen.ComboBox_GameMode.SelectedOption)
        state.vanilla.widget:SetVisibility(mode == 'Fill a Pot+' and 1 or 0)
        state.vanilla.widget:SetIsEnabled(session.role ~= 'host')
        if session.role == 'host' then state.vanilla.set(session.is_vanilla_session) else state.vanilla.set(policy.VanillaServer) end
    end
    for _, event in ipairs({'Construct', 'LoadCustomMatchOptions', 'UpdateWidgetVisibility',
        'BndEvt__ComboBox_GameMode_K2Node_ComponentBoundEvent_0_OnSelectionChanged__DelegateSignature'}) do
        R.post(custom..':'..event, function(screen)
            -- FAP+ restores its selected label after the native options callbacks finish.
            R.next_frame(function() custom_match(screen) end)
        end)
    end
    C.on_session(function()
        for _, screen in ipairs(R.find_all(custom)) do custom_match(screen) end
    end)
    I.resets[#I.resets + 1] = function() if stop_capture then stop_capture();stop_capture=nil end;opened, pending = nil, nil end
end
