return function(R, C)
    local U, I = {}, {actions = {}, screens = {}, resets = {}, destructors = {}}
    local directory = debug.getinfo(1, 'S').source:match('^@(.+[\\/])')
    I.elements = '/Game/UserInterface/UIElements/'
    I.play = '/Game/UserInterface/Play/'
    I.managers = '/Game/Blueprints/UserInterface/'
    function I.path(asset, event, folder)
        return (folder or I.elements)..asset..'.'..asset..'_C:'..event
    end
    function I.id(widget) return widget:GetAddress() end
    function I.object(path) return StaticFindObject(path) end
    function I.string(value)
        if type(value) == 'string' then return value end
        return value:ToString()
    end
    function I.values(array)
        if not array.ForEach then
            local out={};for i,value in ipairs(array) do out[i]=value:get() end;return out
        end
        local values = {}
        array:ForEach(function(_, value) values[#values + 1] = value:get() end)
        return values
    end
    function I.array(array, values)
        array:Empty()
        for index, value in ipairs(values) do array[index] = value end
    end
    function I.state(screen)
        local id = I.id(screen)
        if not I.screens[id] then I.screens[id] = {screen = screen, actions = {}, menus = {}, tabs = {}} end
        return I.screens[id]
    end
    function I.forget(screen)
        local id = I.id(screen)
        local state = I.screens[id]
        if state then
            for address in pairs(state.actions) do I.actions[address] = nil end
            for _, cleanup in ipairs(I.destructors) do cleanup(state) end
            I.screens[id] = nil
        end
    end
    for _, module in ipairs({'widgets', 'dialogs', 'navigation', 'screens', 'browser'}) do
        dofile(directory..module..'.lua')(R, C, U, I)
    end
    R.post('/Script/UMG.UserWidget:Destruct', function(screen)
        I.forget(screen)
    end)
    R.on('world_end', function()
        -- Menu screens are cached across worlds and keep the widgets added to them, so
        -- their state stays; adding again would duplicate rows. Screens that died with
        -- the world are forgotten without touching their (possibly gone) children.
        for id, state in pairs(I.screens) do
            if not R.valid(state.screen) then
                for address in pairs(state.actions) do I.actions[address] = nil end
                I.screens[id] = nil
            end
        end
        for _, reset in ipairs(I.resets) do reset() end
    end)
    local refusal
    C.on_session(function(session)
        local message=C.mods.message
        if message and message~=refusal and session.gi and session.pc then
            R.next_frame(function() U.message('Modpack mismatch',message) end)
        end
        refusal=message
    end)
    R.native.on_save(function(event)
        if event.complete and event.success==false and C.session().pc then
            R.next_frame(function() U.message('Save failed','The game could not finish saving the layout. Check the saved layout before continuing.') end)
        end
    end)
    return U
end
