local checks=dofile(GateSource..'/source/RE-UE4SS/tests/LuaHost/RealBindings/checks.lua')
checks.run()
ModRef.OnUnload=checks.unload
main_done=true
