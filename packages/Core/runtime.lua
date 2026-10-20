-- Common safe reflection and owned-hook bridge. Gameplay policy stays in the consumer.
local Values=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'../WitchItRuntime/Scripts/callback_values.lua')
local ukind=Values.kind
local M={}
-- Player copy only. Callers retain the original error in the technical log.
function M.player_error(value)
    local text=tostring(value):lower()
    local messages={
        {'unsupported rules code version','This rules code uses a different version. Update Match Rules +.'},
        {'corrupt code','This rules code is damaged. Copy it again.'},
        {'size limit','This rules code is too long. Use a smaller preset.'},
        {'exceeds 36','This preset is too large to share. Remove some overrides.'},
        {'missing preset','Preset not found. Check the name.'},
        {'name: 1%-64','Use a preset name with 1–64 letters, numbers, dashes or underscores.'},
        {'reserved filename','This name cannot be used. Choose another.'},
        {'mode differs','This preset is for another game mode. Select that mode first.'},
        {'game mode differs','This preset is for another game mode. Select that mode first.'},
        {'ten seconds','Please wait ten seconds before sharing again.'},
        {'no compatible modded clients','No players with matching mods are connected. Wait for someone to join.'},
        {'expired','This offer has expired. Ask the sender to send it again.'},
        {'disconnected','The sender disconnected. Ask them to rejoin.'},
        {'save the creative','Save the map first, then try again.'},
        {'save the map first','Save the map first, then try again.'},
        {'saved authoritative','Save a Port Plunder map as host, then try again.'},
        {'host only','Only the host can do this. Ask the host.'},
        {'local host','Only the host can do this. Ask the host.'},
        {'async class load pending','The teleporter is loading. Try again shortly.'},
        {'teleporter companion loading','Teleporters are loading. Wait a moment or use bteleport reload.'},
        {'companion','Saved edits could not be read or written. Check the map folder and reload.'},
        {'unknown teleporter','Teleporter not found. Use bteleport list to check its ID.'},
        {'argument','Invalid arguments. Use the command’s help to check usage.'},
        {'unknown.*command','Unknown command. Use the command’s help to check usage.'},
        {'number from shiphide list','Choose a number from shiphide list.'},
        {'no eligible actor','Scenery not found. Use bwhide list to check its name.'},
        {'no eligible actor matched','Scenery not found. Use bwhide list to check its name.'},
        {'cleanup pending','An edit could not be restored. Use the command’s cleanup option.'},
        {'unapplied','Some saved edits are not applied. Reload them before editing.'},
        {'loading','Still loading. Wait a moment and try again.'},
        {'busy','An edit is still in progress. Wait a moment and try again.'},
    }
    for _,row in ipairs(messages)do if text:find(row[1])then return row[2]end end
    return 'Could not complete this action. Try again.'
end
function M.access(label,fn,...)
    local d=WIR_RUNTIME and WIR_RUNTIME.diagnostics
    if d and d.trace_native_access then return d.access(nil,label,fn,...) end
    return fn(...)
end
function M.read(o,k)
    local ok,v=pcall(M.access,'property:'..k,function() return o[k] end)
    -- A missing member reads as UE4SS's TrivialObject sentinel: report it as absent.
    if ok and ukind(v)~='TrivialObject' then return v end
    return nil
end
function M.call(o,k,...)
    local args=table.pack(...)
    local ok,v=pcall(M.access,'method:'..k,function() return o[k](o,table.unpack(args,1,args.n)) end)
    if ok then return v end; return nil
end
M.unwrap=Values.unwrap
local objects={UObject=true,AActor=true,UWorld=true,UClass=true,UFunction=true,UStruct=true,UEnum=true,UDataTable=true}
function M.isObject(o)
    if type(o)=='userdata' then return objects[ukind(o) or '']==true end
    return type(o)=='table' -- offline fixtures; callers still validate liveness
end
function M.text(o) if type(o)=='string' then return o end; return M.call(o,'ToString') or '' end
function M.values(a)
    local out={}; if not a then return out end
    if type(a)=='table' and not M.read(a,'ForEach') then
        for _,v in ipairs(a) do out[#out+1]=M.unwrap(v) end
    else
        local each=M.read(a,'ForEach')
        if type(each)~='function' then return out end
        a:ForEach(function(_,v) out[#out+1]=M.unwrap(v) end)
    end
    return out
end
function M.new(api,L)
    local H=setmetatable({valid=L.valid,lifetime=L,hook=api.hook,log=api.log,delay=api.delay,loop=api.loop},{__index=M})
    function H.same(a,b) return L.valid(a) and L.valid(b) and L.same(a,b) end
    H.sameWorld=L.sameWorld
    function H.subscribe(path,callback,post,options)
        options=options or {}
        local function phase(name)
            local copy={}; for key,value in pairs(options) do copy[key]=value end
            copy.phase=name; return copy
        end
        local cancel=api.hook(path,callback,phase(path:sub(1,8)=='/Script/' and 'pre' or 'post'))
        local cancelPost=post and api.hook(path,post,phase('post')) or function() end
        return cancel,cancelPost
    end
    function H.object(path) local o=StaticFindObject(path); assert(L.rawValid(o),'object unavailable: '..path); return o end
    function H.authority(o) return L.valid(o) and H.call(o,'HasAuthority')==true end
    function H.group(predicate)
        assert(type(predicate)=='function','group needs an activation predicate')
        local group=setmetatable({},{__index=H})
        function group.hook(path,fn,opts)
            return api.hook(path,function(...) if predicate(path) then return fn(...) end end,opts)
        end
        function group.subscribe(path,fn,post,opts)
            local function gated(callback)
                return function(...) if predicate(path) then return callback(...) end end
            end
            return H.subscribe(path,gated(fn),post and gated(post),opts)
        end
        function group.command(name,fn,opts)
            return api.command(name,function(...)
                if not predicate() then api.log('command inactive in this mode'); return true end
                return fn(...)
            end,opts)
        end
        return group
    end
    return H
end
return M
