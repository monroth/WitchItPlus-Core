return function(R, C, U, I)
    local rules = I.play..'GameRules/'
    local class = rules..'WB_GameRulesTabs.WB_GameRulesTabs_C'
    local declarations, pending, returning, closing = {}, nil, nil, nil
    local cancelTransition
    -- The row each native panel carries, by panel address and caption. A cached screen
    -- keeps its rows after Lua forgets the screen, so this table outlives every world.
    local rows={}
    -- ChangeMenu may run InitializeGameModes; never nest it inside a hook.
    function I.change_menu(manager, target, done)
        if cancelTransition then cancelTransition() end
        cancelTransition=R.next_frame(function()
            cancelTransition=nil
            if not R.valid(manager) then return end
            manager:ChangeMenu(target,true)
            if done then done() end
        end)
    end
    local function class_path(id)
        if id:sub(1, 1) == '/' then return id:gsub(':.*$', '') end
        local folder = id == 'WB_MainMenu' and '/Game/UserInterface/' or I.play
        return folder..id..'.'..id..'_C'
    end
    local function navigation(widget,direction,target)
        widget:SetNavigationRuleExplicit(direction,target)
        local inner=widget.ButtonRef
        if R.valid(inner) then inner:SetNavigationRuleExplicit(direction,target) end
    end
    -- A menu row under the anchor row of a native screen; on_click receives the clicked button.
    -- room names a hidden native widget whose blank space the row takes, for a menu
    -- with no spare height (Creative, PROBE16). when, if given, says whether this session has
    -- the row; caption, if given, gives its current label.
    function U.menu_entry(screen_id, anchor, text, on_click, room, when, caption)
        local path = class_path(screen_id)
        local function click(button)
            on_click(button)
            if caption then I.caption(button,caption()) end
        end
        local function layout(screen)
            local button=I.state(screen).menus[text]
            if not button then return end
            if caption then I.caption(button,caption()) end
            local template,row=screen[anchor],button:GetParent()
            local parent=template:GetParent()
            local blank=room and screen[room]
            if blank and blank:GetVisibility()==2 then blank:SetVisibility(1) end
            parent:ForceLayoutPrepass()
            local tail,found={},false
            for _,child in ipairs(I.values(parent:GetAllChildren())) do
                if I.id(child)==I.id(template) then found=true
                elseif I.id(child)==I.id(row) then break
                elseif found then tail[#tail+1]=child end
            end
            local distance=0
            for _,child in ipairs(tail) do
                if child:GetVisibility()~=1 then
                    local size=child:GetDesiredSize()
                    distance=distance+size.Y+child.Slot.Padding.Top+child.Slot.Padding.Bottom
                end
            end
            row:SetRenderTranslation({X=0,Y=-distance})
            navigation(template,3,button);navigation(button,2,template)
            for _,child in ipairs(tail) do
                if child:GetVisibility()~=1 then
                    navigation(button,3,child);navigation(child,2,button);break
                end
            end
            return tail
        end
        local function construct(screen)
            if when and not when() then return end
            local state=I.state(screen)
            -- A cached screen returns with a new wrapper.
            state.screen=screen
            if state.menus[text] then layout(screen);return end
            local template=screen[anchor]
            local parent=template:GetParent()
            local key=I.id(parent)..text
            for _,row in ipairs(I.values(parent:GetAllChildren())) do
                if I.id(row)==rows[key] then
                    local button=row:GetChildAt(0)
                    I.own(parent,row);I.own(row,button)
                    I.action(button,{click=click})
                    state.menus[text]=button
                    local original=I.slot_values(template.Slot).padding
                    -- The row's top padding records the gap already reserved.
                    original.Bottom=original.Bottom-original.Top+row.Slot.Padding.Top
                    state.menu_slots=state.menu_slots or {}
                    state.menu_slots[#state.menu_slots+1]={slot=template.Slot,padding=original}
                    layout(screen);return
                end
            end
            local row=I.native(parent,'SizeBox')
            rows[key]=I.id(row)
            local button=U.button(row,{class=template:GetClass(),label=text,on_click=click})
            I.slot_apply(row.Slot,I.slot_values(template.Slot))
            state.menus[text]=button
            -- Native hover animations own their buttons; only our plain row moves.
            local tail=layout(screen)
            if #tail>0 then
                local padding=I.slot_values(row.Slot).padding
                local height=row:GetDesiredSize().Y+padding.Top+padding.Bottom
                local original=I.slot_values(template.Slot).padding
                state.menu_slots=state.menu_slots or {}
                state.menu_slots[#state.menu_slots+1]={slot=template.Slot,padding=original}
                template.Slot:SetPadding({Left=original.Left,Top=original.Top,Right=original.Right,Bottom=original.Bottom+height})
                row.Slot:SetPadding({Left=padding.Left,Top=padding.Top-height,Right=padding.Right,Bottom=padding.Bottom})
            end
        end
        R.post(path..':Construct', construct)
        -- Native visibility can change before a question returns focus. The appended
        -- row ends the native tail; later owned notices never affect its offset.
        R.post(path..':GetInitialFocusWidget',function(screen) layout(screen) end)
        R.post(I.path('B_MenuUIManager','ShowConfirmationDialog',I.managers),function()
            -- Exit changes native visibility before opening its modal, while
            -- the originating menu remains visible behind it. The same label
            -- also belongs to other menu classes with different native anchors.
            for _,screen in ipairs(R.find_all(path)) do layout(screen) end
        end)
        for _, screen in ipairs(R.find_all(path)) do construct(screen) end
    end
    -- More tabs than the row holds: it shows a window of them and two arrows move it.
    local VISIBLE_TABS=7
    local function paint_tabs(screen)
        local state=I.state(screen)
        local buttons=I.values(screen.Buttons)
        if #buttons<=VISIBLE_TABS then return end
        if not state.tab_arrows then
            state.tab_first=1
            local bar=screen.TabButton_Mods:GetParent()
            local function step(by)
                return function()
                    state.tab_first=math.max(1,math.min(state.tab_first+by,#I.values(screen.Buttons)-VISIBLE_TABS+1))
                    paint_tabs(screen)
                end
            end
            state.tab_arrows={U.button(bar,{label='<',on_click=step(-1)}),U.button(bar,{label='>',on_click=step(1)})}
        end
        for index,button in ipairs(buttons) do
            button:SetVisibility(index>=state.tab_first and index<state.tab_first+VISIBLE_TABS and 0 or 1)
        end
    end
    local function add_tab(screen, declaration)
        local state = I.state(screen)
        local built = state.tabs[declaration]
        if built then
            if built.refresh then built.refresh() end
            return
        end
        if not state.rule_arrays then state.rule_arrays={buttons=I.values(screen.Buttons),pages=I.values(screen.RuleTabs)} end
        local scroll = U.scroll(screen.RulesSwitcher, {})
        local rows = U.section(scroll, {})
        local button = U.button(screen.TabButton_Mods:GetParent(), {label = declaration.label,
            on_click = function()
                screen:SetActiveTab(scroll)
                local tab = state.tabs[declaration]
                if tab.refresh then tab.refresh() end
            end})
        I.slot_apply(button.Slot, I.slot_values(screen.TabButton_Mods.Slot))
        local tab = {page = scroll, rows = rows, button = button}
        state.tabs[declaration] = tab
        local buttons, pages = I.values(screen.Buttons), I.values(screen.RuleTabs)
        buttons[#buttons + 1], pages[#pages + 1] = button, scroll
        I.array(screen.Buttons, buttons)
        I.array(screen.RuleTabs, pages)
        -- A build can return a function that repaints the tab each time it opens.
        tab.refresh = declaration.build(rows,scroll)
        if tab.refresh then tab.refresh() end
        screen:UpdateTabButtonStyles()
        paint_tabs(screen)
    end
    local function construct(screen)
        for _, declaration in ipairs(declarations) do add_tab(screen, declaration) end
    end
    function U.rules_tab(screen_id, label, build)
        local declaration = {id = screen_id, label = label, build = build}
        declarations[#declarations + 1] = declaration
        for _, screen in ipairs(R.find_all(class)) do add_tab(screen, declaration) end
        return declaration
    end
    local function finish(manager)
        if not pending or manager.CurrentMenuState ~= pending.target then return end
        local screen
        manager.MenuStateWidgets:ForEach(function(key, value)
            if key:get() == pending.target then screen = value:get() end
        end)
        if not R.valid(screen) then return end
        construct(screen)
        local page = pending.page
        for declaration, tab in pairs(I.state(screen).tabs) do
            if declaration == page or declaration.id == page or declaration.label == page then
                screen:SetActiveTab(tab.page)
                I.focus(I.initial(tab.rows) or tab.button,screen)
            end
        end
        if pending.own then returning = pending end
        pending = nil
    end
    function U.open_rules(origin, page)
        local saved = I.snapshot(origin)
        local pc, gi = saved.pc, saved.gi
        local manager, target, family = gi.MenuUIManager, nil, {}
        manager.MenuStateClasses:ForEach(function(key, value)
            local widget_class = value:get()
            if R.valid(widget_class) then
                local name = I.string(widget_class:GetFName())
                if widget_class:GetFullName():find('/Game/UserInterface/Play/GameRules/', 1, true) then family[key:get()] = true end
                if name == 'WB_GameRulesTabs_C' then target = key:get() end
            end
        end)
        assert(target, 'Native Game Rules menu is unavailable')
        pending = returning or {target=target, previous=manager.CurrentMenuState,
            saved=saved, family=family, manager=manager, ingame=gi.IngameUIManager, own=manager.CurrentMenuState ~= target}
        pending.page = page
        if manager.CurrentMenuState ~= target then
            pending.subordinate = not pc:IsA(I.object('/Game/Blueprints/Player/B_MenuPlayerController.B_MenuPlayerController_C'))
            if pending.subordinate then
                gi.IngameUIManager:OpenSubordinateMenu(true)
            end
            I.change_menu(manager,target,function() finish(manager) end)
        else finish(manager) end
    end
    local function active(screen)
        local state = I.screens[I.id(screen)]
        if not state then return end
        local address = I.id(screen.RulesSwitcher:GetActiveWidget())
        for _, tab in pairs(state.tabs) do
            if I.id(tab.page) == address then
                return I.initial(tab.rows, true) or I.initial(tab.rows) or tab.button
            end
        end
    end
    R.post(class..':Construct', construct)
    R.post(class..':GetInitialFocusWidget', function(screen, out)
        local widget = active(screen)
        if widget then out:set(widget) end
    end)
    R.post(class..':HandleRefocus', function(screen)
        local widget = active(screen)
        if widget and not widget:HasFocusedDescendants() then I.focus(widget, screen) end
    end)
    R.pre(I.path('B_MenuUIManager', 'ChangeMenu', I.managers), function(manager, target)
        if returning and not returning.family[target] then
            local record = returning
            returning = nil
            -- Native Back owns this transition and its synchronous asset loads.
            -- Keep its return context until the native cleanup has completed.
            if target == record.previous then closing = record; return end
            I.change_menu(manager,record.previous,function()
                if record.subordinate then record.ingame:CloseSubordinateMenu() end
                I.restore_after_close(record.saved)
            end)
            return true
        end
    end)
    R.post(I.path('B_MenuUIManager', 'ChangeMenu', I.managers), function(manager)
        finish(manager)
        if closing and manager.CurrentMenuState == closing.previous then
            local record = closing
            closing = nil
            I.restore_after_close(record.saved)
        end
    end)
    I.destructors[#I.destructors+1]=function(state)
        for _, saved in ipairs(state.menu_slots or {}) do
            saved.slot:SetPadding(saved.padding)
        end
        if state.rule_arrays then
            I.array(state.screen.Buttons,state.rule_arrays.buttons)
            I.array(state.screen.RuleTabs,state.rule_arrays.pages)
        end
    end
    I.resets[#I.resets + 1] = function() if cancelTransition then cancelTransition();cancelTransition=nil end;pending, returning, closing = nil, nil, nil end
end
