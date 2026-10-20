-- Data/schema mechanics only; controls, admission/lives and next-match policy stay in FAPP.
local M={}
function M.timing(rule)
    return rule.restart and 'after restart' or rule.nextMatch and 'next match' or 'applies now'
end
function M.new(api,defaults,schema)
    for _,rule in pairs(schema) do
        if rule.step then assert(type(rule.step)=='number' and rule.step>0 and rule.step<math.huge,'invalid setting step') end
        if rule.precision then assert(type(rule.precision)=='number' and rule.precision%1==0
            and rule.precision>=0 and rule.precision<=10,'invalid setting precision') end
    end
    local values=api.config(defaults); local S={revision=0}
    local function valid(key,value)
        local rule=schema[key]; if not rule then return false end
        if rule.validate then return rule.validate(value)==true end
        if rule.kind=='bool' then return type(value)=='boolean' end
        if rule.kind=='number' then return type(value)=='number' and value==value and math.abs(value)<math.huge
            and (not rule.min or value>=rule.min) and (not rule.max or value<=rule.max) end
        if rule.kind=='choice' then for _,v in ipairs(rule.choices) do if v==value then return true end end end
        return false
    end
    for key,value in pairs(values) do
        if not valid(key,value) then
            values[key]=defaults[key]; api.log('ignored invalid setting '..tostring(key))
        end
    end
    function S.get(key) return values[key] end
    function S.set(key,value)
        if not valid(key,value) then return false end
        local rule=schema[key]
        if rule.step and type(value)=='number' then
            value=math.floor(value/rule.step+.5)*rule.step
            if rule.precision then value=tonumber(string.format('%.'..rule.precision..'f',value)) end
            if not valid(key,value) then return false end
        end
        if values[key]~=value then values[key]=value; S.revision=S.revision+1 end
        return true
    end
    function S.snapshot() return api.json.decode(api.json.encode(values)) end
    function S.save() return api.save_settings(values) end
    return S
end
return M
