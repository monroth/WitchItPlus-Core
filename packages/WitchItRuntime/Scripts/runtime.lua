-- The runtime every package is built on: hooks, world events, timers, settings and
-- native primitives. It knows nothing about any mod.
local dir=debug.getinfo(1,'S').source:match('^@(.+[\\/])')
return function()
    local R={json=dofile(dir..'json.lua'),base64=dofile(dir..'base64.lua')}
    local gameplay=StaticFindObject('/Script/Engine.Default__GameplayStatics')

    -- UE4SS passes hook parameters wrapped; callbacks receive plain values.
    local function unwrap(x)
        if type(x)=='userdata' and type(x.type)=='function' then
            local kind=x:type()
            if kind=='RemoteUnrealParam' or kind=='LocalUnrealParam' then return x:get() end
        end
        return x
    end
    function R.valid(x)
        x=unwrap(x)
        if x~=nil and x:IsValid() then return x end
    end
    function R.log(pkg,fmt,...) print('['..pkg..'] '..string.format(fmt,...)..'\n') end

    -- Events --------------------------------------------------------------------
    -- world_begin(world), world_end, local_player(pc), match_start(gs), round_start(gs),
    -- match_end(gs), player_joined(ps), player_left(ps), layout_saved(info), layout_loaded(info),
    -- failure(why): the first mod failure of this run.
    local events={}
    local emit
    local announced=false
    local logged={}
    -- Every mod callback runs here: a failure is logged with its trace and the callbacks
    -- after it still run.
    function R.call(fn,...)
        local ok,value=xpcall(fn,debug.traceback,...)
        if ok then return value end
        -- A failure in a frequent hook repeats unchanged; one line tells it.
        if not logged[value] then logged[value]=true;print('[Error] '..tostring(value)..'\n') end
        if not announced then announced=true;emit('failure',value) end
    end
    function R.on(event,fn)
        events[event]=events[event] or {}
        table.insert(events[event],fn)
    end
    emit=function(event,...)
        for _,fn in ipairs(events[event] or {}) do R.call(fn,...) end
    end

    -- Timers --------------------------------------------------------------------
    -- Each returns a cancel function. Timers end with their world unless they survive it.
    local pending={} -- [handle] = true to cancel at world end, false to survive
    local function schedule(enqueue,delay,fn,survive)
        local handle
        handle=enqueue(delay,function()
            if pending[handle]==nil then return end
            pending[handle]=nil
            R.call(fn)
        end)
        pending[handle]=not survive
        return function()
            if pending[handle]~=nil then
                pending[handle]=nil
                CancelDelayedAction(handle)
            end
        end
    end
    function R.next_frame(fn) return schedule(ExecuteInGameThreadAfterFrames,1,fn) end
    function R.after(ms,fn,survive) return schedule(ExecuteInGameThreadWithDelay,ms,fn,survive) end

    -- Hooks ---------------------------------------------------------------------
    -- R.pre(path,fn) runs before a function; returning true skips the original.
    -- R.post(path,fn) runs after it; a returned value replaces the result.
    -- A hook installs once the function's class exists. Blueprint functions get their
    -- PRE hook from the native intercept, native functions from RegisterHook.
    local hooks={}
    local function dispatch(h,phase,...)
        local args=table.pack(...)
        local raw=table.pack(...)
        -- Output parameters stay wrapped so callbacks can :set() them.
        local shift=phase=='post' and h.native and h.returns and 1 or 0
        for i=1,args.n do
            if i<=1+shift or not h.outputs[i-1-shift] then args[i]=unwrap(args[i]) end
        end
        if not R.valid(args[1]) then return end
        raw[1]=args[1]
        local result
        for _,callback in ipairs(h[phase]) do
            local input=callback.raw and raw or args
            local value=R.call(callback.fn,table.unpack(input,1,input.n))
            if phase=='pre' then result=result or value==true
            elseif value~=nil then result=value end
        end
        return result
    end
    local function isNative(path) return StaticFindObject(path):GetFunctionFlags() & 0x400~=0 end
    local function intercept(h)
        R.native.intercept(h.path,function(...) return dispatch(h,'pre',...) end)
        h.intercepted=true
    end
    local function install(h)
        if h.installed then return end
        if not R.valid(StaticFindObject(h.path)) then return end
        h.installed=true
        local schema=WitchItNativeFunctionSchema(StaticFindObject(h.class):GetCDO(),h.name)
        h.outputs={}
        h.returns=schema.return_name~=nil
        for i=1,schema.count do h.outputs[i]=schema['out:'..i] end
        h.native=isNative(h.path)
        if h.native then
            RegisterHook(h.path,function(...) dispatch(h,'pre',...) end,function(...) return dispatch(h,'post',...) end)
        else
            if #h.pre>0 then intercept(h) end
            RegisterHook(h.path,function(...) return dispatch(h,'post',...) end)
        end
    end
    local function hook(path,phase,fn,raw)
        local h=hooks[path]
        if not h then
            local class,name=path:match('^(.*):([^:]+)$')
            h={path=path,class=class,name=name,pre={},post={}}
            hooks[path]=h
            NotifyOnNewObject(class,function() install(h) end)
        end
        table.insert(h[phase],{fn=fn,raw=raw})
        -- A Blueprint function installed for POST alone gets its intercept on the first PRE.
        if h.installed and phase=='pre' and not h.intercepted and not isNative(path) then intercept(h) end
        install(h)
    end
    function R.post(path,fn) hook(path,'post',fn) end
    -- Raw PRE inputs keep the parameter wrappers so a callback can rewrite them.
    -- Context is still unwrapped; ordinary callbacks retain their value arguments.
    function R.pre(path,fn,raw) hook(path,'pre',fn,raw) end

    -- Objects of the current world ----------------------------------------------
    local world,pc,worldAddress,pcAddress
    function R.find_all(path)
        local class=R.valid(StaticFindObject(path))
        if not class or not world then return {} end
        local out={}
        if class:IsChildOf(StaticFindObject('/Script/Engine.Actor')) then
            gameplay:GetAllActorsOfClass(world,class,out)
        elseif class:IsChildOf(StaticFindObject('/Script/UMG.UserWidget')) then
            StaticFindObject('/Script/UMG.Default__WidgetBlueprintLibrary'):GetAllWidgetsOfClass(world,out,class,false)
        elseif class:IsChildOf(StaticFindObject('/Script/Engine.GameInstance')) then
            out[1]=gameplay:GetGameInstance(world)
        else
            for _,object in ipairs(FindAllOf(path:match('%.([^%.]+)$')) or {}) do
                if R.valid(object) and object:IsA(class) and object:GetWorld():GetAddress()==worldAddress then
                    out[#out+1]=object
                end
            end
        end
        for i,x in ipairs(out) do out[i]=unwrap(x) end
        return out
    end
    function R.find(path) return R.find_all(path)[1] end

    -- Files and settings ----------------------------------------------------------
    -- Every package's files live under Saved/WitchItRuntime/<package>/.
    local storage=dofile(dir..'files.lua')()
    local saved=StaticFindObject('/Script/Engine.Default__KismetSystemLibrary'):GetProjectSavedDirectory()
    saved=type(saved)=='string' and saved or saved:ToString()
    local root=saved:gsub('[/\\]+$','')..'/WitchItRuntime/'
    R.saved_root=root
    R.native=dofile(dir..'native.lua')(R)
    R.native.file_read=storage.read
    R.native.file_write=storage.write
    R.native.file_list=storage.list
    R.native.file_remove=storage.remove
    local function clone(t)
        if type(t)~='table' then return t end
        local out={}
        for k,v in pairs(t) do out[k]=clone(v) end
        return setmetatable(out,getmetatable(t))
    end
    local settings={}
    -- A package's settings: its saved values over the given defaults, loaded once.
    function R.settings(pkg,defaults)
        if not settings[pkg] then
            local value=clone(defaults)
            local bytes=storage.read(root..pkg..'/settings.json')
            if bytes then
                for k,v in pairs(R.json.decode(bytes)) do value[k]=v end
            end
            settings[pkg]=value
        end
        return settings[pkg]
    end
    function R.save_settings(pkg) assert(storage.write(root..pkg..'/settings.json',R.json.encode(settings[pkg]))) end
    function R.prepare_directory(rel) assert(storage.mkdir(root..rel)) end
    R.prepare_directory('WitchItRuntime')
    dofile(dir..'keys.lua')(R,function() return pc end)

    -- World lifetime --------------------------------------------------------------
    -- A world begins with its first sign (map load, game state, local controller) and
    -- ends on travel or when the local controller leaves it.
    local function end_world()
        if not world then return end
        world,pc,worldAddress,pcAddress=nil,nil,nil,nil
        local old=pending
        pending={}
        for handle,cancel in pairs(old) do
            if cancel then CancelDelayedAction(handle) else pending[handle]=false end
        end
        emit('world_end')
    end
    local function begin_world(w)
        if worldAddress==w:GetAddress() then return end
        end_world()
        world,worldAddress=w,w:GetAddress()
        emit('world_begin',w)
    end
    local function local_player(actor)
        begin_world(actor:GetWorld())
        pc,pcAddress=actor,actor:GetAddress()
        emit('local_player',actor)
    end
    local function observe_pc(actor)
        if R.valid(actor) and actor:IsA(StaticFindObject('/Script/Engine.PlayerController'))
            and actor:IsLocalPlayerController()
            and (worldAddress~=actor:GetWorld():GetAddress() or pcAddress~=actor:GetAddress()) then
            local_player(actor)
        end
    end
    RegisterLoadMapPreHook(end_world)
    RegisterLoadMapPostHook(function(_,w)
        w=R.valid(w)
        if w then begin_world(w) end
    end)
    RegisterInitGameStatePostHook(function(context)
        context=R.valid(context)
        if context then begin_world(context:GetWorld()) end
    end)
    RegisterBeginPlayPostHook(function(actor) observe_pc(unwrap(actor)) end)
    RegisterEndPlayPreHook(function(actor,reason)
        actor,reason=unwrap(actor),unwrap(reason)
        -- 1 = level transition, 4 = quit.
        if reason==1 or reason==4 or actor:GetAddress()==pcAddress then end_world() end
    end)
    -- A guest's controller may begin play before Lua sees it; these native calls reach it.
    R.pre('/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:ClientSetRevisionNumber',observe_pc)
    for _,method in ipairs({'Initialize','ShowHUD','ShowTeamSelectionObjective'}) do
        R.post('/Game/Blueprints/UserInterface/B_IngameUIManager.B_IngameUIManager_C:'..method,function(ui)
            observe_pc(gameplay:GetPlayerController(ui,0))
        end)
    end

    -- Match events ----------------------------------------------------------------
    local base='/Game/Blueprints/Core/'
    -- The skill countdown bypasses StartGame/OnGameStartedMulti. The reliable
    -- objective multicast marks every half on both countdown and Ready paths.
    R.post('/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectiveGameState.B_ObjectiveGameState_C:OnMatchStartedMulti',function(gs)
        emit('match_start',gs)
        emit('round_start',gs)
    end)
    R.post(base..'B_GameState.B_GameState_C:OnMatchEndedMulti',function(gs) emit('match_end',gs) end)
    -- The objective game mode overrides the base login functions and calls them; the
    -- event fires once, when the outermost call returns.
    for name,event in pairs({K2_PostLogin='player_joined',K2_OnLogout='player_left'}) do
        local depth=0
        for _,class in ipairs({base..'B_GameMode.B_GameMode_C:',
            '/Game/Blueprints/Gamemodes/ObjectiveMode/B_ObjectiveGameMode.B_ObjectiveGameMode_C:'}) do
            R.pre(class..name,function() depth=depth+1 end)
            R.post(class..name,function(_,controller)
                depth=depth-1
                if depth==0 and R.valid(controller) then emit(event,controller.PlayerState) end
            end)
        end
    end

    -- Layout save and load --------------------------------------------------------
    local function layout_info(id)
        local gs=world and R.valid(gameplay:GetGameState(world))
        local creative=gs~=nil and gs:GetClass():GetFName():ToString()=='B_CreativeGameState_C'
        return {world=world,layout_id=id,is_creative=creative,is_playtest=creative and gs.IsPlayTestMode==true,
            host=world~=nil and R.valid(gameplay:GetGameMode(world))~=nil}
    end
    R.native.save_load(function(phase,serial,owner,requested,compressed,autosave,target,success,_,name)
        if phase=='complete' and success then
            local info=layout_info(target)
            info.name,info.serial,info.autosave=name,serial,autosave
            emit('layout_saved',info)
        end
        return true
    end,function(phase,serial,owner,target,compressed,recovery,intent,reader_done,reader_success,applied,name,w,path)
        if phase=='return' and reader_done and reader_success and applied then
            local info=layout_info(target)
            info.name,info.serial,info.recovery,info.map_name=name,serial,recovery,w
            emit('layout_loaded',info)
        end
        return true
    end)

    -- A runtime loaded into a running world finds its local controller once.
    R.next_frame(function()
        for _,actor in ipairs(FindAllOf('PlayerController') or {}) do
            if R.valid(actor) and actor:IsLocalPlayerController() then local_player(actor);break end
        end
    end)
    return R
end
