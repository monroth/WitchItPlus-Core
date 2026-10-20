-- Deliberately small constraint grammar: *, exact x.y.z, or whitespace AND comparisons.
local M={}
local function version(s)
    if type(s)~='string' then return end
    local a,b,c=s:match('^(%d+)%.(%d+)%.(%d+)$')
    if a then return {tonumber(a),tonumber(b),tonumber(c)} end
end
local function compare(a,b)
    for i=1,3 do if a[i]~=b[i] then return a[i]<b[i] and -1 or 1 end end
    return 0
end
function M.satisfies(v,constraint)
    local a=assert(version(v),'invalid version: '..tostring(v))
    assert(type(constraint)=='string' and constraint~='','invalid constraint')
    if constraint=='*' then return true end
    local matches=true
    for token in constraint:gmatch('%S+') do
        local op,wanted=token:match('^([<>=]*)(%d+%.%d+%.%d+)$')
        assert(op and (op=='' or op=='=' or op=='==' or op=='>' or op=='>=' or op=='<' or op=='<='),
            'invalid constraint: '..constraint)
        local n=compare(a,assert(version(wanted)))
        local pass=((op=='' or op=='=' or op=='==') and n==0) or (op=='>' and n>0)
            or (op=='>=' and n>=0) or (op=='<' and n<0) or (op=='<=' and n<=0)
        matches=matches and pass
    end
    return matches
end
function M.validate(m)
    assert(type(m)=='table','manifest must return a table')
    assert(type(m.id)=='string' and m.id:match('^[%w_%-]+$'),'invalid id')
    assert(version(m.version),'version must be x.y.z')
    assert((m.api_version==1 or m.api_version==2),'unsupported api_version: '..tostring(m.api_version))
    m.protocol_version=m.protocol_version or 1
    assert(type(m.protocol_version)=='number' and m.protocol_version%1==0 and m.protocol_version>=1 and m.protocol_version<=65535,'invalid protocol_version')
    if m.required_on_all_players==nil then m.required_on_all_players=false end
    assert(type(m.required_on_all_players)=='boolean','required_on_all_players must be boolean')
    assert(type(m.enabled)=='boolean','enabled must be boolean')
    for _,field in ipairs({'gameplay','critical','core_component'}) do
        assert(m[field]==nil or type(m[field])=='boolean',field..' must be boolean')
    end
    assert(type(m.dependencies)=='table','dependencies must be a table')
    for id,c in pairs(m.dependencies) do
        assert(type(id)=='string' and id:match('^[%w_%-]+$'),'invalid dependency id')
        M.satisfies('0.0.0',c) -- validate even if dependency is absent
    end
    return m
end
-- Both directions matter: a required package present only on the peer is also a mismatch.
function M.mismatch(localList,remoteList)
    assert(type(remoteList)=='table' and #remoteList<=128,'invalid package inventory')
    local left,right={},{}
    for _,item in ipairs(localList) do left[item.id]=item end
    for _,item in ipairs(remoteList) do
        assert(type(item)=='table' and type(item.id)=='string' and item.id:match('^[%w_%-]+$') and not right[item.id],'invalid package inventory id')
        assert(version(item.version) and type(item.protocol_version)=='number' and item.protocol_version%1==0
            and item.protocol_version>=1 and item.protocol_version<=65535 and type(item.required_on_all_players)=='boolean','invalid package inventory metadata')
        right[item.id]=item
    end
    local problems={}; local ids={}
    for id in pairs(left) do ids[id]=true end; for id in pairs(right) do ids[id]=true end
    for id in pairs(ids) do
        local a,b=left[id],right[id]
        local name=({BaseWorldPlus="Editor tweaks"})[id]or id
        if a and b or (a and a.required_on_all_players) or (b and b.required_on_all_players) then
            if not a or not b then problems[#problems+1]='Install or enable '..name..' '..(a and a.version or b.version)..' on '..(a and 'the other player' or 'this player')
            elseif a.version~=b.version or a.protocol_version~=b.protocol_version or a.required_on_all_players~=b.required_on_all_players then
                local order=compare(assert(version(a.version)),assert(version(b.version)))
                local side=order<0 and 'this player' or order>0 and 'the other player' or 'the player with the older protocol'
                problems[#problems+1]='Update '..name..': '..side..' must update (this player '..a.version..', other player '..b.version..'); enabled versions must match'
            end
        end
    end
    table.sort(problems); return problems
end
return M
