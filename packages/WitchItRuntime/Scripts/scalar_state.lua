-- Retained world-owned scalar notifications. No timer, UObject or native API.
local M={}
local function clone(value)
    if value==nil then return nil end
    assert(type(value)=='table' and getmetatable(value)==nil,'state must be a plain flat scalar table')
    local result,count={},0
    for key,v in pairs(value) do
        count=count+1
        assert(count<=64 and type(key)=='string' and #key>0 and #key<=128,'invalid state field')
        local kind=type(v)
        assert(kind=='boolean' or kind=='string' and #v<=4096
            or kind=='number' and v==v and math.abs(v)<math.huge,'state fields must be finite scalars')
        result[key]=v
    end
    return result
end
function M.new(R,subscribe,call)
    local topics={}
    local clearDepth=0
    local function topic(name)
        assert(type(name)=='string' and #name>0 and #name<=128,'invalid state topic')
        local t=topics[name]
        if not t then t={subscribers={}};topics[name]=t end
        return t
    end
    local function notify(name,t,record,value,reason)
        local snapshot={table.unpack(t.subscribers)}
        for _,s in ipairs(snapshot) do
            -- A nested publication/revoke supersedes the outer delivery.
            if t.record~=record then break end
            if R.alive and s.alive and s.owner.active then
                call(s.owner,'state '..name,s.fn,s,clone(value),reason)
            end
        end
    end
    local function revoke(name,t,incarnation,reason)
        local old=t.record
        if not old or old.incarnation~=incarnation then return false end
        t.record=nil
        notify(name,t,nil,nil,reason)
        return true
    end
    local B={}
    function B.attach(p,A)
        local incarnation={}
        local function current()
            assert(R.alive and p.active and p.api==A,'state API owner is inactive or replaced')
        end
        function A.publish_state(name,value)
            current()
            local t=topic(name)
            if value==nil then return revoke(name,t,incarnation,'publisher revoked') end
            local copy=clone(value)
            assert(clearDepth==0 and not R.loading and not R.state_retired and copy.world_generation==R.world_generation,'stale state generation')
            assert(type(copy.revision)=='number' and copy.revision>=0 and copy.revision%1==0,'state revision required')
            local old=t.record
            assert(not old or old.incarnation==incarnation,'state topic has another owner')
            if old and copy.revision<=old.value.revision then return false end
            local record={incarnation=incarnation,value=copy}
            t.record=record
            notify(name,t,record,copy,'published')
            return true
        end
        function A.on_state(name,fn,options)
            current()
            local t=topic(name)
            local cancel,s=subscribe(p,t.subscribers,fn)
            s.ungated=true -- scalar revocation must reach an engaged/off consumer
            if options and options.replay then
                local record=t.record
                call(p,'state '..name..' replay',fn,s,clone(record and record.value),'replay')
            end
            return cancel
        end
        A.own(function()
            for name,t in pairs(topics) do revoke(name,t,incarnation,'publisher disabled') end
        end)
    end
    function B.clear(reason,beforeNotify)
        -- Drop every old world value before notifying anyone. A callback cannot
        -- see stale replay for a different topic during this boundary.
        assert(beforeNotify==nil or type(beforeNotify)=='function','internal boundary callback required')
        local changed={}
        clearDepth=clearDepth+1
        for name,t in pairs(topics) do
            if t.record then t.record=nil;changed[#changed+1]={name=name,topic=t} end
        end
        -- Native retirement can commit identity/epoch after all replay is
        -- invalidated, but before arbitrary public nil observers are delivered.
        if beforeNotify then
            local ok,err=pcall(beforeNotify)
            if not ok then clearDepth=clearDepth-1;error(err,0) end
        end
        for _,item in ipairs(changed) do notify(item.name,item.topic,nil,nil,reason) end
        clearDepth=clearDepth-1
    end
    return B
end
return M
