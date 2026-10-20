local tree=IterateGameDirectories()
-- No metadata access on the parent stack before the child's first lookup.
local co=coroutine.create(function()
    assert(tree.Game.Content.Paks.LogicMods.__name=='LogicMods','ROUND6 first SDK metadata callback failed')
    coroutine.yield()
    assert(type(tree.Game.Content.Paks.LogicMods.__files)=='table','ROUND6 resumed metadata callback failed')
end)
local ok,why=coroutine.resume(co);assert(ok,why)
ok,why=coroutine.resume(co);assert(ok,why)
assert(coroutine.status(co)=='dead')
local checks=dofile(GateSource..'/source/RE-UE4SS/tests/LuaHost/RealBindings/checks.lua');checks.run()
main_done=true

ModRef.OnUnload=checks.unload
