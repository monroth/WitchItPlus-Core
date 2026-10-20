-- Publishes factories first; runtime_started installs the central hotkey tab service.
return function(api)
    local engine=api.module('engine')
    local ownership=api.module('ownership')
    local widgets=api.module('widgets')
    local bindings=api.module('bindings')
    local Kit={version='2.2.0'}
    function Kit.new(owner)
        assert(owner.api_version>=2,'UIKit needs runtime API 2')
        local ui=widgets.new(owner,engine,ownership)
        ui.bind=bindings.new(ui,owner,engine)
        return ui
    end
    api.publish(Kit)
    if api.id=='UIKit' then
        local installed=false
        local function start() if not installed then installed=true; api.module('hotkeys').install(api,Kit); api.module('mods').install(api,Kit); api.module('server_browser').install(api,Kit) end end
        api.on('runtime_started',start,{ungated=true})
        if WIR_RUNTIME.started then start() end
    end
end
