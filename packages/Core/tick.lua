-- Finite event-driven settlement, replacing MRPP hot Tick hooks and PCOS retained sender.
local M={}
function M.new(api,fn,options)
    options=options or {}; local remaining,stop=0,nil
    local interval,passes=options.interval or 100,options.passes or 16
    assert(type(fn)=='function' and passes>=1 and passes%1==0,'invalid settlement callback')
    local T={}; local schedule; local nextTime
    schedule=function()
        if options.external_driver or stop or remaining<=0 then return end
        stop=api.delay(interval,function()
            stop=nil; remaining=remaining-1
            fn(); schedule()
        end)
    end
    function T.wake() remaining=passes; nextTime=nil; schedule() end
    function T.advance(time)
        assert(options.external_driver and type(time)=='number','advance requires an external monotonic clock')
        if remaining<=0 or nextTime and time<nextTime then return end
        remaining=remaining-1; nextTime=time+interval
        fn()
    end
    function T.reset() remaining=0; if stop then stop(); stop=nil end end
    function T.pending() return remaining>0 end
    return T
end
return M
