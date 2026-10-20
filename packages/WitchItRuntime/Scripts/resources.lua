-- Native registrations are runtime-owned; only subscriber closures belong to packages.
local Values=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'callback_values.lua')
local ukind=Values.kind
local M={hot={Tick=true,ReceiveTick=true,GetGameInstance=true,TickFillState=true,UpdateChatMessagesPerSecond=true}}
local function order(a,b)
    if a.priority~=b.priority then return a.priority<b.priority end
    if a.owner.id~=b.owner.id then return a.owner.id<b.owner.id end
    return a.sequence<b.sequence
end
local function same_value(a,b)
    local ta,tb=type(a),type(b)
    if ta~=tb then return false end
    if ta~='userdata' and ta~='table' then return a==b end
    -- UObject/FText/struct wrappers never participate in Lua equality.
    local function identity(o)
        if type(o)=='userdata' then
            local kind=ukind(o)
            if not ({UObject=true,AActor=true,UWorld=true,UClass=true,UFunction=true,UStruct=true,UEnum=true,UDataTable=true})[kind] then return end
        end
        local ok,key=pcall(function() return tostring(o:GetAddress())..':'..o:GetFullName() end)
        if ok then return key end
    end
    local ka,kb=identity(a),identity(b)
    return ka~=nil and ka==kb
end
function M.dispatch_hook(h,phase,call,log,...)
    local args=table.pack(...); local snapshot={table.unpack(h.subscribers)}
    table.sort(snapshot,order)
    local writes,chosen={},nil
    local function choose(key,value,s)
        local prior=writes[key]
        if prior and prior.owner~=s.owner and not same_value(prior.value,value) and not h.conflicts[phase..key] then
            h.conflicts[phase..key]=true
            log('override conflict path='..h.path..' phase='..phase..' field='..key
                ..' packages='..prior.owner.id..','..s.owner.id..' winner='..s.owner.id)
        end
        writes[key]={value=value,owner=s.owner,subscriber=s}
    end
    for _,s in ipairs(snapshot) do
        if s.alive and s.phase==phase and s.owner.active and (s.ungated or s.owner.engaged) then
            local argv={table.unpack(args,1,args.n)}; local staged={}
            for index in pairs(s.outputs) do
                local raw=args[index]
                argv[index]={get=function()
                    if staged[index] then return staged[index].value end
                    local prior=writes[tostring(index)]
                    if prior then return prior.value end
                    return raw:get()
                end,set=function(_,value) staged[index]={value=value} end}
            end
            local ok,value=call(s.owner,'hook '..h.path,s.fn,s,table.unpack(argv,1,args.n))
            if ok and s.alive and s.owner.active then
                for index,write in pairs(staged) do choose(tostring(index),write.value,s) end
                if s.override and value~=nil then choose('return',value,s); chosen=true end
            end
        end
    end
    for index,write in pairs(writes) do
        if index~='return' and write.owner.active and write.subscriber.alive then
            call(write.owner,'hook out commit '..h.path,function() args[tonumber(index)]:set(write.value) end,write.subscriber)
        end
    end
    local result=writes['return']
    if chosen and result.owner.active and result.subscriber.alive then return result.value end
end
function M.validate_context(value,seen,depth)
    depth=depth or 0; assert(depth<32,'context too deep')
    local t=type(value)
    assert(t=='table' or t=='string' or t=='boolean' or t=='number','context must contain data only')
    if t=='number' then assert(value==value and math.abs(value)<math.huge,'nonfinite context') end
    if t=='table' then
        assert(not getmetatable(value),'context metatable forbidden')
        seen=seen or {}; assert(not seen[value],'cyclic context'); seen[value]=true
        for k,v in pairs(value) do
            assert(type(k)=='string' or type(k)=='number','invalid context key')
            M.validate_context(v,seen,depth+1)
        end
        seen[value]=nil
    end
end
function M.clone_context(value)
    if type(value)~='table' then return value end
    local out={}; for k,v in pairs(value) do out[k]=M.clone_context(v) end; return out
