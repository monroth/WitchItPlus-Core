-- World lifetime guard shared by this mod's dofile-loaded modules.
-- IsValid alone includes objects retained by GC after EndPlay/DestroyActor.
local Value=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'runtime.lua')
local M={}
function M.new(api,options)
options=options or {}
local L={transition=false,epoch=0,listeners={},retired={},departed={},onRetire={},retirement_revision=0}
local call=Value.call
-- Scalar ownership is admitted while the native owner is healthy. Retirement
-- must never inspect a dying actor's world/owner chain or local-player flag.
local boundaryState,boundaryController
local continuingController
local beginRevision=0
local function nativeKey(key)
    if type(key)~='string' then return end
    local address,name=key:match('^([^|]+)|(.+)$')
    local number=address and tonumber(address)
    if number and number>0 then return address..':'..name end
end
local function runtimeState()
    local runtime=WIR_RUNTIME
    if type(runtime)=='table' then
        return nativeKey(runtime.state_key),nativeKey(runtime.world_key)
    end
end
local function runtimeController()
    local runtime=WIR_RUNTIME
    local record=type(runtime)=='table' and runtime.input_owner
    if type(record)=='table' and runtime.alive==true and not runtime.loading and not runtime.state_retired
        and record.generation==runtime.world_generation then
        return nativeKey(record.key),nativeKey(record.world)
    end
end
function L.rawValid(o)
    if not Value.isObject(o) or call(o,'IsValid')~=true then return false end
    if type(EInternalObjectFlags)=='table' then
        for _,name in ipairs({'PendingKill','Unreachable','Garbage'}) do
            local flag=EInternalObjectFlags[name]
            if flag and call(o,'HasAnyInternalFlags',flag)==true then return false end
        end
    end
    if type(EObjectFlags)=='table' then
        for _,name in ipairs({'RF_BeginDestroyed','RF_FinishDestroyed','BeginDestroyed','FinishDestroyed'}) do
            local flag=EObjectFlags[name]
            if flag and call(o,'HasAnyFlags',flag)==true then return false end
        end
    end
    return call(o,'IsA','/Script/Engine.Actor')~=true or call(o,'IsActorBeingDestroyed')~=true
end
-- Identity assumes the caller has already checked liveness. Never recurse back
-- through rawValid while walking the same UObject/owner/world chain.
local function identity(o)
    local address=call(o,'GetAddress'); local name=call(o,'GetFullName')
    if address and name then return tostring(address)..':'..name end
end
local function retiringIdentity(o)
    if not Value.isObject(o) then return end
    local address,name=call(o,'GetAddress'),call(o,'GetFullName')
    local number=address and tonumber(address)
    if number and number>0 and type(name)=='string' and name~='' then
        return tostring(address)..':'..name
    end
end
function L.key(o)
    if L.rawValid(o) then return identity(o) end
end
function L.worldKey(o)
    if not L.alive(o) then return nil end
    local world=call(o,'GetWorld')
    if L.rawValid(world) then return identity(world) end
end
function L.alive(o)
    if not L.rawValid(o) then return false end
    local key=identity(o); return not (key and L.retired[key])
end
-- UE4SS may return an invalid address-zero wrapper for a native null pointer.
local function present(o)
    if not o then return false end
    local address=call(o,'GetAddress')
    if address~=nil then return tonumber(address)~=0 end
    return L.rawValid(o)
end
local function ownersAlive(o)
    local parent
    if call(o,'IsA','/Script/Engine.Actor')==true then parent=call(o,'GetOwner') end
    if not present(parent) then parent=call(o,'GetOuter') end
    local seen={}
    for _=1,8 do
        if not present(parent) then break end
        if not L.rawValid(parent) then return false end
        local key=identity(parent)
        if key and L.retired[key] then return false end
        if not key or seen[key] then break end
        seen[key]=true
        parent=call(parent,'GetOuter')
    end
    return true
end
function L.valid(o)
    if not L.alive(o) or not ownersAlive(o) then return false end
    local worldObject=call(o,'GetWorld')
    local world=L.rawValid(worldObject) and identity(worldObject)
    if not world then
        local name=call(o,'GetFullName') or ''
        if not name:find('Default__',1,true) then
            for _,class in ipairs({'/Script/Engine.Actor','/Script/Engine.ActorComponent','/Script/UMG.UserWidget'}) do
                if call(o,'IsA',class)==true then return false end
            end
        end
    end
    return not world or not L.departed[world] and (not L.world or world==L.world)
