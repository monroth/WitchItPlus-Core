-- Mods' rules saved with the game's own. Each native Save/Load rules file <name> has
-- Core/rules/<name>.json beside it, with one section per mod that registered.
return function(R,C)
    local PAGE='/Game/UserInterface/Play/GameRules/WB_SaveLoadGameRules.WB_SaveLoadGameRules_C:'
    local hooks=C.scoped(function(s) return s.mode~='vanilla' and s.role~='client' end)
    local owners={}
    local function path(name) return 'Core/rules/'..(type(name)=='string' and name or name:ToString())..'.json' end
    local function collect()
        local value={}
        for id,owner in pairs(owners) do value[id]=owner.save() end
        return value
    end
    local function load(value,except)
        for id,owner in pairs(owners) do
            if id~=except then owner.load(value[id]) end
        end
    end
    R.prepare_directory('Core/rules')
    hooks.post(PAGE..'OnSaved',function(_,name)
        -- A section of a mod that is not installed now stays for when it comes back.
        local value=C.files.read_json(path(name)) or {}
        for id,section in pairs(collect()) do value[id]=section end
        C.files.write_json(path(name),value)
    end)
    hooks.post(PAGE..'SaveFileLoaded',function(_,name) load(C.files.read_json(path(name)) or {}) end)
    hooks.post(PAGE..'ResetRules',function() load({}) end)
    -- owner.save() returns a mod's rules; owner.load(value) takes them, or nil for its defaults.
    -- collect and load also serve rules codes, which carry the same sections.
    return {register=function(id,owner) owners[id]=owner end,collect=collect,load=load}
end
