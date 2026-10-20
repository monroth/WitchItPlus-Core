-- Package-owned synchronous boundaries, measured from runtime boot to menu.
-- The clock is replaceable so mocks can add measured/modelled native call costs.
local M={}
function M.new(clock,log)
    clock=clock or function() return os.clock()*1000 end
    local P={active=true,records={},stack={}}
    function P.begin(id,label)
        if not P.active then return end
        local frame={id=id,label=label,start=clock(),children=0}
        P.stack[#P.stack+1]=frame; return frame
    end
    function P.finish(frame)
        if not frame then return end
        assert(P.stack[#P.stack]==frame,'startup profiler nesting')
        P.stack[#P.stack]=nil
        local ms=math.max(0,clock()-frame.start)
        local record=P.records[frame.id] or {total=0,max=0,steps=0}
        P.records[frame.id]=record; record.steps=record.steps+1
        record.total=record.total+math.max(0,ms-frame.children)
        if ms>record.max then record.max=ms; record.label=frame.label end
        local parent=P.stack[#P.stack]; if parent then parent.children=parent.children+ms end
        if ms>5 and log then log(string.format('STARTUP overrun package=%s step=%s ms=%.3f limit=5',frame.id,frame.label,ms)) end
    end
    function P.check()
        for id,r in pairs(P.records) do
            assert(r.max<=5,string.format('startup step %s / %s %.3f ms > 5 ms',id,r.label,r.max))
            assert(r.total<=50,string.format('startup total %s %.3f ms > 50 ms',id,r.total))
        end
        return true
    end
    function P.stop()
        if not P.active then return end
        P.active=false
        if log then for id,r in pairs(P.records) do
            log(string.format('startup package=%s total_ms=%.3f max_step_ms=%.3f steps=%d',id,r.total,r.max,r.steps))
        end end
    end
    return P
end
return M
