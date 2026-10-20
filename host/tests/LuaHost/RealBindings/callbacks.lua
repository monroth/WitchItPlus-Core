local checks=dofile(GateSource..'/source/RE-UE4SS/tests/LuaHost/RealBindings/checks.lua')
checks.run();main_done=true
event_count=0;tick_count=0;frame_count=0;async_count=0
ExecuteInGameThread(function()checks.run();event_count=event_count+1 end,EGameThreadMethod.ProcessEvent)
ExecuteInGameThread(function()checks.run();tick_count=tick_count+1 end,EGameThreadMethod.EngineTick)
local handle=ExecuteInGameThreadAfterFrames(1,function()checks.run();frame_count=frame_count+1 end)
assert(math.type(handle)=='integer' and IsValidDelayedActionHandle(handle))
ExecuteAsync(function()checks.run();async_count=async_count+1 end)
ExecuteWithDelay(1,function()checks.run();async_count=async_count+1 end)
LoopAsync(1,function()checks.run();async_count=async_count+1;return true end)
RegisterConsoleCommandHandler('round6',function()checks.run();command_count=(command_count or 0)+1;return true end)
ModRef.OnUnload=checks.unload
