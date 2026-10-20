-- Reflected calls deliberately use exact argument counts and loaded classes only.
-- Some UE4SS userdata (TrivialObject) have no bound type(); never assume one.
local function ukind(o)
    local ok,t=pcall(function() return o:type() end); if ok and type(t)=='string' then return t end
    -- Live UE4SS: some UObject wrappers print as 'TrivialObject: <addr>' and have no type().
    local ok2,s=pcall(tostring,o); if ok2 and type(s)=='string' and s:match('^TrivialObject') then return 'TrivialObject' end
end
local E={elements='/Game/UserInterface/UIElements/'}
local objects={UObject=true,AActor=true,UWorld=true,UClass=true,UFunction=true,UStruct=true,UEnum=true,UDataTable=true}
local function access(label,fn,...)
    local d=WIR_RUNTIME and WIR_RUNTIME.diagnostics
    if d and d.trace_native_access then return d.access('UIKit',label,fn,...) end
    return fn(...)
end
function E.read(o,k)
    local ok,v=pcall(access,'property:'..k,function() return o[k] end)
    -- A missing member reads as UE4SS's TrivialObject sentinel: report it as absent.
    if ok and ukind(v)~='TrivialObject' then return v end
end
function E.call(o,k,...)
    assert(E.valid(o),'stale object for '..k)
    local args=table.pack(...)
    for i=1,args.n do assert(args[i]~=nil,'nil UFunction argument: '..k) end
    return access('method:'..k,function()
        local fn=o[k]
        if fn==nil or type(fn)~='function' and ukind(fn)=='TrivialObject' then
            error('missing reflected function '..k..' on '..o:GetFullName(),2)
        end
        return fn(o,table.unpack(args,1,args.n))
    end)
end
function E.valid(o)
    if o==nil or type(o)=='userdata' and not objects[ukind(o) or ''] then return false end
    local ok,v=pcall(function() return o:IsValid() end)
    if not ok or v~=true then return false end
    for _,spec in ipairs({{EInternalObjectFlags,'HasAnyInternalFlags',{'PendingKill','Unreachable','Garbage'}},
        {EObjectFlags,'HasAnyFlags',{'RF_BeginDestroyed','RF_FinishDestroyed'}}}) do
        if type(spec[1])=='table' then
            for _,name in ipairs(spec[3]) do
                local flag=spec[1][name]
                if flag then
                    local worked,dead=pcall(function() return o[spec[2]](o,flag) end)
                    if worked and dead then return false end
                end
            end
        end
    end
    return true
end
function E.key(o)
    if not E.valid(o) then return end
    local ok,key=pcall(function()
        local address=o:GetAddress()
        assert(tonumber(address)~=0)
        return tostring(address)..':'..o:GetFullName()
    end)
    if ok then return key end
end
function E.same(a,b) local key=E.key(a); return key~=nil and key==E.key(b) end
function E.unwrap(p)
    if type(p)=='userdata' then
        local kind=ukind(p)
        if kind=='RemoteUnrealParam' or kind=='LocalUnrealParam' then return access('param:get',function() return p:get() end) end
    elseif type(p)=='table' then
        local get=rawget(p,'get')
        if type(get)=='function' then return get(p) end
    end
    return p
end
function E.text(t)
    if type(t)=='string' then return t end
    return t:ToString()
end
function E.need(o,label) assert(E.valid(o),'unavailable '..label); return o end
function E.class(path) return E.need(StaticFindObject(path),path) end
function E.native(screen,kind)
    return E.need(StaticConstructObject(E.class('/Script/UMG.'..kind),
        E.need(screen.WidgetTree,'WidgetTree')),kind)
end
function E.widget(screen,asset)
    return E.clone(screen,E.elements..asset..'.'..asset..'_C')
end
function E.clone(screen,classOrPath)
    local class=type(classOrPath)=='string' and E.class(classOrPath) or E.need(classOrPath,'widget UClass')
    return E.need(E.call(E.class('/Script/UMG.Default__WidgetBlueprintLibrary'),'Create',
        screen,class,E.need(E.call(screen,'GetOwningPlayer'),'local owning player')),'cooked widget')
end
function E.values(array)
    local out={}
    assert(array,'missing TArray')
    if E.read(array,'ForEach') then
        array:ForEach(function(_,p) out[#out+1]=E.unwrap(p) end)
    else for _,v in ipairs(array) do out[#out+1]=E.unwrap(v) end end
    return out
end
function E.replace(array,values)
    -- This is a TArray operation, never a TMap [] write or Lua-container UFunction.
    assert(E.read(array,'ForEach') and E.read(array,'Empty'),'native TArray required')
    array:Empty()
    for i,v in ipairs(values) do array[i]=v end
end
function E.contains(array,value)
    for _,v in ipairs(E.values(array)) do if E.same(v,value) then return true end end
    return false
end
function E.remove(array,value)
    local out={}
    for _,v in ipairs(E.values(array)) do if not E.same(v,value) then out[#out+1]=v end end
    E.replace(array,out)
end
function E.open(w)
    if not E.valid(w) then return false end
    local node=w; local seen={}
    for _=1,32 do
        if not E.valid(node) or not E.read(node,'IsVisible') or not E.call(node,'IsVisible') then return false end
        if E.read(node,'IsInViewport') and E.call(node,'IsInViewport') then return true end
        local key=E.key(node); if not key or seen[key] then return false end; seen[key]=true
        local parent=E.call(node,'GetParent')
        if not E.valid(parent) then
            local tree=E.read(node,'GetOuter') and node:GetOuter()
            parent=E.valid(tree) and E.read(tree,'GetOuter') and tree:GetOuter()
            if E.same(parent,node) then return false end
        end
        node=parent
    end
    return false
end
function E.path(asset,fn,folder)
    return (folder or E.elements)..asset..'.'..asset..'_C:'..fn
end
return E
