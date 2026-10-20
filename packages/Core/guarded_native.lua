-- Explicit held Native walk. No global guard state or UObject proxy is introduced.
local M={}
local objects={UObject=true,AActor=true,UWorld=true,UClass=true,UFunction=true,UStruct=true,UEnum=true,UDataTable=true}
function M.new(owner,L,guard)
    assert(type(guard)=='function' and type(owner)=='table' and type(owner.is_live)=='function','explicit Native owner/guard required')
    assert(type(L.retirement_revision)=='number','guarded Native needs Core2 retirement producer')
    local retirementRevision=L.retirement_revision
    local epoch,world=L.epoch,L.world
    local retired,departed=L.retired,L.departed
    local G={}
    function G.current()
        return owner.is_live()==true and L.retirement_revision==retirementRevision and not L.transition and L.epoch==epoch and L.world==world
            and retired==L.retired and departed==L.departed and guard()==true
    end
    function G.read(o,key)
        if not G.current() or type(o)~='table' and type(o)~='userdata' then return end
        local value=o[key]
        if G.current() then return value end
    end
    function G.call(o,key,...)
        local fn=G.read(o,key)
        if not G.current() or type(fn)~='function' and type(fn)~='userdata' then return end
        local value=fn(o,...)
        if G.current() then return value end
    end
    local function kind(o)
        if not G.current() then return end
        -- UE4SS TrivialObject can lack the bound type() method. Discrimination
        -- is optional; this narrow protected probe performs no fallback reflection.
        local ok,value=pcall(G.call,o,'type')
        if ok and G.current() and type(value)=='string' then return value end
    end
    function G.unwrap(o)
        if not G.current() then return end
        if type(o)=='table' then
            local fn=rawget(o,'get')
            if type(fn)=='function' then
                local value=fn(o)
                if G.current() then return value end
            else return o end
        elseif type(o)=='userdata' then
            local kind=kind(o)
            if not G.current() then return end
            if kind=='RemoteUnrealParam' or kind=='LocalUnrealParam' then return G.call(o,'get') end
            if G.current() then return o end
        else return o end
    end
    function G.text(o)
        if not G.current() then return end
        if type(o)=='string' then return o end
        local value=G.call(o,'ToString')
        if G.current() and type(value)=='string' then return value end
    end
    function G.isObject(o)
        if not G.current() then return false end
        if type(o)=='table' then return true end -- offline actor facade; valid still checks actual methods
        return type(o)=='userdata' and objects[kind(o) or '']==true and G.current() or false
    end
    local function rawValid(o)
        if not G.isObject(o) or G.call(o,'IsValid')~=true or not G.current() then return false end
        for _,spec in ipairs({{EInternalObjectFlags,'HasAnyInternalFlags',{'PendingKill','Unreachable','Garbage'}},
            {EObjectFlags,'HasAnyFlags',{'RF_BeginDestroyed','RF_FinishDestroyed','BeginDestroyed','FinishDestroyed'}}}) do
            if type(spec[1])=='table' then
                for _,name in ipairs(spec[3]) do
                    if not G.current() then return false end
                    local flag=spec[1][name]
                    if not G.current() or flag and (G.call(o,spec[2],flag)==true or not G.current()) then return false end
                end
            end
        end
        local actor=G.call(o,'IsA','/Script/Engine.Actor')
        if not G.current() then return false end
        return actor~=true or G.call(o,'IsActorBeingDestroyed')~=true and G.current()
    end
    local function identity(o)
        local address=G.call(o,'GetAddress');if not G.current() or not tonumber(address) or tonumber(address)<=0 then return end
        local name=G.call(o,'GetFullName');if not G.current() or type(name)~='string' or name=='' then return end
        return tostring(address)..':'..name
    end
    local function alive(o)
        if not rawValid(o) then return false end
        local key=identity(o)
        return G.current() and key~=nil and not L.retired[key]
    end
    local function present(o)
        if not G.current() or not o then return false end
        local address=G.call(o,'GetAddress')
        if not G.current() then return false end
        if address~=nil then return tonumber(address)~=0 end
        return rawValid(o)
    end
    local function ownersAlive(o)
        local parent
        if G.call(o,'IsA','/Script/Engine.Actor')==true then parent=G.call(o,'GetOwner') end
        if not G.current() then return false end
        if not present(parent) then parent=G.call(o,'GetOuter') end
        local seen={}
        for _=1,8 do
            if not G.current() then return false end
            if not present(parent) then break end
            if not rawValid(parent) then return false end
            local key=identity(parent)
            if not G.current() or key and L.retired[key] then return false end
            if not key or seen[key] then break end
            seen[key]=true;parent=G.call(parent,'GetOuter')
        end
        return G.current()
    end
    function G.worldKey(o)
        if not alive(o) then return end
        local w=G.call(o,'GetWorld')
        if G.current() and rawValid(w) then return identity(w) end
    end
    function G.valid(o)
        if not alive(o) or not ownersAlive(o) then return false end
        local w=G.call(o,'GetWorld');if not G.current() then return false end
        local worldKey=rawValid(w) and identity(w)
        if not G.current() then return false end
        if not worldKey then
            local name=G.call(o,'GetFullName')
            if not G.current() or type(name)~='string' then return false end
            if not name:find('Default__',1,true) then
                for _,class in ipairs({'/Script/Engine.Actor','/Script/Engine.ActorComponent','/Script/UMG.UserWidget'}) do
                    if G.call(o,'IsA',class)==true or not G.current() then return false end
                end
            end
        end
        return G.current() and (not worldKey or not L.departed[worldKey] and (not L.world or worldKey==L.world)) or false
    end
    function G.key(o) if G.valid(o) then return identity(o) end end
    function G.authority(o) return G.valid(o) and G.call(o,'HasAuthority')==true and G.current() or false end
    function G.sameWorld(a,b)
        if not G.valid(a) or not G.valid(b) then return false end
        local wa,wb=G.worldKey(a),G.worldKey(b)
        return G.current() and wa~=nil and wa==wb or false
    end
    return G
end
return M
