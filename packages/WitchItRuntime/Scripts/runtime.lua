local Environment=dofile(debug.getinfo(1,'S').source:match('^@(.+[\\/])')..'environment.lua')
local Diagnostics=dofile(debug.getinfo(1,'S').source:match('^@(.+[\\/])')..'diagnostics.lua')
local CORE_COMPONENTS={UE4SS=true,WitchItNativeProducer=true,WitchItRuntime=true,Core=true,UIKit=true}
local Manifest=dofile(debug.getinfo(1,'S').source:match('^@(.+[\\/])')..'manifest.lua')
local function keys(t)
    local out={}; for k in pairs(t) do out[#out+1]=k end; table.sort(out); return out
end
local function count(t) local n=0; for _ in pairs(t) do n=n+1 end; return n end
local function traceback(err)
    local message=type(err)=='string' and err or ('error value type='..type(err))
    return debug.traceback(message,2)
end
return function(folders,log,options)
    options=options or {}
    local R
    local output,outputEpoch=nil,0
    local function output_current(frame)
        return frame and not frame.closed and frame.epoch==outputEpoch and output==frame
            and R and R.is_healthy()
    end
    local function revoke_output()
        outputEpoch=outputEpoch+1
        local frame=output;output=nil -- revoke before dropping captures; never device.Log
        while frame do
            local parent=frame.parent
            frame.closed=true;frame.device=nil;frame.parent=nil
            frame=parent
        end
    end
    local function enter_output(device)
        if not R.is_healthy() then return end
        local frame={device=device,parent=output,epoch=outputEpoch}
        output=frame;return frame
    end
    local function leave_output(frame)
        if output_current(frame) then
            local parent=frame.parent
            output=parent and not parent.closed and parent.epoch==outputEpoch and parent or nil
        elseif output==frame then output=nil end
        frame.closed=true;frame.device=nil;frame.parent=nil
    end
    local dir=debug.getinfo(1,'S').source:match('^@(.+[\\/])')
    local Store=dofile(dir..'storage.lua')
    local rawLog=log
    local function console_copy(message)
        local names={WitchItRuntime='Mod runtime',WitchItNativeProducer='Mod support',Core='Core',UIKit='UIKit',BaseWorldPlus='Editor tweaks',FillAPotPlus='Fill a Pot+',MatchRulesPlusPlus='Match Rules +',PrivateCosmetics='Private Cosmetics'}
        local component,body=message:match('^%[([^%]]+)%]%s*(.*)')
        if component then
            local name=names[component]or 'Mod'
            if body:find('Refused:',1,true)or body:find('FAILED',1,true)or body:find('%.lua:%d+:')then
                return name..': Could not complete this action. Try again.'
            end
            if body:match('^shiphide queued ')then return name..': Queued.'end
            if body:lower():find('native',1,true)or body:lower():find('companion',1,true)then return nil end
            return name..': '..body
        end
        local id,version,status=message:match('^list package=(%S+) version=(%S+).- status=(%S+)')
        if id then return (names[id]or id)..' '..version..' — '..(status=='loaded'and 'Enabled'or status=='disabled'and 'Disabled'or 'Unavailable')end
        id=message:match('^disabled package=(%S+)')
        if id then return (names[id]or id)..' disabled.'end
        id=message:match('^loaded package=(%S+)')
        if id then return (names[id]or id)..' enabled.'end
        id=message:match('^(%S+): core component, cannot be disabled')
        if id then return (names[id]or id)..' is required and cannot be disabled.'end
        local key,chord,description=message:match('^keys (%S+) (%S+) %b[] (.*)')
        if key then return description..': '..chord end
        if message:match('^unknown package=')then return 'Mod not found. Use wir list to check its name.'end
        if message:match('^stats world_generation=')then return 'Diagnostics written to the log.'end
        if message:match('^usage:')then return message end
        if message:match('^ERROR')or message:match('^blocked package=')then return 'Could not complete this action. Restart the game and try again.'end
        if message:match('^stats ')or message:match('^hook ')or message:match('^INFO')or message:find('package=',1,true)or message:match('^keys declarations=')then return nil end
        return message
    end
    log=function(message)
        local frame=output -- the raw logger cannot acquire output for this message
        rawLog(message)
        if output_current(frame) and frame.device~=nil then
            local ok,err=pcall(function()
                local device=frame.device
                local method=device.Log
                if not output_current(frame) then return end
                local copy=console_copy(message)
                if copy then method(device,copy)end
            end)
            if not ok and output_current(frame) then
                rawLog('ERROR OutputDevice: '..(type(err)=='string' and err or 'error value type='..type(err)))
            end
        end
    end
    R={version='3.1.0',packages={},hooks={},hook_lookup={},events={},shared={},context={},world_generation=0,alive=true,booting=true,health_state='healthy'}
    function R.is_healthy() return R.alive and R.health_state=='healthy' end
    R.exports=dofile(dir..'shared_exports.lua').new(function()
        -- Ordinary synchronous shutdown may still need its service methods for
        -- verified rollback. Terminal shutdown can never use a borrowed service.
        return R.health_state~='terminal' and R.health_state~='stopped'
    end)
    R.startup=dofile(dir..'startup.lua').new(options.startup_clock,log)
    R.store_module=Store
    function R.mkdir(path)
        if options.mkdir then return options.mkdir(path) end
        return Store.provision({path},R.booting)
    end
    function R.storage()
        if not R.store then
            local root=options.saved_root
            if not root then
                local lib=R.native_call('StaticFindObject','/Script/Engine.Default__KismetSystemLibrary')
                assert(lib:IsValid(),'KismetSystemLibrary unavailable')
                local saved=lib:GetProjectSavedDirectory()
                root=(type(saved)=='string' and saved or saved:ToString()):gsub('[/\\]+$','')..'/WitchItRuntime'
            end
            R.store=Store.new(root,R.mkdir)
        end
        return R.store
    end
    local function on_game_thread()
        return type(IsInGameThread)=='function' and IsInGameThread()==true
            and (type(IsInAsyncThread)~='function' or IsInAsyncThread()==false)
    end
    local retireInputOwner,retireInputContext
    local inputRetirements={}
    function R.ensure_input()
        assert(R.health_state~='terminal' and R.health_state~='stopped','runtime native boundary is unhealthy')
        if not R.input then
            local input=dofile(dir..'input.lua')(R,log)
            R.input=input
            R.input_owner_supported=true
            local revision,admissionSerial=0,0
            local observe,clear=input.observe,input.clear
            local function revoke()
                admissionSerial=admissionSerial+1
                revision=revision+1;R.input_owner=nil
            end
            local function dropInput(mute)
                input.controller=nil;input.controller_name=nil
                local capture=input.capturing
                input.capturing=nil
                if capture then
                    capture.fn=nil;capture.interested=nil;capture.finished=nil;capture.down=nil
                end
                for _,binding in ipairs(input.bindings) do binding.down=false end
                if mute then input.muted=true end
            end
            retireInputContext=function()
                if not R.is_healthy() then return end
                revoke();dropInput(true)
            end
            retireInputOwner=function(owner,controller)
                -- Native retirement is pure: cancellation/finished belongs to
                -- ordinary healthy clear, and may dereference the dying owner.
                -- A replacement admitted during the retirement phase owns a
                -- different scalar record and must survive this completion.
                if not R.is_healthy() or not rawequal(R.input_owner,owner) then return end
                revoke()
                if not rawequal(input.controller,controller) then return end
                dropInput(false)
                -- Keep the continuing world's admission state: observe(newPC)
                -- needs no destination refresh to replace this retired owner.
            end
            input.clear=function(...)
                assert(R.health_state~='terminal' and R.health_state~='stopped','runtime native boundary is unhealthy')
                revoke() -- cancellation callbacks cannot borrow the old identity
                local result=table.pack(pcall(clear,...))
                revoke() -- clear's callback may reenter an admission
                if not result[1] then error(result[2],0) end
                return table.unpack(result,2,result.n)
            end
            input.observe=function(actor,...)
                local guard=select(1,...)
                admissionSerial=admissionSerial+1;local ticket=admissionSerial
                local function allowed()
                    return ticket==admissionSerial and R.is_healthy() and (type(guard)~='function' or guard())
                end
                if not allowed() then return end
                assert(R.alive and R.health_state~='terminal' and R.health_state~='stopped','runtime native boundary is unhealthy')
                -- A GS/load boundary has already revoked this world. Reject
                -- reentry before the ordinary setter can query its old owner.
                if R.loading or R.state_retired then return end
                if #inputRetirements>0 then
                    -- Native EndPlay PRE may still report IsValid=true. A same
                    -- retiring identity cannot manufacture a replacement record
                    -- reentrantly. Identity is the only retiring-object access.
                    local ok,address,name=pcall(function()
                        local address=actor:GetAddress();if not allowed() then return end
                        local name=actor:GetFullName();if not allowed() then return end
                        return address,name
                    end)
                    local key=ok and tonumber(address) and tonumber(address)>0 and type(name)=='string'
                        and name~='' and tostring(address)..'|'..name or nil
                    for _,ending in ipairs(inputRetirements) do
                        if key==ending.key and R.world_key==ending.world
                            and R.world_generation==ending.generation then return end
                    end
                end
                if not allowed() then return end
                local generation=R.world_generation
                local result=table.pack(pcall(observe,actor,allowed))
                if not result[1] then revoke();error(result[2],0) end
                local admittedRevision=revision
                -- observe has no success return. Reprove its exact healthy
                -- admitted owner; an unchanged controller alone is insufficient
                -- when observe rejected the old actor or a foreign world.
                local ok,key,world=pcall(function()
                    if not allowed() or not R.alive or R.loading or R.state_retired or input.muted
                        or input.controller~=actor then return end
                    local valid=actor:IsValid();if not allowed() or not valid then return end
                    local localPlayer=actor:IsLocalPlayerController();if not allowed() or not localPlayer then return end
                    local w=actor:GetWorld();if not allowed() or not w then return end
                    local worldValid=w:IsValid();if not allowed() or not worldValid then return end
                    local address=actor:GetAddress();if not allowed() then return end
                    local name=actor:GetFullName();if not allowed() then return end
                    local wa=w:GetAddress();if not allowed() then return end
                    local wn=w:GetFullName();if not allowed() then return end
                    if not tonumber(address) or tonumber(address)<=0 or type(name)~='string' or name==''
                        or not tonumber(wa) or tonumber(wa)<=0 or type(wn)~='string' or wn=='' then return end
                    return tostring(address)..'|'..name,tostring(wa)..'|'..wn
                end)
                local produced
                if allowed() and ok and key and world==R.world_key and generation==R.world_generation
                    and revision==admittedRevision and input.controller==actor and not input.muted
                    and R.alive and not R.loading and not R.state_retired then
                    revision=revision+1
                    R.input_owner={key=key,world=world,generation=generation}
                    produced=R.input_owner
                    -- Existing healthy admission is the producer, including a
                    -- controller replacement with no GameState/world change.
                    R.emit('input_owner_admitted',{key=key,world=world,generation=generation})
                end
                if type(guard)=='function' then
                    if allowed() and produced and R.input_owner==produced and input.controller==actor then
                        return {key=produced.key,world=produced.world,generation=produced.generation}
                    end
                    return nil
                end
                return table.unpack(result,2,result.n)
            end
        end
        return R.input
    end
    local Resources=dofile(debug.getinfo(1,'S').source:match('^@(.+[\\/])')..'resources.lua')
    Diagnostics.trace_timer_calls=options.trace_timer_calls==true
    Diagnostics.trace_native_access=options.trace_native_access==true
    R.diagnostics=Diagnostics
    function R.native_call(name,...)
        assert(R.health_state~='terminal' and R.health_state~='stopped','runtime native boundary is unhealthy')
        return Diagnostics.invoke(name,_G[name],nil,...)
    end
    local sequence=0
    local maxErrors=options.max_errors or 3
    local errorInterval=options.error_interval or 10
    local function summarize(p,record,held)
        if record.pending==0 then return end
        log('ERROR repeats package='..p.id..' at='..record.where..' repeated='..record.pending
            ..' total='..record.count..' message='..record.message)
        if held and not held() then return end
        record.pending=0; record.last=os.time()
    end
    local function report(p,where,err,resource)
        p.errors=p.errors+1
        p.last_error=err
        local now=os.time()
        local message=err:match('^(.-)\nstack traceback:') or err
        local location=err:match('\n%s*([^\n]-:%d+):') or ''
        local fingerprint=where..'\n'..message..'\n'..location
        local record=p.error_records[fingerprint]
        if not record then
            record={where=where,message=message,count=1,pending=0,last=now}
            p.error_records[fingerprint]=record
            log('ERROR package='..p.id..' at='..where..' count='..p.errors..'\n'..err)
        else
            record.count=record.count+1; record.pending=record.pending+1
            if now-record.last>=errorInterval then summarize(p,record) end
        end
        if p.manifest.gameplay or p.manifest.critical or R.loading
            or R.context.mode and R.context.mode~='menu'
            or R.input_context=='match' or R.input_context=='creative' then
            if resource then
                resource.failures=resource.failures or {}
                local failures=(resource.failures[fingerprint] or 0)+1
                resource.failures[fingerprint]=failures
                if failures>=maxErrors then
                    summarize(p,record)
                    -- A missing gameplay callback is not an active package. Use
                    -- the existing owned teardown/cascade transaction; failed
                    -- rollback retains cleanup_pending for explicit completion.
                    log('CRITICAL package teardown package='..p.id..' at='..where..' same_error='..failures
                        ..' message='..message)
                    R.disable(p.id,'same-error callback threshold at='..where,true)
                end
            end
        elseif p.errors>=maxErrors then
            summarize(p,record)
            log('auto-disable package='..p.id..' errors='..p.errors)
            R.disable(p.id,'error threshold',true)
        end
    end
    -- Console mistakes are individual command failures, not package health.
    -- Keep every occurrence visible and leave subscriptions/tools installed.
    local function reportCommand(p,where,err)
        p.command_errors=(p.command_errors or 0)+1
        p.last_error=err
        log('ERROR package='..p.id..' at='..where..' command_error='..p.command_errors..'\n'..err)
    end
    local function call(p,where,fn,resource,...)
        if not R.is_healthy() or not p.active or resource and resource.api and resource.api~=p.api then return false end
        local measurement=R.startup.begin(p.id,where)
        local result=table.pack(xpcall(function(...) return Diagnostics.access(p.id,'callback:'..where,fn,...) end,traceback,...))
        R.startup.finish(measurement)
        if not R.is_healthy() then return false end
        if not result[1] then (resource and resource.report_error or report)(p,where,result[2],resource) end
        return table.unpack(result,1,result.n)
    end
    function R.profile(p,label,fn,...)
        assert(R.health_state~='terminal' and R.health_state~='stopped','runtime native boundary is unhealthy')
        if not R.startup.active then return fn(...) end
        local measurement=R.startup.begin(p.id,label)
        local result=table.pack(pcall(fn,...))
        R.startup.finish(measurement)
        if not result[1] then error(result[2],0) end
        return table.unpack(result,2,result.n)
    end
    R.notifications=dofile(dir..'notifications.lua').new(R,call,log)
    local function dispatch(list,where,className,...)
        local snapshot={table.unpack(list)}
        for _,s in ipairs(snapshot) do
            if s.alive and s.owner.active and (s.ungated or s.owner.engaged) then
                local interested=true
                if s.class_filter then
                    interested=s.classes[className or '']
                    if interested==nil then
                        local ok,result=call(s.owner,where..' class filter',s.class_filter,s,className or '')
                        interested=ok and result==true
                        if ok then s.classes[className or '']=interested end
                    end
                end
                if interested then call(s.owner,where,s.fn,s,...) end
            end
        end
        -- Subscriber return values deliberately never override a UFunction result.
    end
    local HookState,MapDocument
    local Generations=dofile(dir..'owner_generations.lua').new(R)
    local function uninstall(h,held)
        if not h.installed then return true end
        if held and not held() then return end
        local ok,err=pcall(R.native_call,'UnregisterHook',h.path,h.pre,h.post)
        if held and not held() then return end
        if ok then
            h.installed=false; h.pre=nil; h.post=nil
            if HookState then HookState.changed('hook unregistered') end
            if held and not held() then return end
            log('hook unregister requested path='..h.path)
            return true
        else
            -- UE4SS uninstall still owns native cleanup if explicit unregister fails.
            log('ERROR hook removal path='..h.path..': '..tostring(err))
            return false,'hook '..h.path..': '..tostring(err)
        end
    end
    local function install(h)
        if h.installed or h.installing or #h.subscribers==0 or not R.alive then return end
        -- Avoid allocating UE4SS hook references while a BP UFunction is absent.
        local object=R.native_call('StaticFindObject',h.path)
        if not Diagnostics.invoke('UObject:IsValid',function() return object:IsValid() end) then
            local changed=not h.waiting
            h.waiting=true
            if changed and HookState then HookState.changed('hook waiting') end
            if changed then log('hook pending path='..h.path..' (class not loaded)') end
            return
        end
        local first=function(...)
            if not R.alive then return end
            h.calls=h.calls+1
            return Resources.dispatch_hook(h,h.native and 'pre' or 'post',call,log,...)
        end
        local after=h.native and function(...)
            if R.alive then return Resources.dispatch_hook(h,'post',call,log,...) end
        end or nil
        h.installing=true
        local ok,pre,post
        if after then ok,pre,post=pcall(R.native_call,'RegisterHook',h.path,first,after)
        else ok,pre,post=pcall(R.native_call,'RegisterHook',h.path,first) end
        h.installing=nil
        if ok then
            h.failed=nil;h.installed=true; h.pre=pre; h.post=post; h.waiting=false
            if HookState then HookState.changed('hook installed') end
            log('hook installed path='..h.path..' registrations=1 subscribers='..#h.subscribers)
        else
            h.failed=pre; h.waiting=false
            if HookState then HookState.changed('hook failed') end
            log('ERROR native hook registration failed; refresh may retry path='..h.path..': '..(type(pre)=='string' and pre or type(pre)))
        end
    end
    function R.retry_hooks(owner)
        for _,path in ipairs(keys(R.hooks)) do
            local h=R.hooks[path]
            for _,s in ipairs(h.subscribers) do
                if s.alive and s.owner.active and not s.finite_install and (not owner or s.owner==owner) then
                    install(h);break
                end
            end
        end
    end
    local function subscribe(p,list,fn,h)
        assert(p.active,'package inactive'); assert(type(fn)=='function','callback must be function')
        sequence=sequence+1
        local s={owner=p,fn=fn,alive=true,sequence=sequence}
        list[#list+1]=s
        local function cancel()
            if s.alive then
                Diagnostics.lua_cancel(p.id,h and 'hook' or 'subscriber',h and h.path)
                s.alive=false; s.fn=nil
                for i,v in ipairs(list) do if v==s then table.remove(list,i); break end end
                if h and HookState then HookState.changed('hook subscription revoked') end
            end
            if h and #h.subscribers==0 then
                local ok,why=uninstall(h)
                if ok==false then return false,why end
            end
            p.subscriptions[s]=nil;return true
        end
        p.subscriptions[s]=cancel
        s.cancel=cancel
        return cancel,s
    end
    HookState=dofile(dir..'hook_state.lua').new(R,subscribe,call,install)
    MapDocument=dofile(dir..'map_document.lua').new(R,subscribe,call,Generations)
    local ScalarState=dofile(dir..'scalar_state.lua').new(R,subscribe,call)
    local PackageInventory=dofile(dir..'package_inventory.lua').new(R,subscribe,call,Store.json.array,Manifest.mismatch)
    local OwnerRetirement=dofile(dir..'owner_retirement.lua').new(R)
    local function timer(p,ms,fn,repeating,opts)
        assert(p.active,'package inactive'); assert(type(fn)=='function','callback must be function')
        assert(type(ms)=='number' and ms==math.floor(ms) and ms>0 and ms<=2147483647,'invalid timer ms')
        local t={alive=true,fn=fn,ungated=opts and opts.ungated==true}
        local function cancel()
            if not t.alive then return end
            Diagnostics.lua_cancel(p.id,'timer')
            t.alive=false; t.fn=nil; p.timers[t]=nil
            local handle=t.handle;t.handle=nil
            if handle then R.native_call('CancelDelayedAction',handle) end
        end
        t.cancel=cancel
        p.timers[t]=cancel
        local schedule
        t.resume=function()
            t.handle=nil -- host detached this one-shot before entering Lua
            if not t.alive or not p.active or not R.alive then return end
            if not p.engaged and not t.ungated then t.pending=true; return end
            t.pending=false
            local callback=t.fn
            if not repeating then cancel() end
            call(p,repeating and 'loop' or 'delay',callback,t)
            if repeating and t.alive and p.active then schedule() end
        end
        schedule=function()
            -- Native steady-clock delay, dispatched by UE4SS's game-thread queue.
            -- Never enter Lua on the async loop, even for a queue handoff.
            local handle=R.native_call('ExecuteInGameThreadWithDelay',ms,function() t.resume() end)
            assert(math.type(handle)=='integer','native delay handle required')
            if t.alive then t.handle=handle else R.native_call('CancelDelayedAction',handle) end
        end
        schedule()
        return cancel
    end
    local function api(p)
        local A={id=p.id,version=p.manifest.version,api_version=p.manifest.api_version,path=p.path}
        function A.log(message) if p.active then log('['..p.id..'] '..tostring(message)) end end
        function A.is_live()
            -- Pure owner-incarnation status, independent of its context gate.
            -- A gated live package may own held timers; a retired API may not.
            return R.is_healthy() and p.active and p.api==A
        end
        function A.fail(reason)
            -- First critical owner failure must not remain an active advertised
            -- package merely because its own guard blocks subsequent callbacks.
            -- An old API cannot tear down a newly enabled incarnation.
            if not R.is_healthy() or not p.active or p.api~=A then return false end
            assert(type(reason)=='string' and reason~='' and #reason<=1024,'invalid owner failure reason')
            assert(R.native_call('IsInGameThread')==true,'owner failure teardown requires game thread')
            return R.disable(p.id,'owner fault: '..reason,true)
        end
        function A.admit_input_owner(actor)
            if not A.is_live() or R.loading or R.state_retired or not R.world_key then return nil end
            local generation,world=R.world_generation,R.world_key
            local function allowed()
                return A.is_live() and not R.loading and not R.state_retired
                    and generation==R.world_generation and world==R.world_key
            end
            if R.native_call('IsInGameThread')~=true or not allowed() then return nil end
            local input=R.ensure_input();if not allowed() then return nil end
            local admitted=input.observe(actor,allowed)
            if not allowed() then return nil end
            if admitted and admitted.world==world and admitted.generation==generation then
                return {key=admitted.key,world=world,generation=generation}
            end
        end
        function A.on(event,fn,opts)
            assert(type(event)=='string' and event~='','invalid event')
            if opts and opts.class_filter then
                assert(event=='begin_play_pre' or event=='begin_play_post','class filter requires BeginPlay')
                assert(type(opts.class_filter)=='function','invalid class filter')
            end
            R.events[event]=R.events[event] or {}
            local cancel,s=subscribe(p,R.events[event],fn)
            s.ungated=opts and opts.ungated or event=='context_changed'
            if opts and opts.class_filter then
                s.class_filter=opts.class_filter; s.classes={}
            end
            return cancel
        end
        function A.hook(path,fn,opts)
            assert(A.is_live(),'package API inactive')
            assert(type(path)=='string' and path:match('^/.+:.+$'),'invalid UFunction path')
            local name=path:match(':([^:]+)$')
            assert(not Resources.hot[name],'hot hook forbidden: '..path)
            opts=opts or {}
            local native=path:sub(1,8):lower()=='/script/'
            local phase=opts.phase or (native and 'pre' or 'post')
            assert(phase=='pre' or phase=='post','invalid hook phase')
            assert(native or phase=='post','Blueprint pre hooks are unavailable: '..path)
            local priority=opts.priority or 0
            assert(type(priority)=='number' and priority==priority and math.abs(priority)<math.huge,'invalid priority')
            local outputs={}
            for _,index in ipairs(opts.out_params or {}) do
                assert(type(index)=='number' and index>=1 and index%1==0,'invalid out parameter index')
                outputs[index]=true
            end
            local h=R.hook_lookup[path:lower()]
            if not h then
                h={path=path,subscribers={},installed=false,native=native,calls=0,since=os.time(),conflicts={}}
                R.hooks[path]=h; R.hook_lookup[path:lower()]=h
            end
            local cancel,s=subscribe(p,h.subscribers,fn,h)
            s.api=A
            s.phase=phase; s.priority=priority; s.override=p.manifest.api_version>=2 and opts.override==true
            s.outputs=outputs; s.ungated=opts.ungated==true;s.finite_install=opts.finite_install==true
            HookState.changed('hook subscription added')
            install(h); return cancel
        end
        function A.retry_hooks() if p.active then R.retry_hooks(p) end end
        function A.delay(ms,fn,opts) return timer(p,ms,fn,false,opts) end
        function A.loop(ms,fn,opts) return timer(p,ms,fn,true,opts) end
        Resources.api(R,p,A,subscribe,call,log,options,reportCommand)
        Generations.attach(p,A)
        HookState.attach(p,A)
        MapDocument.attach(p,A)
        ScalarState.attach(p,A)
        PackageInventory.attach(p,A)
        OwnerRetirement.attach(p,A)
        return A
    end
    for _,folder in ipairs(folders) do
        local ok,m=xpcall(function()
            return Manifest.validate(assert(loadfile(folder.path..'/manifest.lua','t',{}))())
        end,traceback)
        if not ok then log('rejected folder='..folder.folder..'\n'..m)
        elseif R.packages[m.id] then
            R.packages[m.id].blocked='duplicate id'
            log('rejected duplicate id='..m.id..' folder='..folder.folder)
        else
            R.packages[m.id]={id=m.id,manifest=m,path=folder.path,status='disabled',errors=0,
                subscriptions={},timers={},error_records={},active=false,engaged=true}
        end
    end
    -- Each synchronous teardown owns an exact API incarnation and runtime
    -- lifecycle. A terminal runtime cannot resume old rollback.
    local disableOperations={}
    local worldBoundarySerial=0
    local detach_package
    local function teardown_lane()
        if not on_game_thread() then return end
        local health,alive,unloading=R.health_state,R.alive,R.unload_started
        if not R.is_healthy() and not (health=='stopping' and unloading and alive==false) then return end
        local generation,world,loading,retired=R.world_generation,R.world_key,R.loading,R.state_retired
        local boundary=worldBoundarySerial
        return function()
            return on_game_thread() and R.health_state==health and R.alive==alive and R.unload_started==unloading
                and R.world_generation==generation and R.world_key==world and R.loading==loading and R.state_retired==retired
                and worldBoundarySerial==boundary
        end
    end
    local function release_package(p,held)
        if held and not held() then return false end
        R.exports.revoke(p.id)
        if held and not held() then return false end
        -- Release the old incarnation before a hook-state observer may enable
        -- its successor. No old operation may write after that observer returns.
        disableOperations[p]=nil;p.disabling=nil
        R.shared[p.id]=nil; p.gate=nil; p.api=nil; p.storage=nil; p.env=nil
        p.cleanup_pending=nil; p.cleanup_errors=nil; p.status='disabled'
        HookState.flush('package retired')
        return true
    end
    local function abandon_disable(operation)
        -- Only the captured old incarnation may enter this pure lane. A
        -- terminal detach or newer API already owns its own records instead.
        if not operation.owns() then return false end
        if not operation.owns() then return false end
        R.exports.revoke(operation.id) -- runtime-owned pure lease revocation
        if not operation.owns() then return false end
        local p=operation.package
        detach_package(p) -- never invoke package cleanup/abandonment or Native
        p.active=false;p.engaged=false;p.status='disabled'
        p.reason='cleanup abandoned: runtime lifecycle changed'
        R.shared[operation.id]=nil
        HookState.changed('package cleanup abandoned',true)
        return false
    end
    local function cancellation_snapshot(list)
        local snapshot={}
        for token,cancel in pairs(list) do snapshot[#snapshot+1]={token=token,cancel=cancel} end
        -- Detach subscriptions first, then undo resources in reverse creation
        -- order: feature rollback precedes the native scope it borrows.
        table.sort(snapshot,function(a,b)
            local x=type(a.token)=='table' and a.token.cleanup_order or nil
            local y=type(b.token)=='table' and b.token.cleanup_order or nil
            return (x or math.huge)>(y or math.huge)
        end)
        return snapshot
    end
    function R.abandon_pending(reason)
        local lane=teardown_lane()
        if not lane then return end -- terminal lane already detached ownership
        for _,id in ipairs(keys(R.packages)) do
            local p=R.packages[id]
            if p.cleanup_pending then
                local A,environment,subscriptions,timers,operation=p.api,p.env,p.subscriptions,p.timers,disableOperations[p]
                local function held()
                    return lane() and R.packages[id]==p and p.api==A and p.env==environment
                        and p.subscriptions==subscriptions and p.timers==timers and disableOperations[p]==operation
                end
                for _,entry in ipairs(cancellation_snapshot(subscriptions)) do
                    if not held() then return end
                    local token=entry.token
                    -- Only the explicit pure bookkeeping lane runs here.
                    if type(token)=='table' and type(token.abandon)=='function' then
                        local ok,err=pcall(token.abandon)
                        if not held() then return end
                        if not ok then
                            log('ERROR cleanup abandonment package='..id..': '..(type(err)=='string' and err or 'error value type='..type(err)))
                            if not held() then return end
                        end
                    end
                    subscriptions[token]=nil
                end
                if not release_package(p,held) then return end
                if not lane() or R.packages[id]~=p or p.api~=nil or p.env~=nil or p.active or p.disabling
                    or p.subscriptions~=subscriptions or p.timers~=timers or disableOperations[p]~=nil then return end
                log('cleanup abandoned package='..id..' reason='..reason)
                if not lane() then return end
            end
        end
    end
    function R.disable(id,reason,internal)
        if CORE_COMPONENTS[id] and internal~=true then
            log(id..': core component, cannot be disabled');return false
        end
        local lane=teardown_lane()
        if not lane then return false end
        local p=R.packages[id]
        if not p then log('unknown package='..tostring(id)); return false end
        if p.disabling then return true end
        local A,environment,subscriptions,timers=p.api,p.env,p.subscriptions,p.timers
        local operation={id=id,package=p,api=A}
        disableOperations[p]=operation
        function operation.owns()
            return disableOperations[p]==operation and R.packages[id]==p and p.api==A and p.env==environment
                and p.subscriptions==subscriptions and p.timers==timers and p.disabling==true and p.active==false
        end
        local function held() return lane() and operation.owns() end
        p.disabling=true
        local retirement=OwnerRetirement.begin(p) -- pure exact-owner capture
        -- Quiesce this owner before dependent rollback can reenter dispatch.
        -- Its retained service leases remain available only until owned teardown
        -- completes, as required by the existing cleanup_pending transaction.
        p.active=false
        MapDocument.retire_package(p)
        if not held() then return abandon_disable(operation) end
        HookState.changed('package retired',true) -- pure before dependency rollback
        if not held() then return abandon_disable(operation) end
        OwnerRetirement.dispatch(retirement) -- healthy WIR owns this one-shot lane
        if not held() then return abandon_disable(operation) end
        log('INFO package teardown package='..id..' reason='..(reason or 'console'))
        if not held() then return abandon_disable(operation) end
        -- Dependents first, transitively, including during an in-flight dispatch.
        local cascadeDone=true
        for _,other in ipairs(keys(R.packages)) do
            local q=R.packages[other]
            if (q.active or q.cleanup_pending) and q.manifest.dependencies[id] then
                if not R.disable(other,'cascade dependency '..id,true) then cascadeDone=false end
                if not held() then return abandon_disable(operation) end
            end
        end
        for _,record in pairs(p.error_records) do
            summarize(p,record,held)
            if not held() then return abandon_disable(operation) end
        end
        -- Scope callbacks may cancel other resources; each pass uses a snapshot.
        -- Retry only failed rollback callbacks once, never from a timer/poll.
        local errors={};p.cleanup_errors=errors
        for attempt=1,2 do
            for _,entry in ipairs(cancellation_snapshot(subscriptions)) do
                if not held() then return abandon_disable(operation) end
                local token,cancel=entry.token,entry.cancel
                if subscriptions[token]==cancel then
                    local ok,result,reason=xpcall(cancel,traceback)
                    if not held() then return abandon_disable(operation) end
                    if ok and result~=false then
                        subscriptions[token]=nil; errors[token]=nil
                    else
                        errors[token]=ok and (reason or ('package '..id..' resource '..tostring(type(token)=='table' and (token.resource or token.kind) or debug.getinfo(cancel,'S').short_src)..': cancellation returned false without a reason')) or result
                        -- Older resources may be borrowed by this undo. Retain
                        -- that stack until the failed consumer can roll back.
                        if type(token)=='table' and token.cleanup_order then break end
                    end
                end
            end
            if not next(subscriptions) then break end
        end
        for _,entry in ipairs(cancellation_snapshot(timers)) do
            if not held() then return abandon_disable(operation) end
            local ok,err=pcall(entry.cancel)
            if not held() then return abandon_disable(operation) end
            if not ok then
                log('ERROR timer cancellation package='..id..': '..(type(err)=='string' and err or 'error value type='..type(err)))
                if not held() then return abandon_disable(operation) end
            end
            if type(entry.token)=='table' then entry.token.alive=false; entry.token.fn=nil end
            timers[entry.token]=nil
        end
        R.shared[id]=nil;p.gate=nil -- pending rollback may still own borrowed service methods
        local why=reason or 'console';p.reason=why
        if next(subscriptions) then
            disableOperations[p]=nil;p.disabling=false
            p.cleanup_pending=true; p.status='cleanup_pending'
            local function pending()
                return lane() and R.packages[id]==p and p.api==A and p.env==environment and not p.active and not p.disabling
                    and disableOperations[p]==nil and p.subscriptions==subscriptions and p.timers==timers
                    and p.cleanup_errors==errors and p.cleanup_pending and p.status=='cleanup_pending'
            end
            for _,err in pairs(errors) do
                if not pending() then return false end
                log('ERROR cleanup pending package='..id..': '..err)
                if not pending() then return false end
            end
            return false
        end
        if not release_package(p,held) then return abandon_disable(operation) end
        local function released()
            return lane() and R.packages[id]==p and p.api==nil and p.env==nil and not p.active and not p.disabling
                and disableOperations[p]==nil and p.subscriptions==subscriptions and p.timers==timers and p.status=='disabled'
        end
        if not released() then return false end
        log('disabled package='..id..' reason='..why)
        if not released() then return false end
        return cascadeDone
    end
    -- Plan the entire closure before starting any package, avoiding partial cycle loads.
    local function plan(id,startup)
        local order,visiting,done={},{},{}
        local function settle_dependents(key,seen)
            seen=seen or {}; if seen[key] then return true end; seen[key]=true
            for _,other in ipairs(keys(R.packages)) do
                local q=R.packages[other]
                if q.manifest.dependencies[key] then
                    local ok,why=settle_dependents(other,seen)
                    if not ok then return false,why end
                    if q.cleanup_pending and not R.disable(other,'retry pending cleanup for enable',true) then
                        return false,'dependent '..other..' cleanup pending'
                    end
                end
            end
            return true
        end
        local function visit(key)
            local p=R.packages[key]
            if not p then return false,'missing dependency '..key end
            if p.disabling then return false,key..' teardown in progress' end
            if p.blocked then return false,key..': '..p.blocked end
            if startup and p.failed_start then return false,'dependency '..key..' failed to start' end
            if startup and not R.package_enabled(p) then return false,'dependency '..key..' disabled in manifest' end
            local settled,why=settle_dependents(key)
            if not settled then return false,why end
            if p.cleanup_pending and not R.disable(key,'retry pending cleanup for enable',true) then
                return false,key..' cleanup pending'
            end
            if done[key] or p.active then return true end
            if visiting[key] then return false,'dependency cycle at '..key end
            visiting[key]=true
            for _,dep in ipairs(keys(p.manifest.dependencies)) do
                local q=R.packages[dep]
                if not q then return false,'missing dependency '..dep end
                local c=p.manifest.dependencies[dep]
                if not Manifest.satisfies(q.manifest.version,c) then
                    return false,'incompatible dependency '..dep..' version='..q.manifest.version..' requires='..c
                end
                local ok,why=visit(dep); if not ok then return false,why end
            end
            visiting[key]=nil; done[key]=true; order[#order+1]=p
            return true
        end
        local ok,why=visit(id); return ok,why,order
    end
    function R.enable(id,startup)
        if not R.is_healthy() then return false end
        local p=R.packages[id]
        if not p then log('unknown package='..tostring(id)); return false end
        local ok,why,order=plan(id,startup)
        if not ok then
            p.status='blocked'; p.reason=why; log('blocked package='..id..' reason='..why); return false
        end
        for _,q in ipairs(order) do
            for dep in pairs(q.manifest.dependencies) do
                if not R.packages[dep].active then
                    q.status='blocked'; q.reason='dependency '..dep..' failed to start'
                    log('blocked package='..q.id..' reason='..q.reason); return false
                end
            end
            disableOperations[q]=nil -- an accepted enable owns a fresh API incarnation
            q.engaged=true; q.active=true; q.status='active'; q.reason=nil; q.errors=0; q.error_records={}; q.failed_start=nil; q.last_error=nil
            local environment=Environment.new(q.id,function(...)
                local values={}
                for index=1,select('#',...) do values[index]=tostring(select(index,...)) end
                local message=table.concat(values,'\t'):gsub('[\r\n]+$','')
                local prefix='['..q.id..'] '
                if message:sub(1,#prefix)==prefix then message=message:sub(#prefix+1) end
                log('['..q.id..'] '..message)
            end,function(name) return q.api.module(name) end,function(name,fn) return Diagnostics.bind(name,fn,q.id) end); q.env=environment
            local started=call(q,'start',function()
                Diagnostics.begin(q.id)
                q.api=api(q)
                local start=assert(loadfile(q.path..'/package.lua','t',environment))()
                assert(type(start)=='function','package.lua must return function(api)')
                start(q.api)
            end,nil)
            Diagnostics.finish(q.id)
            if not R.is_healthy() then return false end
            if not started or not q.active then
                if q.active then R.disable(q.id,'start failed',true) end
                q.failed_start=true
                if p~=q then
                    p.status='blocked'; p.reason='dependency '..q.id..' failed to start'
                    log('blocked package='..p.id..' reason='..p.reason)
                end
                return false
            end
            log('loaded package='..q.id..' version='..q.manifest.version..' api='..q.manifest.api_version..' state=shared')
        end
        return true
    end
    local function objectCall(object,method)
        local ok,value=pcall(function() return object[method](object) end)
        if ok then return value end
    end
    local function objectKey(object)
        if objectCall(object,'IsValid')~=true then return end
        local address=objectCall(object,'GetAddress')
        local name=objectCall(object,'GetFullName')
        if address and address~=0 and name then return tostring(address)..'|'..name,name end
    end
    function R.observe_world(world,gameState,loaded)
        local worldKey,name=objectKey(world)
        if not worldKey then return end
        local stateKey=objectKey(gameState)
        local changed=worldKey~=R.world_key or stateKey and (R.state_retired or R.state_key and stateKey~=R.state_key)
            or loaded and R.loading and not R.load_observed
        R.world_key=worldKey
        if stateKey then R.state_key=stateKey; R.state_retired=nil
        elseif changed then R.state_key=nil; R.state_retired=nil end
        if changed then R.load_observed=true; R.world_changed(name) end
        if loaded then
            R.loading=false; if R.input then R.input.refresh(true) end
        end
        return changed
    end
    local function begin_world_boundary()
        if not R.is_healthy() then return end
        worldBoundarySerial=worldBoundarySerial+1
        local serial=worldBoundarySerial
        -- Refresh a hold only after our own pure lifecycle commit. A callback
        -- cannot refresh it: its nested boundary invalidates this private serial.
        local function capture()
            local lane=teardown_lane()
            return function() return lane and lane() and worldBoundarySerial==serial end
        end
        return capture
    end
    function R.emit(event,...)
        if not R.alive then return end
        local className,actor,fullName,inputRetirement
        if event=='load_map_pre' then
            local capture=begin_world_boundary();if not capture then return end
            local held=capture()
            R.notifications.pause('LoadMap PRE')
            if not held() then return end
            R.loading=true; R.load_observed=false
            held=capture()
            MapDocument.clear('LoadMap PRE',function() ScalarState.clear('LoadMap PRE') end)
            if not held() then return end
            R.abandon_pending('LoadMap PRE')
            if not held() then return end
            if R.input then R.input.clear();if not held() then return end end
        end
        if event=='world_changed' then R.abandon_pending('world changed') end
        if event=='end_play_pre' then
            local argument=select(1,...)
            local ending=objectCall(argument,'get') or argument
            -- This delivered native identity is already inside EndPlay. Liveness
            -- and owner/world reflection cannot decide whether it must retire.
            local address=objectCall(ending,'GetAddress')
            local name=objectCall(ending,'GetFullName')
            local key=tonumber(address) and tonumber(address)>0 and type(name)=='string' and name~=''
                and tostring(address)..'|'..name or nil
            if key then
                inputRetirement={key=key,world=R.world_key,generation=R.world_generation}
                inputRetirements[#inputRetirements+1]=inputRetirement
            end
            local owner=R.input_owner
            local retiringOwner=owner and key==owner.key and owner.world==R.world_key
                and owner.generation==R.world_generation and owner or nil
            local retiringController=retiringOwner and R.input and R.input.controller or nil
            local nativeArguments=table.pack(...)
            local function retirePhase()
                -- Original native arguments, including trailing nils, remain
                -- unchanged through this synchronous internal clear boundary.
                dispatch(R.events.end_play_retire or {},'event end_play_retire',nil,
                    table.unpack(nativeArguments,1,nativeArguments.n))
                if retiringOwner and retireInputOwner then retireInputOwner(retiringOwner,retiringController) end
            end
            if key and key==R.state_key then
                -- Teardown is the cancellation boundary, BEFORE destination
                -- constructors. Objective round travel need not call LoadMap.
                local capture=begin_world_boundary();if not capture then return end
                local held=capture()
                R.notifications.pause('GameState EndPlay')
                if not held() then return end
                R.state_retired=true
                -- GS owns the input context. Drop it purely before any scalar
                -- observer or ordinary listener; finished cannot see a dying GS
                -- with the old Core epoch. Keep the existing GS mute policy.
                if retireInputContext then retireInputContext() end
                held=capture()
                R.abandon_pending('current GameState EndPlay')
                if not held() then return end
                -- Clear all replay first; trusted retirement then precedes both
                -- public scalar nil observers and ordinary EndPlay listeners.
                MapDocument.clear('current GameState EndPlay',function()
                    ScalarState.clear('current GameState EndPlay',retirePhase)
                end)
                if not held() then return end
            elseif not MapDocument.retire_key(key,'current document actor EndPlay',retirePhase) then
                retirePhase()
            end
        end
        if event=='begin_play_pre' or event=='begin_play_post' or event=='init_game_state_post' then
            local argument=select(1,...)
            actor=objectCall(argument,'get') or argument
            if event~='init_game_state_post' then
                fullName=objectCall(actor,'GetFullName') or ''
                className=fullName:match('^(%S+)') or ''
            end
            if event=='init_game_state_post' or event=='begin_play_pre' and className:find('GameState',1,true) then
                local state=actor
                if event=='init_game_state_post' then
                    local ok,value=pcall(function() return actor.GameState end)
                    state=ok and value or nil
                end
                R.observe_world(objectCall(actor,'GetWorld'),state)
                R.emit('world_ready',actor)
            end
        end
        if R.input then
            if event=='world_ready' then R.input.refresh(true)
            elseif event=='begin_play_post' and className and className:find('PlayerController',1,true) then R.input.observe(actor) end
        end
        if fullName then
            local arguments=table.pack(...)
            arguments[arguments.n+1]=className; arguments[arguments.n+2]=fullName
            dispatch(R.events[event] or {},'event '..event,className,table.unpack(arguments,1,arguments.n+2))
        else dispatch(R.events[event] or {},'event '..event,className,...) end
        if inputRetirement then
            for i=#inputRetirements,1,-1 do
                if inputRetirements[i]==inputRetirement then table.remove(inputRetirements,i);break end
            end
        end
        if event=='world_ready' then R.notifications.resume() end
        if event=='begin_play_post' and className=='B_MenuPlayerController_C' then R.startup.stop() end
    end
    function R.world_changed(world_name)
        local capture=begin_world_boundary();if not capture then return end
        local held=capture()
        -- Destination props may be constructed before InitGameState/BeginPlay.
        -- Keep their queued births; the departing queue was cleared at teardown.
        if R.input then R.input.clear();if not held() then return end end
        R.world_generation=R.world_generation+1
        held=capture()
        MapDocument.clear('world changed',function() ScalarState.clear('world changed') end)
        if not held() then return end
        log('world_changed generation='..R.world_generation..' world='..world_name)
        if not held() then return end
        R.emit('world_changed',R.world_generation,world_name)
        if not held() then return end
        -- Existing bounded delayed hook recovery retains only this lifecycle,
        -- never a UObject wrapper; no new retry/poll is introduced here.
        local generation=R.world_generation
        R.native_call('ExecuteInGameThreadWithDelay',1000,function()
            if held() and R.alive and generation==R.world_generation then
                R.retry_hooks()
                if held() and R.input then R.input.refresh() end
            end
        end)
    end
    function R.stats()
        log('stats world_generation='..R.world_generation)
        if R.input then log('stats input native_game_loop=1 interval_ms=50 polls='..R.input.polls..' queries='..R.input.queries) end
        for _,path in ipairs(keys(R.hooks)) do
            local h=R.hook_lookup[path:lower()]
            local now=os.time(); local elapsed=math.max(1,now-h.since)
            log('stats hook path='..path..' native='..(h.installed and 1 or 0)..' subscribers='..#h.subscribers
                ..' calls='..h.calls..' calls_per_second='..string.format('%.2f',h.calls/elapsed))
            h.calls=0; h.since=now
        end
        for _,id in ipairs(keys(R.packages)) do
            local p=R.packages[id]; local events,hooks=0,0
            for _,record in pairs(p.error_records) do summarize(p,record) end
            for _,list in pairs(R.events) do for _,s in ipairs(list) do if s.owner==p then events=events+1 end end end
            for _,h in pairs(R.hooks) do for _,s in ipairs(h.subscribers) do if s.owner==p then hooks=hooks+1 end end end
            local owned={key=0,notify=0,command=0}
            for kind,channels in pairs(R.channels or {}) do for _,channel in pairs(channels) do
                for _,s in ipairs(channel.subscribers) do if s.owner==p then owned[kind]=owned[kind]+1 end end
            end end
            log('stats package='..id..' status='..p.status..' hooks='..hooks..' events='..events
                ..' timers='..count(p.timers)..' errors='..p.errors..' engaged='..tostring(p.engaged)
                ..' keys='..owned.key..' notifications='..owned.notify..' commands='..owned.command)
        end
    end
    function R.package_list()
        if not R.is_healthy() then return {} end
        local list={{id='WitchItRuntime',version=R.version,protocol_version=2,required_on_all_players=true}}
        for _,id in ipairs(keys(R.packages)) do
            local p=R.packages[id]
            if p.active then list[#list+1]={id=id,version=p.manifest.version,protocol_version=p.manifest.protocol_version,
                required_on_all_players=p.manifest.required_on_all_players} end
        end
        return list
    end
    function R.package_mismatch(peer,expectedRevision) return PackageInventory.mismatch(peer,expectedRevision) end
    function R.command(args,device)
        local frame=enter_output(device);if not frame then return false end
        local ok,err=xpcall(function()
        local verb,id=args[1],args[2]
        if verb=='list' and #args==1 then
            for _,key in ipairs(keys(R.packages)) do
                local p=R.packages[key]
                log('list package='..key..' version='..p.manifest.version..' protocol='..p.manifest.protocol_version
                    ..' required_on_all_players='..tostring(p.manifest.required_on_all_players)..' status='..p.status
                    ..(p.reason and ' reason='..p.reason or ''))
            end
        elseif verb=='keys' and #args==1 then R.ensure_input().registry.report()
        elseif verb=='key' and id and #args==3 then R.ensure_input().registry.set(id,args[3])
        elseif verb=='stats' and #args==1 then R.stats()
        elseif verb=='disable' and id and #args==2 then R.disable(id)
        elseif verb=='enable' and id and #args==2 then R.enable(id)

        else log('usage: wir list | stats | keys | key <id> <chord|Disabled> | disable <id> | enable <id>') end
        end,traceback)
        if not ok and output_current(frame) then log('ERROR console: '..err) end
        local current=output_current(frame)
        leave_output(frame)
        return current==true
    end
    function R.with_output(device,fn,...)
        local frame=enter_output(device);if not frame then return end
        local result=table.pack(xpcall(fn,traceback,...))
        local current=output_current(frame)
        leave_output(frame)
        if not current then return end
        if not result[1] then error(result[2],0) end
        return table.unpack(result,2,result.n)
    end
    function R.resume_timers(p)
        if not p.engaged then return end
        local pending={}
        for token in pairs(p.timers) do if token.pending then pending[#pending+1]=token end end
        for _,token in ipairs(pending) do if p.engaged and token.alive and token.pending then
            token.pending=false; R.native_call('ExecuteInGameThread',function() if token.alive then token.resume() end end,0)
        end end
    end
    function R.set_context(context)
        Resources.validate_context(context)
        R.context=Resources.clone_context(context)
        for _,p in pairs(R.packages) do if p.active and p.gate then
            local ok,engaged=call(p,'activation gate',p.gate,nil,Resources.clone_context(R.context))
            local before=p.engaged
            p.engaged=ok and engaged==true
            if before~=p.engaged then
                log('INFO context gate package='..p.id..' engaged='..tostring(p.engaged)
                    ..' reason='..(ok and 'context refresh' or 'predicate error')..' mode='..tostring(R.context.mode or 'unset'))
            end
            R.resume_timers(p)
        end end
        R.emit('context_changed',Resources.clone_context(R.context))
    end
    detach_package=function(p)
        disableOperations[p]=nil -- revoke the old transaction before dropping captures
        -- These are runtime-created plain records. Never call cancellation,
        -- rollback or package-provided abandonment callbacks after a fault.
        for token in pairs(p.subscriptions) do
            if type(token)=='table' then
                token.alive=false;token.fn=nil;token.class_filter=nil
                token.abandonCleanup=nil;token.abandon=nil
            end
        end
        for token in pairs(p.timers) do
            if type(token)=='table' then token.alive=false;token.fn=nil;token.pending=nil end
        end
        p.subscriptions={};p.timers={};p.gate=nil;p.api=nil;p.env=nil;p.storage=nil
        p.cleanup_pending=nil;p.cleanup_errors=nil;p.disabling=nil
    end
    local function abandon_runtime()
        disableOperations={} -- pure revocation precedes every terminal bookkeeping step
        OwnerRetirement.clear() -- pure; no retiring publisher after terminal/unload
        HookState.clear()
        MapDocument.clear('runtime terminal/unload',nil,true)
        PackageInventory.clear() -- pure terminal retirement; no inventory callbacks
        R.exports.clear();R.shared={}
        ScalarState.clear('runtime terminal/unload') -- R.alive=false: no subscriber delivery
        -- Drop pending native handles and input captures without user callbacks.
        local N=R.notifications
        N.epoch=N.epoch+1;N.queue={};N.head=1;N.paused=true
        R.input_owner=nil -- pure retirement; never invoke input.clear after fault
        if R.input then
            local I=R.input
            if I.capturing then
                I.capturing.fn=nil;I.capturing.interested=nil;I.capturing.finished=nil
                I.capturing.down=nil;I.capturing=nil
            end
            I.controller=nil;I.controller_name=nil;I.muted=true;I.bindings={}
        end
        for _,p in pairs(R.packages) do detach_package(p) end
        R.events={}
        for _,h in pairs(R.hooks) do h.subscribers={} end
        for _,channels in pairs(R.channels or {}) do
            for _,channel in pairs(channels) do channel.subscribers={} end
        end
    end
    function R.terminate(reason)
        if R.health_state=='terminal' then return end
        R.health_state='terminal';R.fault_reason=reason;R.alive=false;revoke_output()
        for _,p in pairs(R.packages) do
            p.active=false;p.engaged=false;p.status='faulted';p.reason=reason
        end
        abandon_runtime()
        R.cleanup_outcome='abandoned unsafe native boundary'
    end
    function R.shutdown()
        if R.unload_started then return end
        R.unload_started=true
        -- OnUnload is delivered synchronously on UE4SS's control thread too.
        -- The root decision precedes input/package/native cleanup. Off-GT only
        -- revoke plain Lua records: even package abandonment callbacks are not
        -- trusted with UObject access after native consumers have stopped.
        if not on_game_thread() then
            R.alive=false;revoke_output()
            for _,p in pairs(R.packages)do p.active=false;p.engaged=false;p.status='disabled';p.reason='off-game-thread unload' end
            abandon_runtime()
            R.health_state='stopped';R.cleanup_outcome='abandoned off-game-thread unload';R.unload_completed=true
            log('runtime stopped: off-game-thread unload; Lua references abandoned')
            return
        end
        if R.health_state=='terminal' or R.health_state=='stopped' then
            R.unload_completed=true;return
        end
        R.health_state='stopping'
        R.alive=false;revoke_output()
        local lane=teardown_lane()
        local function proceeding()
            if lane and lane() then return true end
            if R.health_state~='terminal' and R.health_state~='stopped' then
                -- A changed lifecycle cannot authorize another rollback or
                -- Native removal. Drop captures through the pure lane instead.
                for _,p in pairs(R.packages) do
                    p.active=false;p.engaged=false;p.status='disabled';p.reason='runtime unload lifecycle changed'
                end
                abandon_runtime()
                R.health_state='stopped';R.cleanup_outcome='abandoned changed runtime lifecycle'
            end
            R.unload_completed=true
            return false
        end
        R.notifications.reset('runtime unload')
        if not proceeding() then return end
        if R.input then
            R.input.clear()
            if not proceeding() then return end
        end
        for _,id in ipairs(keys(R.packages)) do
            local p=R.packages[id]
            if disableOperations[p] then
                -- A nested unload supersedes the already-running disable.
                -- Never invoke that owner's callback a second time.
                abandon_disable(disableOperations[p])
            elseif p.active or p.cleanup_pending then R.disable(id,'runtime unload',true) end
            if not proceeding() then return end
        end
        R.abandon_pending('runtime unload')
        if not proceeding() then return end
        local nativeIncomplete=false
        for _,path in ipairs(keys(R.hooks)) do
            if not proceeding() then return end
            uninstall(R.hooks[path],lane)
            if not proceeding() then return end
            if R.hooks[path].installed then nativeIncomplete=true end
        end
        R.exports.clear()
        if not proceeding() then return end
        R.health_state='stopped'
        R.cleanup_outcome=nativeIncomplete and 'native cleanup incomplete' or 'completed'
        log('runtime stopped')
        R.unload_completed=true
    end
    local packageOverrides={}
    function R.package_enabled(p)
        if CORE_COMPONENTS[p.id] then return true end
        local value=packageOverrides[p.id]
        if value==nil then return p.manifest.enabled end
        return value
    end
    function R.mods_list()
        local list={}
        for _,id in ipairs(keys(R.packages)) do
            local p=R.packages[id]
            list[#list+1]={id=id,name=p.manifest.name or id,version=p.manifest.version,
                core_component=CORE_COMPONENTS[id]==true,description=p.manifest.description or '',state=p.status=='faulted' and 'failed' or p.active and 'loaded' or
                    (p.failed_start or p.status=='blocked') and 'failed' or 'disabled',
                error=p.last_error or p.reason,enabled=R.package_enabled(p),
                restart_required=R.package_enabled(p)~=p.active}
        end
        return list
    end
    function R.set_package_enabled(id,value)
        if CORE_COMPONENTS[id] then log(id..': core component, cannot be disabled');return false end
        assert(R.packages[id],'unknown package'); assert(type(value)=='boolean','enabled must be boolean')
        local nextValues={}; for k,v in pairs(packageOverrides) do nextValues[k]=v end
        nextValues[id]=value
        R.storage().write_json('packages.json',{version=1,enabled=nextValues})
        packageOverrides=nextValues; R.emit('packages_changed')
    end
    function R.preflight()
        if R.preflight_done then return true end
        assert(R.is_healthy(),'runtime preflight unhealthy')
        local saved=R.storage().read_json('packages.json')
        if saved then
            assert(saved.version==1 and type(saved.enabled)=='table','invalid packages.json')
            for id,value in pairs(saved.enabled) do
                assert(type(id)=='string' and type(value)=='boolean','invalid package switch')
                packageOverrides[id]=value
            end
        end
        local root=R.storage().root
        local dirs={}
        for _,id in ipairs(keys(R.packages)) do
            if R.package_enabled(R.packages[id]) then dirs[#dirs+1]=root..'/'..id end
        end
        -- Legacy scalar bridge remains readable by older FAPP installs.
        dirs[#dirs+1]=root:gsub('[/\\]WitchItRuntime$','')..'/MatchRulesPlusPlus'
        if options.mkdir then for _,path in ipairs(dirs) do options.mkdir(path) end
        else Store.provision(dirs,true) end
        R.preflight_done=true;return true
    end
    function R.start()
        if R.started or not R.is_healthy() then return end
        R.preflight()
        log('starting api=2 packages='..count(R.packages)..' error_threshold='..maxErrors)
        for _,id in ipairs(keys(R.packages)) do
            if R.package_enabled(R.packages[id]) then R.enable(id,true) end
            if not R.is_healthy() then return false end
        end
        R.booting=false; R.started=true; R.emit('runtime_started')
        if R.input then R.input.registry.report() end
    end
    -- Only actual package writers enter this world-independent commit lane.
    -- Nested startup/dependent teardown and explicit rollback retries coalesce.
    PackageInventory.initialize()
    for _,name in ipairs({'enable','disable','abandon_pending','start'}) do
        local writer=R[name]
        R[name]=function(...) return PackageInventory.transaction(writer,...) end
    end
    return R
end
