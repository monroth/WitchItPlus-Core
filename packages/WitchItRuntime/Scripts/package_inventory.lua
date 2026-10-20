-- Runtime-owned committed package metadata. Pure Lua; no world or native driver.
local M={}
function M.new(R,subscribe,call,array,mismatch)
    local subscribers={}
    local committed,depth,draining,pending=nil,0,false,false
    local transactionFailed=false
    local B={}
    local function rows()
        local ids={};for id in pairs(R.packages) do ids[#ids+1]=id end;table.sort(ids)
        local packages={{id='WitchItRuntime',version=R.version,protocol_version=2,required_on_all_players=true}}
        local states={}
        for _,id in ipairs(ids) do
            local p=R.packages[id]
            local row={id=id,version=p.manifest.version,protocol_version=p.manifest.protocol_version,
                required_on_all_players=p.manifest.required_on_all_players}
            if p.active then packages[#packages+1]=row end
            states[#states+1]={id=id,active=p.active==true,status=p.status,
                cleanup_pending=p.cleanup_pending==true}
        end
        return {valid=true,packages=packages,states=states,started=R.started==true}
    end
    local function owners()
        local result={}
        for id,p in pairs(R.packages) do if p.active then result[id]=p.api end end
        return result
    end
    local function sameOwners(a,b)
        for id,A in pairs(a or {}) do if not b or b[id]~=A then return false end end
        for id,A in pairs(b or {}) do if not a or a[id]~=A then return false end end
        return true
    end
    local function coherent()
        for _,p in pairs(R.packages) do
            if p.disabling or p.active and (p.status~='active' or p.cleanup_pending)
                or not p.active and p.status=='active'
                or (p.cleanup_pending==true)~=(p.status=='cleanup_pending') then return false end
            if p.active then
                for id in pairs(p.manifest.dependencies) do
                    if not R.packages[id] or not R.packages[id].active then return false end
                end
            end
        end
        return true
    end
    local function equal(a,b)
        if type(a)~=type(b) then return false end
        if type(a)~='table' then return a==b end
        for k,v in pairs(a) do if not equal(v,b[k]) then return false end end
        for k in pairs(b) do if a[k]==nil then return false end end
        return true
    end
    local function copy(record)
        if not record then return nil end
        local result={revision=record.revision,valid=record.value.valid,started=record.value.started,packages={},states={}}
        for _,row in ipairs(record.value.packages) do
            result.packages[#result.packages+1]={id=row.id,version=row.version,
                protocol_version=row.protocol_version,required_on_all_players=row.required_on_all_players}
        end
        for _,row in ipairs(record.value.states) do
            result.states[#result.states+1]={id=row.id,active=row.active,status=row.status,
                cleanup_pending=row.cleanup_pending}
        end
        result.packages=array(result.packages);result.states=array(result.states)
        return result
    end
    local function invalidate()
        -- The original error has already been captured. Revoke the assertion
        -- first, without formatting/logging, reflection or subscriber entry.
        -- Terminal clear is stronger; never recreate a record after it.
        if committed and committed.value.valid then
            committed={revision=committed.revision+1,
                value={valid=false,packages={},states={},started=R.started==true},owners={}}
            pending=true
        end
    end
    local function failedDelivery(record)
        -- A callback (including its ordinary error report) owns the assertion
        -- it entered with. A nested writer may already have replaced it with
        -- a coherent commit; this older failure cannot revoke or poison that.
        if committed==record then transactionFailed=true;invalidate() end
    end
    local function current(s)
        return R.is_healthy() and s.alive and s.owner.active and s.owner.api==s.api
    end
    local function deliver(s,record,reason)
        if current(s) and s.revision~=record.revision then
            -- Mark BEFORE calling: reentrant commits cannot redeliver this revision.
            s.revision=record.revision
            call(s.owner,'package inventory '..reason,s.fn,s,copy(record),reason)
        end
    end
    local function drain()
        if draining or depth>0 or not R.is_healthy() then return true end
        draining=true
        local firstError,failed
        while pending and R.is_healthy() do
            pending=false
            local record=committed
            local snapshot={table.unpack(subscribers)}
            for _,s in ipairs(snapshot) do
                -- A nested writer supersedes the remaining old delivery. The
                -- older invalidation can never overwrite a newer valid commit.
                if committed~=record or not R.is_healthy() then break end
                local ok,err=pcall(deliver,s,record,record.value.valid and 'committed' or 'invalidated')
                if not ok then
                    if not failed then firstError=err;failed=true end
                    failedDelivery(record)
                    -- Repeated report failures for this invalid revision do not
                    -- rearm a delivery. Continue to the other owned listeners.
                end
            end
        end
        draining=false
        return not failed,firstError
    end
    function B.initialize()
        assert(not committed,'package inventory already initialized')
        committed={revision=0,value=rows(),owners=owners()}
    end
    local function commit()
        if not R.is_healthy() then return true end
        if transactionFailed or not coherent() then invalidate()
        else
            local value,currentOwners=rows(),owners()
            if not equal(value,committed.value) or not sameOwners(currentOwners,committed.owners) then
                committed={revision=committed.revision+1,value=value,owners=currentOwners}
                pending=true
            end
        end
        return drain()
    end
    function B.transaction(fn,...)
        if depth==0 then transactionFailed=false end
        depth=depth+1
        local result=table.pack(pcall(fn,...))
        depth=depth-1
        if not result[1] then
            transactionFailed=true
            invalidate() -- pure first, even when native health has also failed
        end
        local settled=table.pack(true,true)
        if depth==0 then
            settled=table.pack(pcall(commit))
            if not settled[1] then
                transactionFailed=true;invalidate()
                -- A secondary notification/report error must not mask the
                -- original writer/dispatch error or starve healthy listeners.
                drain()
            end
        end
        if not result[1] then error(result[2],0) end
        if not settled[1] then error(settled[2],0) end
        -- drain already invalidated its failing delivery. A later reentrant
        -- coherent writer may have superseded it; never revoke that newer commit
        -- while rethrowing the older notification error.
        if settled[2]==false then error(settled[3],0) end
        return table.unpack(result,2,result.n)
    end
    function B.mismatch(remote,expectedRevision)
        assert(R.is_healthy() and committed and committed.value.valid,'committed package inventory unavailable')
        assert(expectedRevision==nil or expectedRevision==committed.revision,'stale package inventory revision')
        return mismatch(copy(committed).packages,remote)
    end
    function B.attach(p,A)
        local function live() return R.is_healthy() and p.active and p.api==A end
        function A.packages.owner_current()
            return live() and committed~=nil and committed.value.valid
                and committed.owners~=nil and committed.owners[p.id]==A or false
        end
        function A.packages.mismatch(remote,expectedRevision)
            assert(A.packages.owner_current(),'package owner has no committed inventory')
            return B.mismatch(remote,expectedRevision)
        end
        function A.packages.snapshot()
            if not live() then return nil end
            return copy(committed)
        end
        function A.packages.is_current(revision)
            return live() and committed~=nil and committed.value.valid
                and type(revision)=='number' and revision==committed.revision or false
        end
        function A.packages.observe(fn)
            if not live() then return nil end
            assert(type(fn)=='function','package inventory callback must be function')
            local cancel,s=subscribe(p,subscribers,fn)
            s.api=A;s.ungated=true
            -- Subscribe first, then replay ONLY the last committed value. Even
            -- during startup the final outer commit follows with no mount gap.
            local record=committed
            local ok,err=pcall(deliver,s,record,'replay')
            if not ok then
                failedDelivery(record)
                drain() -- protected secondary failures; original error wins
                error(err,0)
            end
            return cancel
        end
    end
    function B.clear()
        committed=nil;pending=false;subscribers={}
    end
    return B
end
return M
