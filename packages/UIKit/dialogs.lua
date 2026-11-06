return function(R, C, U, I)
    local umg = '/Script/UMG.Default__WidgetBlueprintLibrary'
    local input = '/Script/PropWitchHuntModule.Default__BpInputSettingsLibrary'
    local showing, dialogs, last = nil, {}, nil
    local cancel_restore
    function I.snapshot(origin)
        local pc
        if origin then pc = origin:GetOwningPlayer()
        else
            for _, controller in ipairs(R.find_all('/Script/Engine.PlayerController')) do
                if controller:IsLocalController() then pc = controller; break end
            end
        end
        local gi = I.object('/Script/Engine.Default__GameplayStatics'):GetGameInstance(pc)
        local mode = I.object(input):GetInputMode(pc)
        local name = I.string(I.object('/Script/PropWitchHuntModule.EInputMode'):GetNameByValue(mode)):match('([^:]+)$')
        local focus = name ~= 'IM_GameOnly' and (origin or last and last.focus) or nil
        if name ~= 'IM_GameOnly' and not R.valid(focus) then
            local ingame = gi.IngameUIManager
            focus = R.valid(ingame) and ingame.TopMostFocusedWidget or origin
            if not R.valid(focus) then
                gi.MenuUIManager.MenuStateWidgets:ForEach(function(k,v)
                    if k:get()==gi.MenuUIManager.CurrentMenuState then focus=v:get() end
                end)
            end
            if not R.valid(focus) then focus = nil end
        end
        local master = gi.MenuUIManager.MasterScreen
        if not R.valid(master) then master = nil end
        return {master = master, pc = pc, gi = gi, mode = name, cursor = pc.bShowMouseCursor, focus = focus,
            lock = last and last.lock or 0, hide = last and last.hide or false}
    end
    function I.restore(saved)
        local library = I.object(umg)
        if saved.mode == 'IM_GameOnly' then library:SetInputMode_GameOnly(saved.pc, false)
        elseif saved.mode == 'IM_GameAndUI' then library:SetInputMode_GameAndUIEx(saved.pc, saved.focus, saved.lock, saved.hide, false)
        else library:SetInputMode_UIOnlyEx(saved.pc, saved.focus, saved.lock, false) end
        saved.pc.bShowMouseCursor = saved.cursor
        if saved.master then saved.master.RefreshInitialFocus = false end
        if saved.focus then saved.focus:SetUserFocus(saved.pc); saved.focus:SetKeyboardFocus() else library:SetFocusToGameViewport() end
    end
    function I.restore_after_close(saved)
        if cancel_restore then cancel_restore() end
        cancel_restore = R.next_frame(function()
            cancel_restore = nil
            if R.valid(saved.pc) then
                if not R.valid(saved.focus) then saved.focus = nil end
                I.restore(saved)
            end
        end)
    end
    for _, mode in ipairs({'GameOnly', 'UIOnlyEx', 'GameAndUIEx'}) do
        R.pre('/Script/UMG.WidgetBlueprintLibrary:SetInputMode_'..mode, function(_, pc, focus, lock, hide)
            if not R.valid(pc) then return end
            if showing and mode ~= 'GameOnly' and R.valid(focus) then
                showing.widget = focus
                dialogs[I.id(focus)] = showing
            end
            last = {focus = mode ~= 'GameOnly' and focus or nil, lock = lock or 0, hide = mode == 'GameAndUIEx' and hide or false}
        end)
    end
    local function show(title, text, answer, origin)
        if cancel_restore then cancel_restore(); cancel_restore = nil end
        local record = {saved = I.snapshot(origin), answer = answer}
        local gi = record.saved.gi
        local previous = showing
        showing = record
        if answer then
            -- Title, question, Ok, Yes, No, Cancel, and the out parameter for the dialog.
            -- OpenScreen adds it hidden; callers make it visible, as the native save screen does.
            local out = {}
            gi.IngameUIManager:ShowQuestionDialog(FText(title), FText(text), false, true, true, false, out)
            local dialog = out.QuestionDialog
            dialog:SetVisibility(0)
            -- In a match OpenScreen changes no input mode, so the answer is bound to the dialog here.
            record.widget = dialog
            dialogs[I.id(dialog)] = record
        else
            gi.MenuUIManager:ShowMessageDialog(title, text)
        end
        showing = previous
    end
    function U.message(title, text) show(title, text) end
    -- origin: the widget that asked; focus returns to it after the answer.
    function U.question(title, text, on_answer, origin) show(title, text, on_answer, origin) end
    local question = '/Game/UserInterface/Information/'
    local function answer(widget, yes)
        local record = dialogs[I.id(widget)]
        if record and record.answer then
            record.result = yes
        end
    end
    for _, entry in ipairs({{'Yes', 1, true}, {'No', 2, false}, {'Cancel', 3, false}, {'Ok', 0, true}}) do
        R.pre(I.path('WB_QuestionDialog', 'BndEvt__'..entry[1]..'_K2Node_ComponentBoundEvent_'..entry[2]..'_Clicked__DelegateSignature', question),
            function(widget) answer(widget, entry[3]) end)
    end
    R.pre(I.path('WB_QuestionDialog', 'OnApply', question), function(widget) answer(widget, true) end)
    R.pre(I.path('WB_QuestionDialog', 'OnBack', question), function(widget) answer(widget, false) end)
    -- Direct ubergraph calls use the same native button entry points.
    R.pre(I.path('WB_QuestionDialog', 'ExecuteUbergraph_WB_QuestionDialog', question), function(widget, entry)
        if entry == 65 or entry == 25 then answer(widget, true)
        elseif entry == 91 or entry == 117 then answer(widget, false) end
    end)
    for _, route in ipairs({{'B_MenuUIManager', 'CloseDialog'}, {'B_IngameUIManager', 'CloseScreen'}}) do
        R.post(I.path(route[1], route[2], I.managers), function(manager, widget)
            if not R.valid(widget) then return end
            local id = I.id(widget)
            local record = dialogs[id]
            if not record then return end
            dialogs[id] = nil
            local underneath = false
            for _, other in pairs(dialogs) do
                if other.saved.focus and I.id(other.saved.focus) == id then
                    other.saved = record.saved
                    underneath = true
                end
            end
            -- Non-Creative CloseScreen completes its own input/focus work.
            -- Native CloseScreen refocuses the in-game menu; a question returns focus to its asker.
            if not underneath and (route[2] == 'CloseDialog' or record.answer or C.session().is_creative or next(dialogs)) then
                I.restore_after_close(record.saved)
            end
            if record.answer then
                local callback, result = record.answer, record.result == true
                R.next_frame(function() callback(result) end)
            end
        end)
    end
    I.resets[#I.resets + 1] = function()
        showing, dialogs, last, cancel_restore = nil, {}, nil, nil
    end
end
