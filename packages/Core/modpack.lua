-- Modpack facts shared by hosting, joining and the server browser: package names,
-- advert parsing and version comparison. Pure functions over the given JSON codec.
return function(json)
    local P={}
    -- Display names by package id; wire ids of the public gameplay packages in adverts.
    P.names={Core='Core',UIKit='UIKit',FillAPotPlus='Fill a Pot+',MatchRulesPlusPlus='Match Rules ++',
        BaseWorldPlus='Editor tweaks',PrivateCosmetics='Private Cosmetics',CreativeUnlocks='Creative Unlocks'}
    P.wires={Core='core',FillAPotPlus='fillapotplus',MatchRulesPlusPlus='matchrulesplusplus',BaseWorldPlus='baseworldplus'}
    P.packageOf={};for id,wire in pairs(P.wires) do P.packageOf[wire]=id end
    local function wireName(wire) return P.names[P.packageOf[wire]] end
    -- -1, 0 or 1 for two x.y.z versions.
    function P.compare(a,b)
        local aa,bb={},{}
        for n in a:gmatch('%d+') do aa[#aa+1]=tonumber(n) end
        for n in b:gmatch('%d+') do bb[#bb+1]=tonumber(n) end
        for i=1,3 do if aa[i]~=bb[i] then return aa[i]<bb[i] and -1 or 1 end end
        return 0
    end
    -- What a guest must change to match the required packages, or nil.
    function P.mismatch(required,guest)
        local reasons={}
        for id,version in pairs(required) do
            if not guest[id] then reasons[#reasons+1]='Guest must enable '..wireName(id)..' '..version..'.'
            elseif guest[id]~=version then
                local side=P.compare(guest[id],version)<0 and 'Guest' or 'Host'
                reasons[#reasons+1]=side..' must update '..wireName(id)..' (host '..version..', guest '..guest[id]..').'
            end
        end
        table.sort(reasons);return #reasons>0 and table.concat(reasons,' ') or nil
    end
    -- A {wire id = version} map of public packages.
    function P.public_map(value)
        if type(value)~='table' then return false end
        local count=0
        for id,version in pairs(value) do
            if not P.packageOf[id] or type(version)~='string' or #version>32 or not version:match('^%d+%.%d+%.%d+$') then return false end
            count=count+1
        end
        return count<=4
    end
    -- A host's advert, or nil when it is not a readable protocol 3 advert.
    function P.parse_advert(bytes)
        local ok,v=pcall(json.decode,bytes)
        if not ok or type(v)~='table' or v.protocol~=3 or not P.public_map(v.packages) or not v.packages.core
            or not P.public_map(v.required) or type(v.vanilla)~='boolean' or type(v.require_modpack)~='boolean' then return nil end
        for id,version in pairs(v.required) do if id=='core' or v.packages[id]~=version then return nil end end
        return v
    end
    return P
end
