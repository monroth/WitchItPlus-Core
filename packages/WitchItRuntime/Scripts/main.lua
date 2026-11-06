local scripts=debug.getinfo(1,'S').source:match('^@(.+[\\/])')
local mods=scripts..'../../'
ExecuteInGameThreadAfterFrames(1,function()
    local R=dofile(scripts..'runtime.lua')()
    local function load(id,...)
        R.prepare_directory(id)
        local result=dofile(mods..id..'/package.lua')(...)
        R.log('WitchItRuntime','Loaded %s',id);return result
    end
    local C=load('Core',R)
    local U=load('UIKit',R,C)
    for _,id in ipairs({'FillAPotPlus','MatchRulesPlusPlus','BaseWorldPlus','PrivateCosmetics','CreativeUnlocks'}) do
        local manifest=io.open(mods..id..'/manifest.lua','rb')
        if manifest then
            manifest:close();local metadata=dofile(mods..id..'/manifest.lua')
            local enabled=metadata.enabled~=false
            if enabled then R.call(load,id,R,C,U) end
        end
    end
    R.log('WitchItRuntime','Started')
end)
