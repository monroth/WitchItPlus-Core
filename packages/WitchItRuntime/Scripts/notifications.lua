-- NotifyOnNewObject enters during construction. The native callback only records
-- a handle/class/subscriber snapshot; package code runs after a native frame barrier.
local M={}
function M.new(R,call,log)
    local N={queue={},head=1,epoch=0}
    local ticket
    local function order(a,b)
        if a.priority~=b.priority then return a.priority<b.priority end
        if a.owner.id~=b.owner.id then return a.owner.id<b.owner.id end
        return a.sequence<b.sequence
    end
    function N.reset(reason)
        if N.queue[N.head] and log then
            log('INFO notification queue reset reason='..reason..' pending='..(#N.queue-N.head+1))
        end
        N.epoch=N.epoch+1; N.queue={}; N.head=1
        -- Queued native closures hold only this service and a scalar epoch.
        -- Keep the ticket until it fires, so rapid resets cannot multiply frames.
    end
    local arm
    function N.pause(reason)
        N.reset(reason); N.paused=true
    end
    function N.resume()
        N.paused=false; arm()
    end
    local function drain()
        local started=os.clock()*1000
        local epoch=N.epoch
        local last=#N.queue
        N.draining=true
        for _=1,8 do
            if N.head>last then break end -- births during delivery need their own later frame
            local item=N.queue[N.head]; if not item then break end
            local entry=item.subscribers[item.cursor]
            item.cursor=item.cursor+1
            if not item.subscribers[item.cursor] then
                N.queue[N.head]=false; N.head=N.head+1
            end
            if entry.alive and entry.owner.active and (entry.ungated or entry.owner.engaged) then
                local ok,result=call(entry.owner,'notify '..item.class,entry.fn,entry,item.object)
                if ok and result==true then entry.cancel() end
            end
            if epoch~=N.epoch or not R.alive or os.clock()*1000-started>=3 then break end
        end
        if not N.queue[N.head] then N.queue={}; N.head=1 end
        N.draining=false
        arm()
    end
    arm=function()
        if ticket or N.draining or N.paused or not R.alive or not N.queue[N.head] then return end
        ticket=true
        local epoch=N.epoch
        R.native_call('ExecuteInGameThreadAfterFrames',1,function()
            ticket=nil
            if not R.alive then return end
            if N.paused then return end
            if epoch~=N.epoch then arm(); return end
            drain()
        end)
    end
    function N.enqueue(class,object,subscribers)
        if not R.alive then return end
        local snapshot={}
        for _,entry in ipairs(subscribers) do
            if entry.alive and entry.owner.active then snapshot[#snapshot+1]=entry end
        end
        if #snapshot==0 then return end
        table.sort(snapshot,order)
        N.queue[#N.queue+1]={class=class,object=object,subscribers=snapshot,cursor=1}
        arm()
    end
    return N
end
return M
