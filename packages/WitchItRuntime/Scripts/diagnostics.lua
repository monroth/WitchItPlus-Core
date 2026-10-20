-- Optional call logging only. Native owns callback entry, marshaling and lifetime.
local M={trace_native_access=false,trace_timer_calls=false,starting=false,owner='runtime'}
local pack,unpack=table.pack,table.unpack
local function emit(text) print('[WIR-DIAG] '..text..'\n') end
local function traced(name)
    return M.trace_native_access or M.starting or M.trace_timer_calls
        and (name:find('Delay',1,true) or name:find('Frames',1,true))
end
local function invoke(label,fn,...)
    emit(label..' phase=before')
    local result=pack(pcall(fn,...))
    emit(label..' phase=after ok='..tostring(result[1]))
    if not result[1] then error(result[2],0) end
    return unpack(result,2,result.n)
end
function M.access(owner,name,fn,...)
    if not M.trace_native_access then return fn(...) end
    return invoke('package='..(owner or M.owner)..' access='..name,fn,...)
end
function M.invoke(name,fn,owner,...)
    if not traced(name) then return fn(...) end
    return invoke('package='..(owner or M.owner)..' api='..name,fn,...)
end
local nativeNames={StaticFindObject=true,FindAllOf=true,StaticConstructObject=true}
function M.bind(name,fn,owner)
    if not (nativeNames[name] or name:match('^Register') or name:match('^Unregister')
        or name:match('^Execute') or name:match('^Loop') or name:match('^Cancel')
        or name:match('^WitchItNative')) then return fn end
    return function(...) return M.invoke(name,fn,owner,...) end
end
function M.begin(owner)
    M.owner=owner;M.starting=true;emit('package='..owner..' phase=before')
end
function M.finish(owner)
    M.starting=false;M.owner='runtime';emit('package='..owner..' phase=after')
end
function M.lua_cancel(owner,kind,key)
    if M.trace_native_access or M.trace_timer_calls then
        emit('package='..owner..' cancel='..kind..(key and ' resource='..key or ''))
    end
end
return M
