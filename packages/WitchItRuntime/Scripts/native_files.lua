-- Runtime-owned registration; only the committed Core API supplies handlers.
-- Native holds these ordinary Lua closures via host references, never globals.
local M={}
function M.install(R,p,A,call,log,save,load)
    assert(p.id=='Core' and p.api==A and p.active,'native boundaries require current Core ownership')
    assert(type(save)=='function' and type(load)=='function','save/load handlers required')
    local alive=true;local attached=false;local generation
    local function current()
        return alive and R.is_healthy() and p.active and p.api==A
    end
    local function boundary(kind,fn,...)
        if not current() or not fn then return false end
        local ok,value=call(p,'native '..kind,fn,nil,...)
        return current() and ok and value==true
    end
    local saveSink=function(...) return boundary('save',save,...) end
    local loadSink=function(...) return boundary('load',load,...) end
    local function revoke(native)
        if not alive then return true end
        alive=false;save=nil;load=nil -- drop package callbacks before Native/GC reentry
        if native and attached and R.is_healthy() then pcall(R.native_call,'WitchItNativeProducerDetach',generation) end
        attached=false;generation=nil;return true
    end
    A.own(function() return revoke(true) end,function() return revoke(false) end)
    local queued=A.game_thread(function()
        if not current() then return end
        local ok,value,token=pcall(R.native_call,'WitchItNativeProducerAttach',saveSink,loadSink)
        generation=ok and value==true and type(token)=='number' and math.type(token)=='integer' and token>0 and token or nil
        attached=generation~=nil and current()
        log(attached and 'native SaveLoad callbacks attached' or 'native SaveLoad unavailable; no runtime acceptance')
        if not current() and generation then pcall(R.native_call,'WitchItNativeProducerDetach',generation) end
    end)
    local function active()return current() and attached end
    return {current=active,
        recovery=function()if active() then return R.native_call('WitchItNativeWriterRecovery') end end,
        recovery_snapshot=function()if active() then return R.native_call('WitchItNativeWriterRecoverySnapshotV1') end end,
        recovery_resolve=function(incident,revision)return active() and R.native_call('WitchItNativeWriterRecoveryResolveRestoredV1',incident,revision)==true end,
        recovery_ready=function(session,revision)return active() and R.native_call('WitchItNativeWriterRecoveryVerifyReadyV1',session,revision)==true end,
        recovery_verified=function(incident,session,revision,proof)return active() and R.native_call('WitchItNativeWriterRecoveryVerifyResolutionV1',incident,session,revision,proof)==true end,
        generation=function()return current() and attached and generation or nil end,
        cancel=function()if queued then queued();queued=nil end;return revoke(true)end}
end
return M
