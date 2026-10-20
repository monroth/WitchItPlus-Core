-- Optional cross-package services, without filesystem detection or hard dependencies.
-- Claims are lifetime-owned; consumers can observe release/replacement.
local M={}
function M.new()
    local claims,listeners={},{}
    local function changed(name)
        for _,fn in ipairs({table.unpack(listeners[name] or {})}) do fn() end
    end
    local service={}
    function service.get(name) local c=claims[name];return c and c.value,c and c.id end
    function service.claim(api,name,value,options)
        assert(type(name)=='string' and type(value)=='table' and not claims[name],'capability already claimed: '..name)
        assert(options==nil or type(options)=='table','invalid capability options')
        local releaseReady=options and options.release_ready
        assert(releaseReady==nil or type(releaseReady)=='function','invalid capability release guard')
        local claim={id=api.id,value=value};claims[name]=claim
        local release=api.own(function()
            if claims[name]==claim and releaseReady then
                local done,why=releaseReady()
                if done~=true then return false,'capability '..name..': '..tostring(why or 'owner rollback incomplete') end
            end
            if claims[name]==claim then claims[name]=nil;changed(name) end
        end,function()
            -- Native world abandonment cannot execute arbitrary listener code.
            if claims[name]==claim then claims[name]=nil end
        end)
        changed(name);return release
    end
    function service.watch(api,name,fn)
        local rows=listeners[name] or {};listeners[name]=rows;rows[#rows+1]=fn
        return api.own(function() for i,v in ipairs(rows) do if v==fn then table.remove(rows,i);break end end end)
    end
    return service
end
return M
