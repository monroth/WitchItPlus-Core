local Value=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'runtime.lua')
local M={}
function M.new(api,L,specs,options)
    options=options or {}
    local R={buckets={},metadata={},deliveries={},versions={},revision=0,scans=0,paths={},classes={},onChange={}}
    local persistent=options.persistent or {}
    local call=Value.call
    local unwrap=Value.unwrap
    local memberships={}
    -- An observed rename/reparent changes the fullname portion of L.key without
    -- creating another actor. Keep one registered identity per native address;
    -- never dereference the previous wrapper while retiring its old membership.
    local canonical={}
    local committedObservers={}
    local commits=0
    -- Additive completion lane. Existing onChange remains pre-invalidation and
    -- keeps changed_bucket filtering; observers receive scalar identity only.
    function R.onCommitted(callback)
        assert(type(callback)=='function','committed observer requires a callback')
        local record={callback=callback};committedObservers[#committedObservers+1]=record
        return function()
            record.callback=nil
            for i=#committedObservers,1,-1 do
                if committedObservers[i]==record then table.remove(committedObservers,i) end
            end
        end
    end
    local function committed(key,world)
        commits=commits+1
        local revision=commits
        local epoch=L.epoch
        local snapshot={}
        for i,record in ipairs(committedObservers) do snapshot[i]=record end
        for _,record in ipairs(snapshot) do
            if record.callback then record.callback(key,world,epoch,revision) end
        end
    end
    local function changed(bucket,object)
        R.revision=R.revision+1; R.versions[bucket]=R.versions[bucket]+1
        if not options.changed_bucket or options.changed_bucket==bucket then
            for _,callback in ipairs(R.onChange) do callback(object,bucket) end
        end
    end
    function R.track(object,bucket,ready)
        object=unwrap(object)
        if not L.alive(object) then return end
        local name=call(object,'GetFullName') or ''
        if name:find('Default__',1,true) or name:match('^Class ') or name:match('^BlueprintGeneratedClass ')
            or call(object,'IsClass')==true then return end
        local key=L.key(object); if not key then return end
        local class=name:match('^(%S+)')
        if not class or class=='Object' or class=='Actor' then
            class=call(call(call(object,'GetClass'),'GetFName'),'ToString')
        end
        local world=L.worldKey(object)
        if not persistent[class] and world and L.departed[world] and not L.transition then return end
        local targets=memberships[class]
        if not targets then
            targets={}; memberships[class]=targets
            if R.paths[class] then targets[class]=true end
            for short,path in pairs(R.paths) do
                if not targets[short] and (not options.inherited or options.inherited[short])
                    and call(object,'IsA',path)==true then targets[short]=true end
            end
        end
        if bucket then targets[bucket]=true end
        local address=key:match('^([^:]+):')
        local record=canonical[address]
        if record and record.key~=key then
            for short in pairs(record.buckets) do
                if R.buckets[short][record.key] then
                    R.buckets[short][record.key]=nil; R.metadata[short][record.key]=nil; changed(short)
                end
            end
            canonical[address]=nil; record=nil
        end
        local tracked=false
        for short in pairs(targets) do if R.buckets[short] then
            tracked=true; R.classes[class]=true
            if not record then record={key=key,buckets={}}; canonical[address]=record end
            record.buckets[short]=true
            local meta=R.metadata[short][key]
            if meta and bucket then meta.birth_at=meta.birth_at or os.clock() end
            if not meta or meta.world~=world or ready and not meta.ready then
                R.metadata[short][key]={name=name,world=world,ready=ready or meta and meta.ready,
                    delivered_at=meta and meta.delivered_at or os.clock(),ready_at=ready and os.clock() or meta and meta.ready_at,
                    birth_at=bucket and os.clock() or meta and meta.birth_at}
                changed(short,object)
            end
            R.buckets[short][key]=object
        end end
        if tracked then committed(key,world);return key end
    end
    function R.remove(object)
        object=unwrap(object)
        local address=call(object,'GetAddress'); local name=call(object,'GetFullName')
        if not address or not name then return end
        local key=tostring(address)..':'..name
        for short,bucket in pairs(R.buckets) do if bucket[key] then
            bucket[key]=nil; R.metadata[short][key]=nil; changed(short,object)
        end end
        -- A late retirement of a different fullname at a reused address must not
        -- remove its replacement's canonical registration.
        local record=canonical[tostring(address)]
        if record and record.key==key then canonical[tostring(address)]=nil end
    end
    function R.ready(object)
        local key=R.track(object,nil,true)
        if key and not L.transition then R.scannedWorld=L.worldKey(unwrap(object)) or R.scannedWorld end
        return key
    end
    function R.touch(object)
        local key=R.track(object); if not key then return end
        for short,bucket in pairs(R.buckets) do if bucket[key] then changed(short,object) end end
    end
    function R.list(short,include_metadata)
        local result={};local metadata=include_metadata and {}
        if L.transition then
            if metadata then return result,metadata end
            return result
        end
        local function append(object,key)
            result[#result+1]=object
            if metadata then metadata[#result]=R.metadata[short][key] end
        end
        for key,object in pairs(R.buckets[short] or {}) do
            local validate=options.validate_list
            if type(validate)=='function' then validate=validate() end
            if validate==false then append(object,key)
            elseif L.valid(object) then
                local name=call(object,'GetFullName') or ''
                if not name:find('Default__',1,true) and not name:match('^Class ')
                    and not name:match('^BlueprintGeneratedClass ') and call(object,'IsClass')~=true then
                    append(object,key)
                end
            end
        end
        if metadata then return result,metadata end
        return result
    end
    function R.reset(reason)
        for _,record in pairs(R.deliveries) do record.count=0;record.rejected=0;record.first=nil;record.last=nil end
        for short,bucket in pairs(R.buckets) do if not persistent[short] then
            for key in pairs(bucket) do
                local meta=R.metadata[short][key]
                if reason~='world identity changed' or not meta or not L.world or meta.world~=L.world then
                    bucket[key]=nil; R.metadata[short][key]=nil; changed(short)
                    local address=key:match('^([^:]+):'); local record=canonical[address]
                    if record and record.key==key then
                        record.buckets[short]=nil
                        if next(record.buckets)==nil then canonical[address]=nil end
                    end
                end
            end
        end end
        R.scannedWorld=nil; R.revision=R.revision+1
    end
    function R.eachExisting(context,visit)
        if not L.rawValid(context) then return false end
        for _,path in pairs(R.paths) do
            for _,object in ipairs(api.world_objects(path,context)) do visit(object) end
        end
        return true
    end
    function R.resync(context)
        if not L.rawValid(context) then context=WIR_RUNTIME and WIR_RUNTIME.input and WIR_RUNTIME.input.controller end
        if not R.eachExisting(context,R.track) then return false end
        R.scans=R.scans+1; R.scannedWorld=L.world; return true
    end
    for short,path in pairs(specs) do
        assert(type(short)=='string' and type(path)=='string','invalid registry spec')
        R.paths[short]=path; R.classes[short]=true
        R.buckets[short]={}; R.metadata[short]={}; R.versions[short]=0
        local deliveries={count=0,rejected=0};R.deliveries[short]=deliveries
        api.notify(path,function(object)
            local now=os.clock();deliveries.count=deliveries.count+1;deliveries.first=deliveries.first or now;deliveries.last=now
            if not R.track(object,short) then deliveries.rejected=deliveries.rejected+1 end
        end)
    end
    L.onReset(R.reset); L.onRetired(R.remove)
    if options.ready_events~=false then
        api.on('begin_play_post',R.ready,{class_filter=function(name) return R.classes[name]==true end})
    end
    return R
end
return M
