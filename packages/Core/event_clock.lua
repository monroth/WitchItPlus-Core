-- Game-thread clock: callback DeltaSeconds plus wall time, no UObject clock calls.
-- Wall-time changes also advance engine-paused deadlines/reconnect grace. A large
-- jump reanchors the fraction; backwards wall jumps never rewind a deadline.
local M={}
function M.new(wallClock)
local Clock={}
local wallTime=wallClock or os.time
local value,lastWall
function Clock.tick(delta)
    local wall=wallTime()
    if not value or wall~=lastWall then value=math.max(value or wall,wall)
    elseif type(delta)=='number' and delta>=0 and delta<10 then value=value+delta end
    lastWall=wall
end
function Clock.now() value=math.max(value or wallTime(),wallTime()); return value end
return Clock
end
return M
