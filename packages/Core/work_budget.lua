-- Main-state resumable steps, instantiated per consumer. Native calls cannot be preempted.
local M={}
function M.new(api,options)
    options=options or {}
    local milliseconds=options.ms or 6
    local B={jobs={},keys={},ms=milliseconds,reserve=options.reserve or math.min(3,milliseconds/2),
        items=options.items or 512,epoch=0,overruns=0,pass=0}
    assert(B.ms>0 and B.reserve>=0 and B.reserve<B.ms and B.items>=1 and B.items%1==0,'invalid work budget')
    local active,wakeup,closed
    local function profile(key,milliseconds)
        if not closed and options.profile then options.profile(key,milliseconds) end
    end
    local function fail(job,value,prefix)
        -- Preserve opaque Lua failures without calling their metamethods. String
        -- errors retain the existing job label for ordinary diagnostics.
        if type(value)=='string' then
            local ok,label=pcall(tostring,job.label)
            error((prefix or 'work ')..(ok and label or '<unprintable>')..': '..value,0)
        end
        error(value,0)
    end
    function B.each(items,fn)
        local index=0
        return function()
            index=index+1
            if index<=#items then fn(items[index],index) end
            return index>=#items
        end
    end
    function B.sequence(steps)
        local index=1
        return function()
            if index>#steps then return true end
            local result=steps[index]()
            if type(result)=='table' and type(result.native)=='function' then return result end
            if result==true then index=index+1 end
            return index>#steps
        end
    end
    function B.defer(build)
        local step
        return function()
            if not step then step=build(); return false end
            return step()
        end
    end
    function B.finish(step)
        while true do
            local result=step()
            if result==true then return end
            if type(result)=='table' and type(result.native)=='function' then result.native() end
        end
    end
    function B.native(fn)
        local done=false
        local action={native=function() fn(); done=true end}
        return function() if done then return true end; return action end
    end
    function B.array(array,unwrap,fn)
        local count=array.GetArrayNum and array:GetArrayNum() or #array
        local index=0
        return function()
            index=index+1
            if index<=count then fn(unwrap(array[index]),index) end
            return index>=count
        end
    end
    function B.pending() return #B.jobs>0 end
    local schedule
    function B.submit(key,label,fn,persistent)
        if closed then return false end
        assert(type(key)=='string' and type(fn)=='function','job requires a string key and step function')
        if B.keys[key] then return false end
        local job={key=key,label=label,epoch=B.epoch,persistent=persistent,step=fn}
        B.keys[key]=job; B.jobs[#B.jobs+1]=job
        schedule(); return true
    end
    function B.cancel(key)
        B.keys[key]=nil
        for i=#B.jobs,1,-1 do if B.jobs[i].key==key then table.remove(B.jobs,i) end end
    end
    function B.drain()
        if closed or B.running then return end
        B.running=true; B.pass=B.pass+1
        local start=os.clock(); local count=0
        while #B.jobs>0 and count<B.items and (os.clock()-start)*1000<B.ms-B.reserve do
            local job=table.remove(B.jobs,1); active=job
            local stepStart=os.clock()
            local ok,done=pcall(function()
                if api.profile then return api.profile('work:'..tostring(job.label),job.step) end
                return job.step()
            end)
            if ok and type(done)=='table' and type(done.native)=='function' then
                if count>0 then
                    active=nil
                    if job.epoch==B.epoch and B.keys[job.key]==job then table.insert(B.jobs,1,job) end
                    break
                end
                local nativeStart=os.clock()
                if job.epoch==B.epoch and B.keys[job.key]==job then ok,done=pcall(done.native) end
                active=nil; B.running=false
                if job.epoch==B.epoch and B.keys[job.key]==job then
                    if not ok then B.keys[job.key]=nil else B.jobs[#B.jobs+1]=job end
                end
                if not ok then
                    pcall(schedule) -- secondary scheduling failure cannot mask owned error
                    pcall(profile,'work:map conversion',(os.clock()-nativeStart)*1000)
                    fail(job,done)
                end
                schedule()
                profile('work:map conversion',(os.clock()-nativeStart)*1000)
                return
            end
            active=nil; count=count+1
            if job.epoch==B.epoch and B.keys[job.key]==job then
                if not ok or done==true then B.keys[job.key]=nil
                else B.jobs[#B.jobs+1]=job end
            end
            if not ok then
                B.running=false
                pcall(schedule) -- preserve the productive step failure
                fail(job,done)
            end
            profile('work:'..tostring(job.label),(os.clock()-stepStart)*1000)
        end
        B.running=false
        if (os.clock()-start)*1000>B.ms then B.overruns=B.overruns+1 end
        schedule()
        profile('work:slice',(os.clock()-start)*1000)
    end
    schedule=function()
        if closed or options.automatic==false or wakeup or not B.pending() then return end
        wakeup=api.delay(1,function() wakeup=nil; B.drain(); schedule() end)
    end
    -- Explicit load barrier: finish this consumer's finite initialization jobs
    -- before revealing its world/UI. Background callers still use drain().
    function B.flush(label)
        assert(type(label)=='string' and label~='','load barrier needs a label')
        if closed or B.running then return false end -- a native continuation can re-enter
        if wakeup then wakeup(); wakeup=nil end
        B.running=true; B.pass=B.pass+1
        local count=0; local start=os.clock()
        while #B.jobs>0 do
            local job=table.remove(B.jobs,1); active=job
            local ok,done=pcall(function()
                if api.profile then return api.profile('work:'..tostring(job.label),job.step) end
                return job.step()
            end)
            if ok and type(done)=='table' and type(done.native)=='function' then
                if job.epoch==B.epoch and B.keys[job.key]==job then ok,done=pcall(done.native) end
                if ok then done=false end
            end
            active=nil; count=count+1
            if job.epoch==B.epoch and B.keys[job.key]==job then
                if not ok or done==true then B.keys[job.key]=nil else B.jobs[#B.jobs+1]=job end
            end
            if not ok or count>1000000 then
                B.running=false; pcall(schedule)
                fail(job,ok and 'non-finite job' or done,'load barrier '..label..' / ')
            end
        end
        B.running=false
        if wakeup then wakeup(); wakeup=nil end
        profile('load:'..label,(os.clock()-start)*1000)
        return true
    end
    function B.reset()
        if closed then return false end
        B.epoch=B.epoch+1
        local kept,keys={},{}
        for _,job in ipairs(B.jobs) do if job.persistent then
            job.epoch=B.epoch; kept[#kept+1]=job; keys[job.key]=job
        end end
        if active and active.persistent and B.keys[active.key]==active then active.epoch=B.epoch; keys[active.key]=active end
        B.jobs=kept; B.keys=keys
        if wakeup then wakeup(); wakeup=nil end
        schedule()
    end
    -- Queue ownership ends with its consumer, including persistent jobs. This
    -- is pure bookkeeping; the runtime cancels the separately owned wake token.
    -- World reset retains persistent jobs; package teardown must release them.
    local function retire()
        if closed then return true end
        closed=true; B.epoch=B.epoch+1
        B.jobs={}; B.keys={}; active=nil; wakeup=nil; B.running=false
        return true
    end
    if api.own then api.own(retire,retire) end
    return B
end
return M
