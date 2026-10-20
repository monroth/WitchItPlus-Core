-- Loader-thread work only schedules bootstrap. Required preflight and channel
-- contracts complete on GT before publishing the runtime to package constructors.
if WIR_RUNTIME then return end
local nativePrint=print
local candidate,terminalLogged
local function terminal(reason)
    if terminalLogged then return end
    terminalLogged=true
    local text=type(reason)=='string' and reason:match('^[^\r\n]+') or 'bootstrap failed'
    if candidate then
        candidate.terminate(text)
        if WIR_RUNTIME==candidate then WIR_RUNTIME=nil end
    end
    nativePrint('[WIR] startup UNAVAILABLE '..text..'\n')
end
local function bootstrap()
    if WIR_RUNTIME then return end
    local ok,why=pcall(function()
        assert(type(IsInGameThread)=='function' and IsInGameThread()==true,'runtime bootstrap requires game thread')
        assert(IsInAsyncThread==nil or IsInAsyncThread()==false,'runtime bootstrap on async thread')
        local lifecycle={
            {'load_map_pre','RegisterLoadMapPreHook'}, {'begin_play_pre','RegisterBeginPlayPreHook'},
            {'begin_play_post','RegisterBeginPlayPostHook'}, {'end_play_pre','RegisterEndPlayPreHook'},
            {'end_play_post','RegisterEndPlayPostHook'}, {'init_game_state_post','RegisterInitGameStatePostHook'},
        }
        for _,name in ipairs({'ExecuteInGameThread','ExecuteInGameThreadWithDelay','ExecuteInGameThreadAfterFrames',
            'RegisterConsoleCommandHandler','CancelDelayedAction',
            'RegisterLoadMapPostHook','IterateGameDirectories','StaticFindObject'}) do
            assert(type(_G[name])=='function',name..' required by runtime v3')
        end
        for _,pair in ipairs(lifecycle) do assert(type(_G[pair[2]])=='function',pair[2]..' required by runtime v3') end
        assert(ModRef~=nil and (type(ModRef)=='userdata' or type(ModRef)=='table')
            and type(ModRef.type)=='function' and ModRef:type()=='ModRef','LuaModRef OnUnload contract required by runtime v3')
        local directory=assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'),'missing script directory')
        local function log(s)assert(type(s)=='string','runtime log requires a string');nativePrint('[WIR] '..s..'\n')end
        local folders=dofile(directory..'discovery.lua')(directory,log)
        local R=dofile(directory..'runtime.lua')(folders,log,dofile(directory..'config.lua'));candidate=R
        R.preflight() -- storage/settings/provisioning failures cannot leave a singleton
        ModRef.OnUnload=R.shutdown
        R.native_call('RegisterConsoleCommandHandler','wir',function(_,parameters,output)return R.command(parameters,output)end)
        for _,pair in ipairs(lifecycle) do
            local event=pair[1]
            R.native_call(pair[2],function(...)if R.alive then R.emit(event,...)end end)
        end
        R.native_call('RegisterLoadMapPostHook',function(_,world,...)
            local object=world:get();R.observe_world(object,nil,true);R.emit('load_map_post',_,world,...)
        end)
        assert(R.is_healthy(),'runtime bootstrap did not complete')
        WIR_RUNTIME=R -- package constructors require this, after required preflight
        R.start()
        assert(R.started and R.is_healthy(),R.fault_reason or 'runtime startup did not complete')
    end)
    if not ok then terminal(why) end
end
if type(ExecuteInGameThreadAfterFrames)~='function' then terminal('ExecuteInGameThreadAfterFrames required by runtime v3');return end
local ok,handle=pcall(ExecuteInGameThreadAfterFrames,1,bootstrap)
if not ok then terminal(handle)
elseif math.type(handle)~='integer' then terminal('ExecuteInGameThreadAfterFrames must return an integer handle') end
return ok and handle or nil