end
function M.api(R,p,A,subscribe,call,log,options,reportCommand)
    local dir=assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))
    -- Registrations detach before owned rollback, even when that rollback must
    -- retain its Native scope for ordinary reflected calls on a later retry.
    local function nativeSubscription(cancel)
        local token={resource=p.id..'/Native subscription'}
        local function stop()
            if not p.subscriptions[token] then return true end
            local done,why=cancel()
            if done==false then return false,token.resource..': '..tostring(why or 'cancellation refused') end
            p.subscriptions[token]=nil;return true
        end
        p.subscriptions[token]=stop
        return stop
    end
    A.Native=dofile(dir..'native_primitives.lua').new(R,A,nativeSubscription)
    if p.id=='Core' then
        function A.native_boundaries(save,load)
            return dofile(dir..'native_files.lua').install(R,p,A,call,log,save,load)
        end
    end
    function A.native_ship_parts()
        assert(p.id=='BaseWorldPlus','ship binding is private to Editor tweaks')
        local function owner(mode)
            if R.loading or R.state_retired
                or R.packages[p.id]~=p or p.api~=A then return false end
            return mode=='edit' and R.is_healthy() and p.active or mode=='cleanup'
                and (R.is_healthy() or R.health_state=='stopping' and R.unload_started==true)
        end
        if not owner('edit') then return nil end
        local factory=rawget(_G,'WitchItNativeShipPartsBridge');if type(factory)~='function' then return nil end
        local raw=R.diagnostics.access(p.id,'ShipParts.Native.Bridge',factory)
        if not owner('edit') then return nil end
        local bound
        local function storage()
            if not owner('edit') then return nil end
            if bound and (bound.operation_current and bound.operation_current()==true or bound.current()==true) then return bound end
            local Core=R.shared.Core;local service=Core and Core.capabilities.get('native_scalar_storage')
            if service and (service.operation_current and service.operation_current()==true or service.current()==true) then bound=service.attach(A,function()return owner('edit')end);return bound end
        end
        return raw,storage,owner
    end
    A.native_access=function(name,fn,...)
        assert(R.health_state~='terminal' and R.health_state~='stopped','runtime native boundary is unhealthy')
        return R.diagnostics.access(p.id,name,fn,...)
    end
    local Store=R.store_module
    -- Cleanup is synchronous. False/throw keeps only this rollback closure for
    -- bounded disable retries; native world teardown abandons it without replay.
    function A.own(fn,abandonCleanup,name)
        assert(p.active and type(fn)=='function','cleanup callback required')
        assert(abandonCleanup==nil or type(abandonCleanup)=='function','invalid cleanup abandonment')
        local info=debug.getinfo(fn,'S')
        local resource=p.id..'/'..(name or ((info.short_src or info.source)..':'..info.linedefined))
        p.cleanup_sequence=(p.cleanup_sequence or 0)+1
        local token={fn=fn,abandonCleanup=abandonCleanup,resource=resource,cleanup_order=p.cleanup_sequence}
        local function cancel()
            local callback=token.fn
            if not callback then return true end
            if token.running then return false,resource..': cleanup already running' end
            token.running=true
            local ok,result,reason=pcall(callback)
            token.running=nil
            if not ok then error(resource..': '..tostring(result),0) end
            if result==false then return false,resource..': '..tostring(reason or 'callback returned false without a reason') end
            token.fn=nil; token.abandonCleanup=nil; p.subscriptions[token]=nil
            return true
        end
        token.abandon=function()
            local abandon=token.abandonCleanup
            token.fn=nil; token.abandonCleanup=nil; p.subscriptions[token]=nil
            if abandon then abandon() end
            return true
        end
        p.subscriptions[token]=cancel
        return cancel
    end
    local function owned_channel(kind,key,register,fn,opts)
        R.channels=R.channels or {}; R.channels[kind]=R.channels[kind] or {}
        local channels=R.channels[kind]; local c=channels[key]
        if not c then c={subscribers={}}; channels[key]=c end
        local cancel,s=subscribe(p,c.subscribers,fn)
        s.priority=opts and opts.priority or 0
        s.ungated=opts and opts.ungated==true
        if kind=='command' then s.report_error=assert(reportCommand) end
        assert(type(s.priority)=='number' and s.priority==s.priority and math.abs(s.priority)<math.huge,'invalid priority')
        if not c.installed then
            local function dispatch(args,snapshot)
                if not R.alive then return kind=='command' and false or nil end
                table.sort(snapshot,order)
                local handled=false
                for _,entry in ipairs(snapshot) do
                    if entry.alive and entry.owner.active and (kind=='command' or entry.ungated or entry.owner.engaged) then
                        local ok,result=call(entry.owner,kind..' '..key,entry.fn,entry,table.unpack(args,1,args.n))
                        if kind=='notify' and ok and result==true then entry.cancel() end
                        if kind=='command' and ok and result==true then handled=true end
                    end
                end
                if kind=='command' then return handled end
                -- Never return true to native NotifyOnNewObject: it would remove the shared channel.
            end
            register(function(...)
                if kind=='notify' then
                    -- Native construction stack: retain only the raw handle and
                    -- declared class. No package code or UObject method runs here.
                    if R.alive then R.notifications.enqueue(key,select(1,...),c.subscribers) end
                    return
                end
                return dispatch(table.pack(...),{table.unpack(c.subscribers)})
            end,c)
            c.installed=true
        end
        s.cancel=cancel
        return cancel
    end
    function A.hotkey(spec,fn)
        assert(type(fn)=='function','hotkey callback required')
        local input=R.ensure_input(); local entry=input.registry.declare(p.id,spec)
        return owned_channel('key',entry.id,function(dispatch,channel)
            input.bind(entry,dispatch,function()
                for _,subscriber in ipairs(channel.subscribers) do
                    if subscriber.alive and subscriber.owner.active and (subscriber.ungated or subscriber.owner.engaged) then return true end
                end
                return false
            end)
        end,fn,{ungated=spec.ungated==true})
    end
    -- Compatibility is declaration, never a second input path.
    function A.keybind(key,modifiers,fn)
        if type(modifiers)=='function' then fn=modifiers; modifiers={} end
        local mods={}; local seen={}
        for _,v in ipairs(modifiers or {}) do if not seen[v] then mods[#mods+1]=v; seen[v]=true end end
        table.sort(mods)
        local registry=R.ensure_input().registry
        local chord=registry.chord(key,mods)
        return A.hotkey({id='legacy_'..key..'_'..table.concat(mods,'_'),default_chord=chord,
            description='Shortcut ('..chord..')',context={'menu','lobby','match','creative'}},fn)
    end
    A.packages={list=function() return Store.json.array(R.package_list()) end,mismatch=R.package_mismatch}
    A.mods={list=R.mods_list,set_enabled=R.set_package_enabled}
    A.hotkeys={keys=function() return R.ensure_input().registry.keys() end,list=function() return R.ensure_input().registry.list() end,
        set=function(id,chord) return R.ensure_input().registry.set(id,chord) end,
        reset=function(id) return R.ensure_input().registry.reset(id) end,
        check=function(id,chord) return R.ensure_input().registry.check(id,chord) end,
        capture=function(fn,interested)
            assert(type(fn)=='function' and type(interested)=='function','capture callbacks required')
            local id,alive,cancelInput={},true,nil
            local function dispose()
                if not alive then return end; alive=false
                p.subscriptions[id]=nil
                local cancel=cancelInput; cancelInput=nil
                if cancel then cancel() end
            end
            cancelInput=R.ensure_input().capture(function(chord) call(p,'key capture',fn,nil,chord) end,
                function()
                    if not p.active or not p.engaged then return false end
                    local ok,wants=call(p,'key capture interest',interested,nil)
                    return ok and wants==true
                end,dispose)
            p.subscriptions[id]=dispose
            return dispose
        end}
    function A.notify(class,fn,opts)
        assert(type(class)=='string' and class:match('^/[^%s]+%.[^%s]+$'),'invalid class path')
        return owned_channel('notify',class:lower(),function(dispatch) R.native_call('NotifyOnNewObject',class,dispatch) end,fn,opts)
    end
    function A.command(name,fn,opts)
        assert(type(name)=='string' and name:match('^[%w_%-]+$') and name:lower()~='wir','invalid/reserved command')
        return owned_channel('command',name:lower(),function(dispatch)
            R.native_call('RegisterConsoleCommandHandler',name:lower(),function(full,args,device)
                return R.with_output(device,dispatch,full,args,device)
            end)
        end,fn,opts)
    end
    function A.game_thread(fn)
        assert(p.active and type(fn)=='function','invalid game-thread callback')
        local token={fn=fn}; local function cancel()
            if not token.fn then return end
            R.diagnostics.lua_cancel(p.id,'game_thread')
            token.fn=nil; p.timers[token]=nil
        end
        token.cancel=cancel
        p.timers[token]=cancel
        R.native_call('ExecuteInGameThread',function()
            local callback=token.fn; cancel()
            if callback and R.alive and p.active and p.engaged then call(p,'game_thread',callback,token) end
        end,0)
        return cancel
    end
    function A.find_all(class)
        assert(p.active and p.engaged,'package inactive in this context')
        assert(type(class)=='string','invalid class')
        return R.native_call('FindAllOf',class) or {}
    end
    local worldObjects=dofile(dir..'world_objects.lua').new(R.native_call)
    function A.world_objects(class,context)
        assert(p.active and p.engaged,'package inactive in this context')
        return worldObjects(class,context)
    end
    function A.gate(predicate)
        assert(p.active and type(predicate)=='function','invalid activation predicate')
        p.gate=predicate
        local ok,result=call(p,'activation gate',predicate,nil,M.clone_context(R.context))
        local before=p.engaged
        p.engaged=ok and result==true
        if before~=p.engaged then log('INFO context gate package='..p.id..' engaged='..tostring(p.engaged)..' reason=gate declaration') end
        R.resume_timers(p)
    end
    function A.is_active() return R.is_healthy() and p.active and p.engaged end
    function A.context()
        local context=Store.clone(R.context)
        context.world_generation=R.world_generation
        return context
    end
    function A.publish(value)
        assert(p.active and type(value)=='table','shared API must be a table')
        R.shared[p.id]=R.exports.publish(p.id,value)
    end
    function A.shared(id)
        assert(R.is_healthy(),'runtime native boundary is unhealthy')
        assert(p.active and p.manifest.dependencies[id],'shared access requires a declared dependency')
        assert(R.packages[id].active and R.shared[id],'dependency has no shared API: '..id)
        return R.shared[id]
    end
    function A.profile(label,fn,...) return R.profile(p,label,fn,...) end
    local modules={}
    function A.module(name)
        assert(p.active and type(name)=='string' and name:match('^[%w_%-]+$'),'invalid module name')
        if modules[name]==nil then modules[name]=A.profile('module:'..name,function() return assert(loadfile(p.path..'/'..name..'.lua','t',p.env))() end) end
        return modules[name]
    end
    function A.storage()
        assert(p.active,'package inactive')
        if not p.storage then
            local root=options.saved_root
            if not root then
                local lib=R.native_call('StaticFindObject','/Script/Engine.Default__KismetSystemLibrary')
                assert(lib:IsValid(),'KismetSystemLibrary unavailable')
                local saved=lib:GetProjectSavedDirectory()
                root=(type(saved)=='string' and saved or saved:ToString()):gsub('[/\\]+$','')..'/WitchItRuntime'
            end
            p.storage=Store.new(root..'/'..p.id,R.mkdir)
        end
        return p.storage
    end
    function A.config(defaults)
        local values=Store.clone(defaults or {})
        local f=io.open(p.path..'/config.json','rb')
        if f then local bytes=f:read(262145); f:close(); Store.merge(values,Store.json.decode(bytes)) end
        local user=A.storage().read_json('settings.json')
        if user then Store.merge(values,user) end
        return values
    end
    function A.save_settings(values) return A.storage().write_json('settings.json',values) end
    A.json=Store.json
    -- The issuer registers its revocation through A.own, so it is built last.
end
return M
