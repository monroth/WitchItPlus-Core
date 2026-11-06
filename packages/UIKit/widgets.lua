return function(R, C, U, I)
    local library = '/Script/UMG.Default__WidgetBlueprintLibrary'
    local slider = 'WB_SliderWithEditableLabel'
    local owners, children, parents = {}, {}, {}
    -- After world_end, keep ownership only for widgets of screens that are still known.
    I.resets[#I.resets + 1] = function()
        local kept = {}
        for _, state in pairs(I.screens) do kept[state] = true end
        for id, state in pairs(owners) do
            if not kept[state] then owners[id], children[id], parents[id] = nil, nil, nil end
        end
        for parent, list in pairs(children) do
            for id in pairs(list) do if not owners[id] then list[id] = nil end end
            if next(list) == nil then children[parent] = nil end
        end
    end
    function I.owner(parent)
        local state = owners[I.id(parent)]
        if state then return state end
        local node = parent
        while not node:IsA(I.object('/Script/UMG.UserWidget')) do node = node:GetOuter() end
        return I.state(node)
    end
    function I.attach(parent, widget)
        parent:AddChild(widget)
        return I.own(parent, widget)
    end
    function I.own(parent, widget)
        local id, state = I.id(widget), I.owner(parent)
        owners[id] = state
        state.widgets = state.widgets or {}
        state.widgets[id] = true
        local parent_id = I.id(parent)
        children[parent_id] = children[parent_id] or {}
        children[parent_id][id] = widget
        parents[id] = parent_id
        return widget
    end
    function I.remove(widget)
        if not R.valid(widget) then return end
        local id = I.id(widget)
        for _, child in pairs(children[id] or {}) do I.remove(child) end
        local state = owners[id]
        if state then state.actions[id], state.widgets[id] = nil, nil end
        if parents[id] and children[parents[id]] then children[parents[id]][id] = nil end
        I.actions[id], owners[id], children[id], parents[id] = nil, nil, nil, nil
        widget:RemoveFromParent()
    end
    I.destructors[#I.destructors + 1] = function(state)
        local roots={}
        for id in pairs(state.widgets or {}) do
            if not state.widgets[parents[id]] then roots[#roots+1]=children[parents[id]][id] end
        end
        for _,widget in ipairs(roots) do I.remove(widget) end
        for id in pairs(state.widgets or {}) do
            if parents[id] and children[parents[id]] then children[parents[id]][id] = nil end
            owners[id], children[id], parents[id] = nil, nil, nil
        end
    end
    function I.initial(parent, focused)
        local id = I.id(parent)
        if I.actions[id] and (not focused or parent:HasKeyboardFocus() or parent:HasFocusedDescendants()) then return parent,I.actions[id].initial end
        local first
        for _, child in pairs(children[id] or {}) do
            local found,preferred = I.initial(child, focused)
            if preferred then return found,true end
            first=first or found
        end
        return first
    end
    function I.native(parent, kind, name)
        local state = I.owner(parent)
        return I.attach(parent, StaticConstructObject(I.object('/Script/UMG.'..kind), state.screen.WidgetTree, name and FName(name)))
    end
    function I.cooked(parent, asset, class)
        local state = I.owner(parent)
        class = class or R.native.load_asset(I.path(asset, ''):sub(1, -2))
        local widget = I.object(library):Create(state.screen, class, state.screen:GetOwningPlayer())
        return I.attach(parent, widget)
    end
    function I.action(widget, record)
        local id, state = I.id(widget), I.owner(widget)
        record.widget = widget
        widget.bIsFocusable = true
        I.actions[id], state.actions[id] = record, true
        return record
    end
    function I.slot_values(slot)
        return {padding={Left=slot.Padding.Left,Top=slot.Padding.Top,Right=slot.Padding.Right,Bottom=slot.Padding.Bottom},
            size={Value=slot.Size.Value,SizeRule=slot.Size.SizeRule}, horizontal=slot.HorizontalAlignment, vertical=slot.VerticalAlignment}
    end
    function I.slot_apply(slot, values)
        slot:SetPadding(values.padding); slot:SetSize(values.size)
        slot:SetHorizontalAlignment(values.horizontal); slot:SetVerticalAlignment(values.vertical)
    end
    function I.focus(widget, screen)
        widget:SetUserFocus(screen:GetOwningPlayer())
        widget:SetKeyboardFocus()
    end
    local function label(widget, text, field)
        local value = FText(text or '')
        if field == 'Label' then widget.LabelText = value else widget.DescriptiveLabel = value end
        widget[field or 'DescriptiveLabelText']:SetText(value)
    end
    local function options(widget, opts)
        if opts.tooltip then widget:SetToolTipText(FText(opts.tooltip)) end
        if opts.enabled ~= nil then widget:SetIsEnabled(opts.enabled) end
    end
    function U.text(parent, opts)
        local template = I.cooked(parent, 'WB_CheckBoxWithLabel')
        template:SetVisibility(1)
        -- A SizeBox accepts one child. Copy the attached cooked style before
        -- replacing its template, then attach the finished text to the empty slot.
        local widget = StaticConstructObject(I.object('/Script/UMG.TextBlock'), I.owner(parent).screen.WidgetTree)
        widget:SetFont(template.DescriptiveLabelText.Font)
        widget:SetColorAndOpacity(template.DescriptiveLabelText.ColorAndOpacity)
        I.remove(template)
        I.attach(parent, widget)
        widget.WrapTextAt = opts.wrap == false and 0 or opts.width or 1000
        widget:SetAutoWrapText(opts.wrap ~= false)
        widget:SetJustification(0)
        local function set(value) widget:SetText(FText(value)) end
        set(opts.text or opts.label or '')
        options(widget, opts)
        return widget, set
    end
    function U.input(parent, opts)
        local widget = I.native(parent, 'EditableTextBox')
        widget:SetHintText(FText(opts.hint or ''))
        local function set(value) widget:SetText(FText(value)) end
        set(opts.value or '')
        options(widget, opts)
        local id,state=I.id(widget),I.owner(widget)
        I.actions[id]={widget=widget,initial=opts.initial};state.actions[id]=true
        return widget, set
    end
    function U.row(parent, widths)
        local row = I.native(parent, 'HorizontalBox')
        local cells = {}
        for index, width in ipairs(widths) do
            cells[index] = I.native(row, 'SizeBox')
            cells[index]:SetWidthOverride(width)
            cells[index]:SetClipping(1)
            cells[index].Slot:SetHorizontalAlignment(0);cells[index].Slot:SetVerticalAlignment(1)
        end
        return cells, row
    end
    function U.navigate(rows, scroll)
        for index, widget in ipairs(rows) do
            local record = I.actions[I.id(widget)]
            if record then record.scroll = scroll end
            for _, direction in ipairs({{2, rows[index-1]}, {3, rows[index+1]}}) do
                if direction[2] then
                    widget:SetNavigationRuleExplicit(direction[1], direction[2])
                    for _,key in ipairs({'FocusableWidget','CheckBox','ValueSlider','ButtonRef'}) do
                        local inner=widget[key]
                        if R.valid(inner) then inner:SetNavigationRuleExplicit(direction[1],direction[2]);break end
                    end
                end
            end
        end
    end
    function U.section(parent, opts)
        opts = opts or {}
        local widget = I.native(parent, 'VerticalBox')
        if opts.label then U.text(widget, {text = opts.label}) end
        return widget, function(visible) widget:SetVisibility(visible and 0 or 1) end
    end
    function U.scroll(parent, opts)
        opts = opts or {}
        if opts.height then
            parent = I.native(parent, 'SizeBox')
            parent:SetHeightOverride(opts.height)
        end
        local widget = I.native(parent, 'ScrollBox')
        widget:SetAlwaysShowScrollbar(true)
        return widget, function(target)
            if target then widget:ScrollWidgetIntoView(target, true, 0, 0) else widget:ScrollToStart() end
        end
    end
    function I.caption(widget, value)
        label(widget, value, 'Label')
        if I.string(widget:GetClass():GetFName()) == 'WB_Button_C' then widget.Text = FText(value) end
    end
    function U.button(parent, opts)
        local widget = I.cooked(parent, 'WB_TabButton', opts.class)
        local function set(value) I.caption(widget, value) end
        set(opts.label)
        widget.Label:SetAutoWrapText(opts.wrap == true)
        options(widget, opts)
        I.action(widget, {click = opts.on_click})
        return widget, set
    end
    function U.checkbox(parent, opts)
        local widget = I.cooked(parent, opts.editable==false and 'WB_CheckBoxWithLabel' or 'WB_CheckBoxWithEditableLabel')
        label(widget, opts.label)
        widget.DefaultBoolValue = opts.default == nil and opts.value or opts.default
        options(widget, opts)
        local record = I.action(widget, {value = opts.value, change = opts.on_change})
        local function set(value)
            record.value = value
            widget:SetIsCheckedWithoutEvent(value)
        end
        set(opts.value)
        record.check = function()
            local value = widget.CheckBox:IsChecked()
            if value ~= record.value then
                record.value = value
                if record.change then record.change(value) end
            end
        end
        return widget, set
    end
    function U.slider(parent, opts)
        local lo, hi, step = opts.min, opts.max, opts.step or (opts.kind == 'int' and 1 or .1)
        local widget = I.cooked(parent, slider)
        label(widget, opts.label)
        widget.MinimumValue, widget.MaximumValue, widget.StepSize = lo, hi, step
        widget.bRoundToIntegers, widget.UIMultiplyFactor = opts.kind == 'int', 1
        widget.bTimeValueInSeconds, widget.ValueSuffix = false, FText(opts.suffix or '')
        local default = opts.default == nil and opts.value or opts.default
        widget.DefaultFloatValue = default == 'infinite' and hi or default
        widget.DefaultValueIndicator:SetValue((widget.DefaultFloatValue - lo) / (hi - lo))
        options(widget, opts)
        local record = I.action(widget, {value = opts.value})
        -- The slider covers the usual range; the field takes anything from input_min to input_max.
        local function shown()
            if record.value == 'infinite' then return '∞' end
            return (string.format('%.2f', record.value):gsub('%.?0+$', ''))
        end
        local function paint()
            record.painting = true
            widget:SetRealValue(record.value == 'infinite' and hi or math.max(lo, math.min(hi, record.value)))
            widget:CheckDefaultToUpdateUI()
            widget.InputField:SetText(FText(shown()))
            record.painting = false
        end
        local function parse(value, dragging)
            if opts.infinite and (value == 'infinite' or tostring(value):lower():match('^%s*inf%s*$') or value == '∞') then return 'infinite' end
            local number = tonumber(value)
            if not number or number ~= number or math.abs(number) == math.huge then return end
            if dragging then
                number = math.max(lo, math.min(hi, lo + math.floor((number - lo) / step + .5) * step))
                if opts.infinite and number >= hi then return 'infinite' end
            end
            if number < (opts.input_min or lo) or number > (opts.input_max or hi) then return end
            if opts.kind == 'int' and number % 1 ~= 0 then return end
            return number
        end
        local function set(value) record.value = value; paint() end
        record.edit = function(value, dragging)
            local parsed = parse(value, dragging)
            if parsed ~= nil and parsed ~= record.value then
                record.value = parsed
                if opts.on_change then opts.on_change(parsed) end
                if opts.on_commit then opts.on_commit(parsed) end
            end
            paint()
        end
        record.slide = function(value) record.edit(lo + (hi - lo) * value, true) end
        record.drag = function() record.edit(widget.RealValue, true) end
        record.commit = function(text) record.edit(I.string(text), false) end
        record.reset = function() record.edit(default, false) end
        record.repaint = function()
            if not widget.InputField:HasFocusedDescendants() then
                widget.InputField:SetText(FText(shown()))
            end
        end
        paint()
        return widget, set
    end
    U.number = U.slider
    function U.combo(parent, opts)
        local widget = I.cooked(parent, 'WB_ComboBoxWithLabel')
        local texts, indices = {}, {}
        for index, option in ipairs(opts.options) do
            texts[index], indices[option.value] = FText(option.label), index - 1
        end
        I.array(widget.Options, texts)
        label(widget, opts.label)
        options(widget, opts)
        local record = I.action(widget, {value = opts.value})
        local function set(value)
            record.value = value
            widget:SetOptionIndex(indices[value])
        end
        record.select = function()
            local option = opts.options[widget.SelectedOptionIndex + 1]
            if option.value ~= record.value then
                record.value = option.value
                if opts.on_change then opts.on_change(option.value) end
            end
        end
        set(opts.value)
        return widget, set
    end
    R.post(I.path('WB_BaseControl', 'OnFocusReceived'), function(widget)
        local record = I.actions[I.id(widget)]
        if not record then return end
        if record.scroll then record.scroll:ScrollWidgetIntoView(widget, true, 0, 0) end
    end)
    function U.on_close(parent, fn)
        local state=I.owner(parent)
        state.on_close=state.on_close or {}
        state.on_close[#state.on_close+1]=fn
    end
    I.destructors[#I.destructors+1]=function(state)
        for _,fn in ipairs(state.on_close or {}) do fn() end
    end
    U.remove=I.remove
    local function hook(asset, event, action, resolve)
        R.post(I.path(asset, event), function(widget, ...)
            if resolve then widget = resolve(widget) end
            local record = I.actions[I.id(widget)]
            if record and not record.painting and record[action] then record[action](...) end
        end)
    end
    -- A click hands over this event's widget: wrappers kept from earlier expire.
    R.post(I.path('WB_BaseControl', 'ButtonClicked'), function(widget)
        local record = I.actions[I.id(widget)]
        if record and record.click then record.click(widget) end
    end)
    hook('WB_CheckBoxWithLabel', 'SetIsChecked', 'check')
    hook('WB_CheckBoxWithLabel', 'BndEvt__CheckBox_K2Node_ComponentBoundEvent_0_OnCheckBoxComponentStateChanged__DelegateSignature', 'check')
    hook('WB_CheckBoxWithEditableLabel','SetIsChecked','check')
    hook('WB_CheckBoxWithEditableLabel','BndEvt__CheckBox_K2Node_ComponentBoundEvent_0_OnCheckBoxComponentStateChanged__DelegateSignature','check')
    hook('WB_ComboBoxOverlay', 'SetOption', 'select', function(overlay) return overlay.BaseWidget end)
    hook(slider, 'BndEvt__ValueSlider_K2Node_ComponentBoundEvent_0_OnFloatValueChangedEvent__DelegateSignature', 'slide')
    hook(slider, 'ChangeRealValue', 'drag')
    hook(slider, 'CheckDefaultToUpdateUI', 'repaint')
    hook(slider, 'BndEvt__WB_SliderWithEditableLabel_InputField_K2Node_ComponentBoundEvent_3_OnEditableTextBoxCommittedEvent__DelegateSignature', 'commit')
    hook(slider, 'BndEvt__WB_SliderWithEditableLabel_ResetButton_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature', 'reset')
end
