-- One data-only declaration registry; input.lua alone polls/dispatches its rows.
return function(R,log,names,store,reserved)
    local K={entries={},revision=0}; local overrides={}; local loaded=false
    local reverse={}; for code,name in pairs(names) do reverse[name:lower()]=code end
    local modNames={[16]='Shift',[17]='Ctrl',[18]='Alt'}
    local allowed={menu=true,lobby=true,match=true,creative=true}
    function K.parse(chord)
        assert(type(chord)=='string','chord must be a string')
        if chord:lower()=='disabled' then return false,{} end
        local parts={}; for part in chord:gmatch('[^+]+') do parts[#parts+1]=part end
        local mods,seen={},{}
        for i=1,#parts-1 do
            local code=({ctrl=17,control=17,shift=16,alt=18})[parts[i]:lower()]
            assert(code and not seen[code],'invalid chord modifiers'); seen[code]=true; mods[#mods+1]=code
        end
        table.sort(mods)
        local key=reverse[(parts[#parts] or ''):lower()]; assert(key,'unsupported chord key')
        return key,mods
    end
    function K.chord(key,mods)
        if key==false then return 'Disabled' end
        local parts={}; for _,code in ipairs(mods) do parts[#parts+1]=assert(modNames[code],'unsupported modifier') end
        parts[#parts+1]=assert(names[key],'unsupported key'); return table.concat(parts,'+')
    end
    local function load()
        if loaded then return end
        local s=store(); local saved=s.read_json('hotkeys.json')
        if saved then
            assert(saved.version==1 and type(saved.bindings)=='table','invalid hotkeys.json')
            for id,chord in pairs(saved.bindings) do
                assert(type(id)=='string','invalid hotkey id'); K.parse(chord); overrides[id]=chord
            end
        end
        -- User game rebindings may reserve additional keys. Reads only, no engine setters.
        local savedRoot=s.root:gsub('[/\\]WitchItRuntime$','')
        for _,platform in ipairs({'Windows','WindowsNoEditor'}) do
            local f=io.open(savedRoot..'/Config/'..platform..'/Input.ini','rb')
            if f then
                local data=f:read(262145); f:close(); assert(#data<=262144,'Input.ini too large')
                for line in data:gmatch('[^\r\n]+') do
                    if not line:match('^%s*%-') then
                        local key=line:match('Key=([^,)]+)') or line:match('ConsoleKeys=(%S+)')
                        if key then reserved[key]='user Input.ini binding' end
                    end
                end
            end
        end
        loaded=true
    end
    local function changed()
        K.revision=K.revision+1
        for _,e in pairs(K.entries) do e.conflict=nil end
        local chords={}
        for _,e in pairs(K.entries) do
            local key,mods=K.parse(overrides[e.id] or e.default_chord)
            e.key=key; e.mods=mods; e.chord=K.chord(key,mods)
            if key then
                if reserved[names[key]] then e.conflict='game: '..reserved[names[key]] end
                chords[e.chord]=chords[e.chord] or {}; table.insert(chords[e.chord],e)
            end
        end
        for chord,entries in pairs(chords) do
            if #entries>1 then
                local ids={}; for _,e in ipairs(entries) do ids[#ids+1]=e.id end; table.sort(ids)
                for _,e in ipairs(entries) do e.conflict=(e.conflict and e.conflict..'; ' or '')..'shared chord: '..table.concat(ids,', ') end
            end
        end
        R.emit('hotkeys_changed',K.revision)
        if R.started then K.report() end
    end
    function K.declare(package,spec)
        load()
        assert(type(spec)=='table' and type(spec.id)=='string' and spec.id:match('^[%w_.%-]+$'),'invalid hotkey id')
        assert(spec.package==nil or spec.package==package,'hotkey package mismatch')
        assert(type(spec.description)=='string' and #spec.description>0,'hotkey description required')
        local contexts=type(spec.context)=='string' and {spec.context} or spec.context
        assert(type(contexts)=='table' and #contexts>0,'hotkey context required')
        for _,ctx in ipairs(contexts) do assert(allowed[ctx],'invalid hotkey context') end
        local chord=spec.default_chord or spec.chord; local key,mods=K.parse(chord)
        local id=package..'.'..spec.id; local old=K.entries[id]
        local normalized=K.chord(key,mods)
        assert(not old or old.default_chord==normalized,'hotkey redeclared with different default')
        if not old then K.entries[id]={id=id,package=package,description=spec.description,context={table.unpack(contexts)},default_chord=normalized}; changed() end
        return K.entries[id]
    end
    function K.check(id,chord)
        assert(K.entries[id],'unknown hotkey: '..tostring(id))
        local key,mods=K.parse(chord); local normalized=K.chord(key,mods)
        if key and reserved[names[key]] then return false,'game: '..reserved[names[key]] end
        for other,e in pairs(K.entries) do
            if key and other~=id and e.chord==normalized then return false,'shared chord: '..other end
        end
        return true,normalized
    end
    function K.set(id,chord)
        load(); assert(K.entries[id],'unknown hotkey: '..tostring(id))
        local key,mods=K.parse(chord); local normalized=K.chord(key,mods)
        local after={}; for k,v in pairs(overrides) do after[k]=v end; after[id]=normalized
        store().write_json('hotkeys.json',{version=1,bindings=after})
        overrides=after; changed()
    end
    function K.reset(id)
        load(); assert(K.entries[id],'unknown hotkey')
        local after={}; for k,v in pairs(overrides) do if k~=id then after[k]=v end end
        store().write_json('hotkeys.json',{version=1,bindings=after}); overrides=after; changed()
    end
    function K.keys()
        local out={}; for _,name in pairs(names) do out[#out+1]=name end
        table.sort(out); return out
    end
    function K.list()
        local out={}
        for _,e in pairs(K.entries) do
            out[#out+1]={id=e.id,package=e.package,description=e.description,context={table.unpack(e.context)},
                default_chord=e.default_chord,chord=e.chord,conflict=e.conflict,enabled=e.key~=false and not e.conflict}
        end
        table.sort(out,function(a,b) return a.id<b.id end); return out
    end
    function K.report()
        local conflicts=0
        for _,e in ipairs(K.list()) do
            if e.conflict then conflicts=conflicts+1 end
            log('keys '..e.id..' '..e.chord..' ['..table.concat(e.context,',')..'] '..e.description
                ..(e.conflict and ' CONFLICT (disabled): '..e.conflict or ' applies now'))
        end
        log('keys declarations='..#K.list()..' conflicts='..conflicts..'; wir keys / wir key <id> <chord|Disabled>')
    end
    function K.interested(e)
        if not e.key or e.conflict then return false end
        local mode=R.context.hotkey_context or (R.input_context=='creative' and 'creative')
            or R.context.mode or R.input_context
        if mode=='objective' or mode=='seek' or mode=='mobification' or mode=='impostor' then mode='match' end
        -- Unknown producer: package's gameplay guards remain authoritative until birth.
        if mode==nil then return true end
        for _,ctx in ipairs(e.context) do if ctx==mode then return true end end
        return false
    end
    return K
end
