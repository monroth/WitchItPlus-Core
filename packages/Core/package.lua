local dir=debug.getinfo(1,'S').source:match('^@(.+[\\/])')
return function(R)
    local C={}
    local gp=StaticFindObject('/Script/Engine.Default__GameplayStatics')
    local current={role='offline',mode='none',is_creative=false,is_playtest=false,is_vanilla_session=true}
    local listeners={}
    local vanilla=true
    -- Fill a Pot+ is chosen on a MatchOptions object; copies of those options keep the choice.
    local plus={}
    local hostMode,publishedMode
    function C.session() return current end
    function C.on_session(fn) listeners[#listeners+1]=fn;fn(current) end
    function C.set_plus(options,value) plus[options:GetAddress()]=value or nil end
    function C.is_plus(options) return plus[options:GetAddress()]==true end
    local function className(object) return object:GetClass():GetFName():ToString() end
    local modeOf=dofile(dir..'mode.lua')
    local function refresh(world,pc)
        local s={world=world,pc=pc,is_vanilla_session=vanilla}
        if world then
            s.gm=R.valid(gp:GetGameMode(world));s.gs=R.valid(gp:GetGameState(world))
            s.gi=R.valid(gp:GetGameInstance(world))
        end
        s.role='offline'
        if s.gi and s.gi.IsCurrentlyInMatch then s.role=s.gm and 'host' or s.pc and 'client' or 'offline' end
        s.is_creative=s.gs~=nil and className(s.gs)=='B_CreativeGameState_C'
        s.is_playtest=s.is_creative and s.gs.IsPlayTestMode==true
        if s.gi then
            local options=R.valid(s.gi.IsCurrentlyInMatch and s.gi.UsedMatchOptions or s.gi.NextMatchOptions)
            local mode=options and R.valid(options.GameMode)
            if mode then s.mode_id=mode:GetFName():ToString() end
        end
        s.mode=modeOf(s,C.is_plus,hostMode)
        current=s
        if s.role=='host' and s.mode~=publishedMode then publishedMode=s.mode;C.net.publish('core.mode',s.mode) end
        for _,fn in ipairs(listeners) do fn(current) end
    end
    -- Hooks registered through a scope run only while applies(session) is true.
    function C.scoped(applies)
        local function gate(fn)
            return function(...) if applies(current) then return fn(...) end end
        end
        return {
            pre=function(path,fn,raw) R.pre(path,gate(fn),raw) end,
            post=function(path,fn) R.post(path,gate(fn)) end,
            on=function(event,fn) R.on(event,gate(fn)) end,
            session=function(fn) C.on_session(gate(fn)) end,
        }
    end
    C.files=dofile(dir..'files.lua')(R)
    C.report=dofile(dir..'report.lua')(R)
    function C.save_report() return C.report.save(dir..'../') end
    C.saved_rules=dofile(dir..'rules.lua')(R,C)
    local net,transport=dofile(dir..'net.lua')(R,C)
    C.net=net
    C.net.watch('core.mode',function(value)
        if current.role=='host' then return end
        hostMode=value;refresh(current.world,current.pc)
    end)
    R.on('world_begin',function(world) refresh(world,nil) end)
    R.on('local_player',function(pc) refresh(pc:GetWorld(),pc) end)
    R.on('world_end',function()
        R.log('Core','World end role=%s mode=%s',current.role,current.mode)
        hostMode,publishedMode=nil,nil;refresh(nil,nil)
    end)
    R.post('/Game/Blueprints/Core/B_GameMode.B_GameMode_C:Initialize',function(gm) refresh(gm:GetWorld(),current.pc) end)
    R.post('/Game/Blueprints/Gamemodes/CreativeMode/B_CreativeGameState.B_CreativeGameState_C:TogglePlayTestMultiCast',function(gs)
        refresh(gs:GetWorld(),current.pc)
    end)
    R.post('/Game/Blueprints/Match/B_MatchOptions.B_MatchOptions_C:CopyMatchOptions',function(options,source)
        if not R.valid(source) then return end
        C.set_plus(options,C.is_plus(source))
        refresh(current.world,current.pc)
    end)
    for _,method in ipairs({'Initialize','ShowHUD','ShowTeamSelectionObjective'})do
        R.post('/Game/Blueprints/UserInterface/B_IngameUIManager.B_IngameUIManager_C:'..method,function(ui)
            local pc=gp:GetPlayerController(ui,0)
            refresh(pc:GetWorld(),pc)
        end)
    end
    -- The game's own connection failures, with their engine type, for both host and guest.
    -- The game's Blueprint overrides them, so the hooks go on its class.
    for _,event in ipairs({'HandleNetworkError','HandleTravelError'}) do
        R.post('/Game/Blueprints/Core/B_GameInstance.B_GameInstance_C:'..event,function(_,kind,server)
            R.log('Core','%s type=%s server=%s',event,tostring(kind),tostring(server))
        end)
    end
    -- Who ended a connection, on either side: the kick and return-to-menu calls.
    local function shown(value)
        local ok,text=pcall(function() return value:ToString() end)
        return ok and text or tostring(value)
    end
    for _,path in ipairs({'/Game/Blueprints/UserInterface/B_MenuUIManager.B_MenuUIManager_C:ShowMessageDialog',
        '/Script/Engine.PlayerController:ClientWasKicked',
        '/Script/Engine.PlayerController:ClientReturnToMainMenuWithTextReason',
        '/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:ClientSetWasKicked',
        '/Script/PropWitchHuntModule.ProjectVersionBlueprint:KickPlayer'}) do
        R.post(path,function(_,...)
            local parts={}
            for i=1,select('#',...) do parts[i]=shown((select(i,...))) end
            R.log('Core','%s %s',path:match('([^:]+)$'),table.concat(parts,' '))
        end)
    end
    C.mods=dofile(dir..'mods.lua')(R,C,transport,function(value)
        vanilla=value;refresh(current.world,current.pc)
    end)
    return C
end
