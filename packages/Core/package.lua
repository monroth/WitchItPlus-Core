return function(api)
    local Core={version=api.version,json=api.json}
    for _,name in ipairs({'lifetime','match_identity','events','event_clock','work_budget','registry','runtime','guarded_native','teleporter_zone','tick','settings','storage','ufunction'}) do
        Core[name]=api.module(name)
    end
    Core.capabilities=api.module('capabilities').new()
    Core.companion_io=api.module('companion_io')
    Core.geometry_files=api.module('geometry_files')
    Core.layout_files=api.module('layout_files').new(api.log)
    Core.actor_edit_claims=api.module('actor_edit_claims').new(function(scope,path)
        local p=Core.capabilities.get('layout_hiding')or Core.capabilities.get('fapp_geometry')
        if not p then return false end
        if type(p.actor_claimed)~='function'then return true end -- old provider cannot prove exclusivity
        return p.actor_claimed(scope,path)==true
    end)
    function Core.new(owner,options)
        options=options or {}
        assert(owner.api_version>=2,'Core needs runtime API 2')
        local L=Core.lifetime.new(owner)
        local C={lifetime=L,event_clock=Core.event_clock.new(),storage=Core.storage.new(owner)}
        C.match_identity=Core.match_identity.new(L); C.events=Core.events.new(L)
        C.runtime=Core.runtime.new(owner,L)
        C.work_budget=Core.work_budget.new(owner,options.budget)
        L.onReset(C.work_budget.reset)
        if options.install_lifetime~=false then L.install(options.native_travel_hooks) end
        return C
    end
    Core.modsync=api.module('modsync')
    function Core.possession_lifetime(owner)
        local value=owner.context().possession_lifetime
        return value==nil and 5 or value
    end
    if api.id=='Core' then
        Core.dialog_input=api.module('dialog_input').install(api,Core)
        -- Dependents mount the inert service during package construction. The
        -- committed-own-inventory/hook/input gates still prohibit READY here.
        local DocumentOwner=api.module('map_document_owner')
        local owner=DocumentOwner.new(api,Core)
        local nativeSaveLoad=api.module('native_integration').install(api,Core,owner)
        Core.write_status=nativeSaveLoad.outcome -- detached scalar status only
        Core.modpack=api.module('modpack').new(api,Core)
        Core.package_sync=Core.modsync.new(api,Core,owner)
        Core.source_geometry=api.module('source_geometry').install(api,Core)
    end
    api.publish(Core)
end
