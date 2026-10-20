-- A one-shot healthy WIR lane after pure Core quiescence, before rollback.
-- It does not infer remote health or run during terminal/runtime unload.
local M={}
function M.new(R)
    local owners={}
    local B={}
    local function copy(value)
        local result={}
        for key,item in pairs(value or {}) do result[key]=item end
        return result
    end
    function B.attach(p,A)
        local r={api=A,alive=true,receipt={status='unregistered',attempted=false}}
        owners[p]=r
        local S={};A.retirement=S
        function S.publish(fn)
            assert(p.id=='Core','only Core owns the common endpoint withdrawal')
            assert(A.is_live() and owners[p]==r and not r.used,'retirement owner inactive')
            assert(type(fn)=='function' and r.fn==nil,'one retirement publisher required')
            r.fn=fn;r.receipt={status='registered',attempted=false}
            return function()
                r.alive=false;r.fn=nil;r.ticket=nil
                if owners[p]==r then owners[p]=nil end
                if not r.used or r.running then
                    r.receipt={status='cancelled',attempted=r.receipt.attempted==true}
                end
            end
        end
        function S.result()
            -- This historical local receipt carries no Native/send capability.
            -- Keep an opaque error separate, with its original Lua identity.
            return copy(r.receipt),r.error
        end
    end
    function B.begin(p)
        local r=owners[p]
        if not R.is_healthy() or R.unload_started or not p.active or not p.disabling
            or not r or not r.alive or r.used or p.api~=r.api or not r.fn then return end
        r.used=true
        local generation,world,mode=R.world_generation,R.world_key,R.context.mode
        local ticket={}
        function ticket.is_live()
            return r.ticket==ticket and r.running==true and owners[p]==r and r.alive
                and R.is_healthy() and not R.unload_started and p.disabling==true
                and p.active==false and p.api==r.api and not R.loading and not R.state_retired
                and R.world_generation==generation and R.world_key==world and world~=nil
                and R.context.mode==mode and mode~='menu'
        end
        local callback=r.fn;r.fn=nil;r.ticket=ticket
        return {record=r,ticket=ticket,callback=callback}
    end
    function B.dispatch(handle)
        if not handle then return end
        local r,ticket=handle.record,handle.ticket
        r.running=true
        local attempted=false
        local ok,result=pcall(function()
            if not ticket.is_live() then return {status='scope_revoked',attempted=false} end
            if R.native_call('IsInGameThread')~=true or not ticket.is_live() then
                return {status='scope_revoked',attempted=false}
            end
            attempted=true
            r.receipt={status='publishing',attempted=true}
            return handle.callback(ticket)
        end)
        handle.callback=nil;r.running=false
        if not ok then r.error=result end
        -- A terminal clear/cancel owns its receipt; older completion cannot
        -- replace it or touch a newly attached API incarnation.
        if r.ticket~=ticket then return end
        r.ticket=nil
        if not ok then r.receipt={status='failed',attempted=attempted};return end
        r.receipt={status='completed',attempted=attempted}
        if type(result)=='table' then
            for _,key in ipairs({'routes','invoked','refused'}) do
                local value=rawget(result,key)
                if type(value)=='number' and value%1==0 and value>=0 and value<=32 then r.receipt[key]=value end
            end
            if rawget(result,'status')=='scope_revoked' then r.receipt.status='scope_revoked' end
        end
    end
    function B.clear()
        for _,r in pairs(owners) do
            r.alive=false;r.fn=nil;r.ticket=nil;r.running=false
            r.receipt={status='abandoned',attempted=r.receipt.attempted==true}
        end
        owners={}
    end
    return B
end
return M
