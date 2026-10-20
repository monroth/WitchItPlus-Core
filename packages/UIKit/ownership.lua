local M={}
function M.new(api)
    local S={closed=false,undo={}}
    function S.own(fn)
        assert(not S.closed,'UI scope closed')
        S.undo[#S.undo+1]=fn
        return fn
    end
    function S.close()
        if S.closing then return false,'UIKit scope cleanup already running' end
        if S.closed and #S.undo==0 then return true end
        S.closed=true
        S.closing=true
        local errors,pending={},{}
        for i=#S.undo,1,-1 do
            local undo=S.undo[i]
            local ok,result,why=pcall(undo)
            if not ok or result==false then
                table.insert(pending,1,undo)
                local info=debug.getinfo(undo,'S')
                errors[#errors+1]=(info.short_src or info.source)..':'..info.linedefined..': '..tostring(ok and (why or 'callback returned false without a reason') or result)
            end
        end
        S.undo=pending; S.closing=false
        if #errors>0 then
            -- api.log is inactive during runtime disable; preserve teardown diagnostics.
            print('[UIKit] cleanup '..api.id..': '..table.concat(errors,' | '))
        end
        return #pending==0,table.concat(errors,' | ')
    end
    function S.abandon()
        -- A proven world boundary releases handles without replaying old undo.
        S.closed=true; S.undo={}; S.screen=nil
        if S.children then
            for child in pairs(S.children) do child.abandon() end
            S.children={}
        end
    end
    return S
end
function M.attach(api,scope)
    scope.close=api.own(scope.close,scope.abandon,'UIKit scopes')
    return scope.close
end
return M
