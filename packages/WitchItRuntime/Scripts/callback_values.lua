local M={}
function M.kind(value)
    local ok,kind=pcall(function() return value:type() end)
    if ok and type(kind)=='string' then return kind end
    local printed,text=pcall(tostring,value)
    if printed and type(text)=='string' and text:match('^TrivialObject') then return 'TrivialObject' end
end
function M.unwrap(value)
    if type(value)=='userdata' then
        local kind=M.kind(value)
        if kind=='RemoteUnrealParam' or kind=='LocalUnrealParam' then return value:get() end
    elseif type(value)=='table' then
        local get=rawget(value,'get')
        if type(get)=='function' then return get(value) end
    end
    return value
end
return M
