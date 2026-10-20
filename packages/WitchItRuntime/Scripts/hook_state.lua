-- Literal installed-hook state for the current API's subscriptions.
-- refresh is finite and uses the existing installer; no discovery loop or timer.
local M={}
function M.new(R,subscribe,call,install)
    local watchers={}
    local revision,record,pending,draining=0,{},false,false
    local B={}
    local function live(p,A) return R.is_healthy() and p.active and p.api==A end
    local function paths(value)
        assert(type(value)=='table' and getmetatable(value)==nil,'hook paths must be a plain array')
        local copy,seen={},{}
        for i,path in ipairs(value) do
            assert(i<=32 and type(path)=='string' and #path<=1024 and not path:find('[%c]') and path:match('^/.+:.+$'),'invalid hook path')
            local key=path:lower();assert(not seen[key],'duplicate hook path');seen[key]=true
            copy[i]=path
        end
        assert(#copy>0,'hook paths required')
        for key in pairs(value) do assert(type(key)=='number' and key%1==0 and key>=1 and key<=#copy,'hook paths must be dense') end
        return copy
    end
    local function snapshot(p,A,list)
        if not live(p,A) then return nil end
        local result={revision=revision,ready=true,healthy=true,records={}}
        for _,path in ipairs(list) do
            local h=R.hook_lookup[path:lower()]
            local subscribed=false
            if h then for _,s in ipairs(h.subscribers) do
                if s.alive and s.owner==p and s.api==A then subscribed=true;break end
            end end
            local installed=h and h.installed==true or false
            local status=not subscribed and 'unsubscribed' or installed and 'installed'
                or h.failed~=nil and 'failed' or h.waiting and 'waiting' or 'pending'
            result.records[#result.records+1]={path=path,installed=installed,subscribed=subscribed,status=status}
            result.ready=result.ready and installed and subscribed
        end
        return result
    end
    local function deliver(s,ref,reason)
        if not s.alive or not live(s.owner,s.api) or s.revision==revision or record~=ref then return end
        local value=snapshot(s.owner,s.api,s.paths)
        s.revision=revision -- before callback; reentry cannot redeliver the old commit
        call(s.owner,'hook state '..reason,s.fn,s,value,reason)
    end
    local function drain(reason)
        if draining or not R.is_healthy() then return end
        draining=true
        local ok,err=pcall(function()
            while pending and R.is_healthy() do
                pending=false
                local ref=record
                for _,s in ipairs({table.unpack(watchers)}) do
                    if record~=ref or not R.is_healthy() then break end
                    deliver(s,ref,reason)
                end
            end
        end)
        draining=false
        if not ok then error(err,0) end
    end
    function B.changed(reason,quiet)
        if not R.is_healthy() then return end
        revision=revision+1;record={};pending=true
        if not quiet then drain(reason or 'changed') end
    end
    function B.flush(reason)
        drain(reason or 'changed')
    end
    function B.clear()
        -- Terminal/unload lane: never enter arbitrary subscriber code.
        revision=revision+1;record={};watchers={};pending=false
    end
    function B.attach(p,A)
        A.hooks={}
        function A.hooks.snapshot(value)
            if not live(p,A) then return nil end
            return snapshot(p,A,paths(value))
        end
        function A.hooks.observe(value,fn)
            if not live(p,A) then return nil end
            assert(type(fn)=='function','hook state callback required')
            local list=paths(value)
            local cancel,s=subscribe(p,watchers,fn)
            s.api=A;s.paths=list;s.ungated=true
            deliver(s,record,'replay') -- subscription exists before the initial snapshot
            return cancel
        end
        function A.hooks.refresh(value)
            if not live(p,A) then return nil end
            local list=paths(value)
            for _,path in ipairs(list) do
                if not live(p,A) then return nil end
                local h=R.hook_lookup[path:lower()]
                local owned=false
                if h then for _,s in ipairs(h.subscribers) do
                    if s.alive and s.owner==p and s.api==A then owned=true;break end
                end end
                if owned then install(h) end
            end
            return snapshot(p,A,list)
        end
    end
    return B
end
return M
