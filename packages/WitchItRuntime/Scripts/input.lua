-- UE4SS RegisterKeyBind enters Lua on the mod/input OS thread. Poll native
-- PlayerController input on the game thread instead; no input-thread closure.
return function(R,log)
    local I={controller=nil,bindings={},polls=0,queries=0,muted=false}
    local names={[1]='LeftMouseButton',[2]='RightMouseButton',[4]='MiddleMouseButton',
        [8]='BackSpace',[9]='Tab',[13]='Enter',[19]='Pause',[20]='CapsLock',[27]='Escape',
        [32]='SpaceBar',[33]='PageUp',[34]='PageDown',[35]='End',[36]='Home',
        [37]='Left',[38]='Up',[39]='Right',[40]='Down',[45]='Insert',[46]='Delete',
        [160]='LeftShift',[161]='RightShift',[162]='LeftControl',[163]='RightControl',
        [164]='LeftAlt',[165]='RightAlt',[186]='Semicolon',[187]='Equals',[188]='Comma',
        [189]='Hyphen',[190]='Period',[191]='Slash',[192]='Tilde',[219]='LeftBracket',
        [220]='Backslash',[221]='RightBracket',[222]='Apostrophe'}
    for code=48,57 do names[code]=({'Zero','One','Two','Three','Four','Five','Six','Seven','Eight','Nine'})[code-47] end
    for code=65,90 do names[code]=string.char(code) end
    for code=112,135 do names[code]='F'..(code-111) end
    local dir=debug.getinfo(1,'S').source:match('^@(.+[\\/])')
    I.registry=dofile(dir..'hotkeys.lua')(R,log,names,R.storage,dofile(dir..'reserved_keys.lua'))
    local modifiers={[16]={'LeftShift','RightShift'},[17]={'LeftControl','RightControl'},[18]={'LeftAlt','RightAlt'}}
    local keyRecords={}
    local function record(name)
        if not keyRecords[name] then keyRecords[name]={KeyName=FName(name)} end
        return keyRecords[name]
    end
    local function alternatives(code)
        local result={}
        local choices=modifiers[code]
        if not choices then local name=assert(names[code],'unsupported game-thread key code: '..code); choices={name} end
        for _,name in ipairs(choices) do
            result[#result+1]=record(name)
        end
        return result
    end
    function I.capture(fn,interested,finished)
        if I.capturing then I.capturing.cancel() end
        local c={fn=fn,interested=interested,finished=finished,down={},armed=false}
        function c.cancel()
            if not c.fn then return end
            if I.capturing==c then I.capturing=nil end
            local done=c.finished
            c.fn=nil; c.interested=nil; c.finished=nil; c.down=nil
            if done then done() end
        end
        I.capturing=c
        return c.cancel
    end
    function I.clear()
        if I.capturing then I.capturing.cancel() end
        I.controller=nil; I.controller_name=nil; I.muted=true
        for _,b in ipairs(I.bindings) do b.down=false end
    end
    function I.observe(actor,guard)
        local function allowed() return not guard or guard() end
        if I.muted or not allowed() then return end
        local ok,full=pcall(function()
            if not allowed() then return end
            local valid=actor:IsValid();if not allowed() or not valid then return end
            local localPlayer=actor:IsLocalPlayerController();if not allowed() or not localPlayer then return end
            local world=actor:GetWorld();if not allowed() or not world then return end
            local worldValid=world:IsValid();if not allowed() or not worldValid then return end
            local address=world:GetAddress();if not allowed() then return end
            local name=world:GetFullName();if not allowed() then return end
            if R.world_key and tostring(address)..'|'..name~=R.world_key then return end
            local actorName=actor:GetFullName();if not allowed() then return end
            return actorName
        end)
        if ok and full and allowed() and not I.muted then
            I.controller=actor
            R.input_context=full:find('MenuPlayerController',1,true) and 'menu'
                or full:find('CreativePlayerController',1,true) and 'creative' or 'match'
            if I.controller_name~=full then
                I.controller_name=full
                log('[WIR-INPUT] local controller='..full..' context='..R.input_context)
            end
        end
    end
    function I.refresh(destination)
        if destination then I.muted=false end
        if I.muted or not R.alive then return end
        local objects=R.native_call('FindAllOf','PlayerController') or {}
        for _,actor in ipairs(objects) do I.observe(actor) end
    end
    local function down(keys)
        for _,key in ipairs(keys) do
            I.queries=I.queries+1
            -- e3ba1016 push_structproperty accepts a reflected table input.
            -- FKey has one reflected field: FName KeyName. Exactly one argument.
            if I.controller:IsInputKeyDown(key) then return true end
        end
        return false
    end
    local keys
    local captureCodes={}
    -- Escape belongs to the native Back route; its PRE interceptor cancels UI capture.
    for code in pairs(names) do if code~=27 and (code<160 or code>165) then captureCodes[#captureCodes+1]=code end end
    table.sort(captureCodes)
    local function poll()
        if not R.alive then I.clear(); return end
        I.polls=I.polls+1
        local capture=I.capturing
        if capture then
            local interested,wants=pcall(capture.interested)
            if not interested or not wants then
                capture.cancel()
                if not interested then log('ERROR key capture interest: '..tostring(wants)) end
                return
            end
            if I.capturing~=capture then return end
            if not I.controller then return end
            local ok,err=pcall(function()
                if not I.controller:IsValid() then I.clear(); return end
                local mods={}
                for _,code in ipairs({16,17,18}) do if down(keys(code)) then mods[#mods+1]=code end end
                for _,code in ipairs(captureCodes) do
                    local pressed=down(keys(code))
                    if capture.armed and pressed and not capture.down[code] then
                        local callback=capture.fn
                        capture.cancel()
                        -- Suppress the captured press in package dispatch until release.
                        for _,b in ipairs(I.bindings) do b.down=true end
                        callback(I.registry.chord(code,mods)); return
                    end
                    capture.down[code]=pressed
                end
                capture.armed=true
            end)
            if not ok then capture.cancel(); log('ERROR key capture: '..tostring(err)) end
            return
        end
        local interested={}
        for _,b in ipairs(I.bindings) do if I.registry.interested(b.entry) and b.interested() then
            b.key=keys(b.entry.key); b.mods={}; b.excluded={}
            local requested={}
            for _,code in ipairs(b.entry.mods) do b.mods[#b.mods+1]=keys(code); requested[code]=true end
            for _,code in ipairs({16,17,18}) do if not requested[code] then b.excluded[#b.excluded+1]=keys(code) end end
            interested[b]=true
        else b.down=false end end
        if I.controller and next(interested) then
            local ok,err=pcall(function()
                if not I.controller:IsValid() then I.clear(); return end
                -- Cache each distinct key query within a poll (shared bindings).
                local states={}
                local function pressed(keys)
                    local state=states[keys]
                    if state==nil then state=down(keys); states[keys]=state end
                    return state
                end
                for _,b in ipairs(I.bindings) do
                    local keyDown=interested[b] and pressed(b.key) or false
                    local active=keyDown and not b.down
                    if active then
                        for _,mod in ipairs(b.mods) do active=pressed(mod) and active end
                        for _,mod in ipairs(b.excluded) do active=not pressed(mod) and active end
                    end
                    if active then
                        log('[WIR-INPUT] key dispatch '..b.entry.id..' '..b.entry.chord..' controller_context='..tostring(R.input_context)
                            ..' shared_mode='..tostring(R.context.mode))
                        b.dispatch()
                    end
                    b.down=keyDown
                end
            end)
            if not ok then I.clear(); log('ERROR game-thread input: '..tostring(err)) end
        end
    end
    local alternativesByCode={}
    keys=function(code)
        if not alternativesByCode[code] then alternativesByCode[code]=alternatives(code) end
        return alternativesByCode[code]
    end
    function I.bind(entry,dispatch,interested)
        local key,mods=entry.key,entry.mods
        local binding={entry=entry,key=key and keys(key) or {},mods={},excluded={},dispatch=dispatch,interested=interested,down=false}
        local requested={}
        for _,code in ipairs(mods) do binding.mods[#binding.mods+1]=keys(code); requested[code]=true end
        -- Native Input::Handler matches the entire Ctrl/Shift/Alt mask.
        for _,code in ipairs({16,17,18}) do
            if not requested[code] then binding.excluded[#binding.excluded+1]=keys(code) end
        end
        I.bindings[#I.bindings+1]=binding
        if #I.bindings==1 then I.refresh(); R.native_call('LoopInGameThreadWithDelay',50,poll) end
    end
    return I
end