end
-- Check the tick sender before passing it to any engine world-clock helper.
function L.tickable(o)
    if options.tick_owners~=false and (not L.alive(o) or not ownersAlive(o)) then return false end
    local world=L.worldKey(o)
    return world~=nil and world~=L.blocked and not L.departed[world]
end
function L.sameWorld(a,b)
    if not L.valid(a) or not L.valid(b) then return false end
    local wa,wb=L.worldKey(a),L.worldKey(b)
    return wa~=nil and wa==wb
end
local function listen(list,fn)
    list[#list+1]=fn
    return function() for i,v in ipairs(list) do if v==fn then table.remove(list,i); break end end end
end
function L.onReset(fn) return listen(L.listeners,fn) end
function L.onRetired(fn) return listen(L.onRetire,fn) end
function L.reset(reason)
    L.epoch=L.epoch+1
    if api.log then api.log('INFO world context reset reason='..tostring(reason or 'owner reset')..' epoch='..L.epoch) end
    for _,fn in ipairs({table.unpack(L.listeners)}) do fn(reason) end -- No UObject access/mutations in reset listeners
end
function L.begin(reason)
    beginRevision=beginRevision+1;continuingController=nil
    L.transition=true; L.blocked=L.world; L.sample=nil; L.sampleTime=nil
    boundaryState=nil; boundaryController=nil
    if L.world then L.departed[L.world]=true end
    L.reset(reason)
end
local function retire(o,key,reason,continuation)
    if not key or L.retired[key] then return false end
    -- Commit retirement before reset/removal listeners can observe or retrack.
    L.retired[key]=true
    L.retirement_revision=L.retirement_revision+1 -- pure commit, before reset/log/listener reentry
    if reason and not L.transition then
        local previous,previousEpoch=beginRevision,L.epoch
        L.begin(reason)
        -- A nested travel/reset cannot be overwritten by this outer retirement.
        if continuation and beginRevision==previous+1 and L.epoch==previousEpoch+1 then
            continuation.epoch=L.epoch;continuation.beginRevision=beginRevision
            continuingController=continuation
        end
    end
    for _,fn in ipairs({table.unpack(L.onRetire)}) do fn(o) end
    return true
end
function L.retire(o)
    o=Value.unwrap(o)
    return retire(o,retiringIdentity(o))
end
-- Address/name pairs can be reused on a second load of the same map. Only an
-- observed native birth of a local controller/GameState may admit that world;
-- a stale Tick alone must never unlock the departing generation.
function L.born(o)
    if not L.rawValid(o) or call(o,'IsA','/Script/Engine.Actor')~=true then return end
    local isState=call(o,'IsA','/Script/Engine.GameStateBase')==true
    if not isState and call(o,'IsLocalPlayerController')~=true then return end
    local world=L.key(call(o,'GetWorld')); if not world then return end
    local key=L.key(o); if key then L.retired[key]=nil end
    continuingController=nil
    if key then
        local record={key=key,world=world}
        if isState then boundaryState=record else boundaryController=record end
    end
    if L.transition and (world==L.blocked or L.departed[world]) then
        L.departed[world]=nil; L.blocked=nil; L.world=nil
        L.sample=nil; L.sampleTime=nil
    end
    return call(o,'IsA','/Game/Blueprints/Player/B_MenuPlayerController.B_MenuPlayerController_C')==true and 'menu' or 'game'
end
-- Only the native BeginPlay dispatcher may revoke another actor's retirement.
-- A fresh PS/prop can reuse a key in the current world without providing the
-- local-controller/GameState proof needed to release a travel latch.
local function actorBorn(p)
    if next(L.retired)==nil then return end
    local o=Value.unwrap(p)
    if not L.rawValid(o) or call(o,'IsA','/Script/Engine.Actor')~=true then return end
    local key=identity(o)
    if not key or not L.retired[key] then return end
    local world=L.key(call(o,'GetWorld'))
    if world and world==L.world then L.retired[key]=nil end
end
-- Native menu GetWidget/ChangeMenu prove a destination UI object. A native
-- local-controller birth must first revoke any departing/reused-world latch.
function L.menuReady(pc)
    if not L.rawValid(pc) or call(pc,'IsA','/Script/Engine.Actor')~=true or call(pc,'IsLocalPlayerController')~=true
        or call(pc,'IsA','/Game/Blueprints/Player/B_MenuPlayerController.B_MenuPlayerController_C')~=true then return false end
    local world=L.worldKey(pc)
    if not world or world==L.blocked or L.departed[world] then return false end
    if L.world~=world then
        continuingController=nil
        local prior=L.world; L.world=world; L.retired={}
        if prior then L.departed[prior]=true; L.reset('native menu activation') end
    end
    L.transition=false; L.blocked=nil; L.sample=nil; L.sampleTime=nil
    local key=L.key(pc)
    if key then boundaryController={key=key,world=world} end
    return true
end
function L.destination(o) return L.born(o) end
-- Package activation can occur after native BeginPlay. Consume the runtime's
-- already-admitted owner cache once; never scan for or guess a missing owner.
local function admittedRuntimeController(world)
    local runtime=WIR_RUNTIME
    -- The runtime's healthy observe setter supplies a scalar mirror, including
    -- observations after installation. Its clear revokes that borrowed proof.
    if type(runtime)=='table' and runtime.input_owner_supported then return end
    local input=type(runtime)=='table' and runtime.input
    local pc=type(input)=='table' and input.controller
    if not L.rawValid(pc) or call(pc,'IsA','/Script/Engine.Actor')~=true
        or call(pc,'IsLocalPlayerController')~=true then return end
    local pcWorld=L.key(call(pc,'GetWorld'))
    local key=L.key(pc)
    if key and pcWorld==world then boundaryController={key=key,world=world} end
end
-- Only a ticking destination can release a travel latch. Two different frame
-- times prevent multiple actors' callbacks in one frame from proving settlement.
function L.observe(o,time)
    local world=L.worldKey(o); if not world then return not L.transition end
    if L.blocked==world or L.departed[world] then return false end
    if L.world~=world then
        continuingController=nil
        local prior=L.world
        L.world=world; L.retired={}
        if prior then L.departed[prior]=true; L.transition=true; L.reset('world identity changed') end
        L.sample=world; L.sampleTime=time
        return not L.transition
    end
    if L.transition then
        if L.sample~=world then L.sample=world; L.sampleTime=time; return false end
        if time==nil or time==L.sampleTime then return false end
        L.transition=false; L.blocked=nil
    end
    return true
end
local function admittedContinuingController(record)
    local previous=continuingController
    local runtime=WIR_RUNTIME
    local current=type(runtime)=='table' and runtime.input_owner
    if not previous or type(record)~='table' or type(current)~='table'
        or runtime.alive~=true or runtime.loading or runtime.state_retired
        or type(runtime.input)~='table' or runtime.input.muted
        or runtime.world_generation~=previous.generation or nativeKey(runtime.world_key)~=previous.world
        or record.generation~=previous.generation or record.key~=current.key or record.world~=current.world
        or record.generation~=current.generation or nativeKey(record.world)~=previous.world
        or not L.transition or L.world~=previous.world or L.blocked~=previous.world
        or not L.departed[previous.world] or L.epoch~=previous.epoch or beginRevision~=previous.beginRevision then return false end
    local key=nativeKey(record.key)
    if not key or key==previous.key or L.retired[key] then return false end
    -- Only a distinct healthy setter admission may resume a local-PC-only
    -- departure in this exact continuing world. GS/travel has no such token.
    continuingController=nil
    L.departed[previous.world]=nil;L.blocked=nil;L.transition=false
    L.sample=nil;L.sampleTime=nil
    boundaryController={key=key,world=previous.world}
    return true
end
function L.install(nativeTravelHooks)
    if L.installed then return end
    L.installed=true
    api.on('load_map_pre',function() L.begin('LoadMap PRE') end,{ungated=true})
    api.on('load_map_post',function()
        L.departed={}; L.retired={}; L.blocked=nil
    end,{ungated=true})
    api.on('world_changed',function() L.begin('runtime world changed') end,{ungated=true})
    api.on('world_ready',function(actor)
        if not L.rawValid(actor) then return end
        -- Birth can reuse the retired actor's address/fullname. The runtime's
        -- native proof is what permits clearing that retired identity.
        local world=L.key(call(actor,'GetWorld'))
        if world then
            L.blocked=nil; L.departed[world]=nil
            -- Native InitGameState/GS birth proves this destination, independently
            -- of LoadMap POST or a later heartbeat. No retired-world reflection.
            continuingController=nil
            L.world=world; L.transition=false; L.sample=nil; L.sampleTime=nil
            local key=L.key(actor); if key then L.retired[key]=nil end
            -- InitGameState POST supplies GameMode; GS birth supplies the GS.
            -- Both are healthy admission boundaries, before ordinary delivery.
            local state=call(actor,'IsA','/Script/Engine.GameStateBase')==true and actor or Value.read(actor,'GameState')
            if L.rawValid(state) and call(state,'IsA','/Script/Engine.GameStateBase')==true then
                local stateKey=L.key(state)
                if stateKey then boundaryState={key=stateKey,world=world} end
            end
            admittedRuntimeController(world)
        end
    end,{ungated=true})
    api.on('begin_play_pre',function(p) L.born(Value.unwrap(p)) end,{ungated=true,class_filter=function(name)
        return name:find('GameState',1,true)~=nil or name:find('PlayerController',1,true)~=nil
    end})
    api.on('begin_play_pre',actorBorn,{ungated=true})
    if nativeTravelHooks~=false then
        for _,path in ipairs({'/Script/Engine.GameplayStatics:OpenLevel',
            '/Script/Engine.PlayerController:ClientTravel','/Script/Engine.PlayerController:ClientTravelInternal'}) do
            api.hook(path,function() L.begin('travel PRE') end,{phase='pre',ungated=true})
        end
    end
    api.on('input_owner_admitted',admittedContinuingController,{ungated=true})
    api.on('end_play_retire',function(p)
        local o=Value.unwrap(p)
        local key=retiringIdentity(o); if not key then return end
        -- Runtime's native GS setter also covers an activation after GS birth.
        -- Compare copied identity strings; never dereference that GS here.
        local stateKey,stateWorld=runtimeState()
        local state=stateKey and stateWorld and {key=stateKey,world=stateWorld} or boundaryState
        local controllerKey,controllerWorld=runtimeController()
        local controller=controllerKey and controllerWorld and {key=controllerKey,world=controllerWorld} or boundaryController
        if state and state.key==key and state.world==L.world then continuingController=nil end
        local isBoundary=L.world and (state and state.key==key and state.world==L.world
            or controller and controller.key==key and controller.world==L.world)
        local runtime=WIR_RUNTIME
        local continuation
        -- The mirror itself proves the exact local controller role. A GS,
        -- native travel, a stale cache or an already departed context cannot
        -- grant generic input admission permission to reopen that world.
        if isBoundary and controllerKey==key and controllerWorld==L.world and key~=stateKey
            and type(runtime)=='table' and runtime.alive==true and not runtime.loading and not runtime.state_retired
            and nativeKey(runtime.world_key)==L.world and type(runtime.world_generation)=='number'
            and type(runtime.input)=='table' and not runtime.input.muted then
            continuation={key=key,world=L.world,generation=runtime.world_generation}
        end
        if retire(o,key,isBoundary and 'current owner EndPlay' or nil,continuation) then
            -- A setter can reenter the existing reset/removal callbacks before
            -- the token is committed. Consume only its still-current admission
            -- synchronously when this retirement completion returns.
            if continuation and type(runtime)=='table' then admittedContinuingController(runtime.input_owner) end
        end
    end,{ungated=true})
    local runtime=WIR_RUNTIME
    if type(runtime)=='table' and runtime.alive==true and not runtime.loading
        and not runtime.state_retired and not L.transition then
        local stateKey,world=runtimeState()
        if world and not L.departed[world] and (not L.world or L.world==world) then
            L.world=world
            if stateKey then boundaryState={key=stateKey,world=world} end
            admittedRuntimeController(world)
        end
    end
end
function L.same(a,b)
    local ka,kb=L.key(a),L.key(b)
    return ka~=nil and ka==kb
end
return L
end
return M
