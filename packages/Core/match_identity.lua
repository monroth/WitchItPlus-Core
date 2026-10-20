-- Native identity strings only. Never marshal BP_GetUniqueId/FUniqueNetIdRepl.
local Module={}
function Module.new(L)
local valid=L.valid
local M={revision=0}
local scope,scopeKey
local stores={}
local scalarTables=setmetatable({},{__mode='k'})
local function scalar(value,seen,depth)
    local t=type(value)
    if t=='nil' or t=='boolean' or t=='string' then return value end
    if t=='number' then assert(value==value and math.abs(value)<math.huge,'nonfinite session fact'); return value end
    assert(t=='table' and (not getmetatable(value) or scalarTables[value]),'session facts must be scalar data')
    seen=seen or {}; depth=depth or 0
    assert(depth<32 and not seen[value],'cyclic/deep session facts'); seen[value]=true
    local data={}
    local function assign(k,v)
        assert(type(k)=='string' or type(k)=='number','invalid session fact key')
        data[k]=scalar(v,seen,depth+1)
    end
    for k,v in pairs(value) do assign(k,v) end
    seen[value]=nil
    local proxy=setmetatable({}, {__index=data,__newindex=function(_,k,v) assign(k,v) end,
        __pairs=function() return pairs(data) end,__len=function() return #data end})
    scalarTables[proxy]=true
    return proxy
end
function M.key(o)
    if not valid(o) then return end
    return tostring(o:GetAddress())..':'..o:GetFullName()
end
function M.session(gi)
    local key=M.key(gi); if not key then return end
    if not scope or scopeKey~=key then
        for _,store in pairs(stores) do store.handles={} end
        stores={}; scope=scalar({key=key}); scopeKey=key; M.revision=M.revision+1
    end
    return scope
end
function M.forget(gi)
    M.forget_key(M.key(gi))
end
function M.forget_key(key)
    if scope and scopeKey==key then
        for _,store in pairs(stores) do store.handles={} end
        stores={}; scope=nil; scopeKey=nil; M.revision=M.revision+1
    end
end
function M.records(gi,namespace,volatile)
    assert(type(namespace)=='string' and namespace~='','record namespace required')
    local session=M.session(gi); if not session then return end
    if stores[namespace] then return stores[namespace].records end
    if not session.records then session.records={} end
    if not session.records[namespace] then session.records[namespace]={} end
    local facts=session.records[namespace]
    local fields={}
    for field,enabled in pairs(volatile or {}) do assert(type(field)=='string' and enabled==true); fields[field]=true end
    local store={handles={},views={}}
    local function view(id)
        if not facts[id] then return end
        if not store.views[id] then
            store.views[id]=setmetatable({},{
                __index=function(_,field)
                    if fields[field] then return store.handles[id] and store.handles[id][field] end
                    return facts[id] and facts[id][field]
                end,
                __newindex=function(_,field,value)
                    assert(facts[id],'retired record')
                    if fields[field] then
                        store.handles[id]=store.handles[id] or {}; store.handles[id][field]=value
                    else facts[id][field]=value end
                end,
                __pairs=function()
                    local combined={}
                    for field,value in pairs(facts[id] or {}) do combined[field]=value end
                    for field,value in pairs(store.handles[id] or {}) do combined[field]=value end
                    return pairs(combined)
                end,
            })
        end
        return store.views[id]
    end
    store.records=setmetatable({},{
        __index=function(_,id) return view(id) end,
        __newindex=function(_,id,record)
            assert(type(id)=='string','records need native/scalar IDs')
            if record==nil then facts[id]=nil; store.handles[id]=nil; store.views[id]=nil; return end
            assert(type(record)=='table','record data required')
            local values,handles={},{}
            for field,value in pairs(record) do
                if fields[field] then handles[field]=value else values[field]=value end
            end
            facts[id]=values; store.handles[id]=handles
        end,
        __pairs=function()
            local ids={}; for id in pairs(facts) do ids[#ids+1]=id end
            local index=0
            return function()
                index=index+1
                local id=ids[index]
                if id then return id,view(id) end
            end
        end,
    })
    stores[namespace]=store
    return store.records
end
L.onReset(function() for _,store in pairs(stores) do store.handles={} end end)
local function read(o,k) local ok,v=pcall(function() return o[k] end); if ok then return v end end
function M.text(v)
    if type(v)=='string' then return v end
    if v~=nil then local ok,s=pcall(function() return v:ToString() end); if ok then return s end end
end
function M.id(ps,validatePlayer)
    if not (validatePlayer or valid)(ps) then return nil end
    local m=read(ps,'ObjectiveMatchPlayerState')
    if valid(m) and (read(m,'ObjectiveTeam')==1 or read(m,'ObjectiveTeam')==2) then
        local id=M.text(read(m,'PlayerId')); if id and id~='' then return id end
    end
    -- These are IDs, unlike PlayerName/OnlineSystemDisplayName. A detached
    -- waiter's PlayerId is just an object label, so never use it as identity.
    for _,v in ipairs({read(ps,'ProductUserId') or '',read(ps,'OnlineSystemAccountName') or ''}) do
        local id=M.text(v); if id and id~='' then return id end
    end
end
function M.member(mm,state)
    if not valid(mm) or not valid(state) then return end
    local roster=read(mm,'Players'); if not roster then return end
    local found,id
    roster:ForEach(function(key,value)
        local member=value:get(); local nativeId=M.text(key:get())
        if nativeId and nativeId~='' and valid(member) and L.same(member,state)
            and (read(member,'ObjectiveTeam')==1 or read(member,'ObjectiveTeam')==2) then found,id=member,nativeId end
    end)
    return found,id
end
function M.participant(mm,ps,validatePlayer)
    if not valid(mm) or not (validatePlayer or valid)(ps) then return end
    local attached=read(ps,'ObjectiveMatchPlayerState')
    local attachedId=valid(attached) and M.text(read(attached,'PlayerId'))
    local product=M.text(read(ps,'ProductUserId'))
    local platform=M.text(read(ps,'OnlineSystemAccountName'))
    local found,id
    local roster=read(mm,'Players')
    if not roster then return end
    roster:ForEach(function(k,v)
        local key=M.text(k:get()); local m=v:get()
        if key and key~='' and valid(m) and (read(m,'ObjectiveTeam')==1 or read(m,'ObjectiveTeam')==2)
            and (key==product or key==platform or
                (key==attachedId and valid(attached) and L.same(attached,m))) then
            found,id=m,key
        end
    end)
    return found,id
end
return M
end
return Module
