-- Engine class/world queries through the existing host reflection/marshaling.
-- The engine chooses live instances; no generic object inventory enters Lua.
local Values=dofile(assert(debug.getinfo(1,'S').source:match('^@(.+[\\/])'))..'callback_values.lua')
local M={}
function M.new(native)
    local function object(path)
        local o=native('StaticFindObject',path)
        return o and o:IsValid() and o or nil
    end
    return function(path,context)
        assert(type(path)=='string','world query requires a class path')
        if not context or not context:IsValid() then return {} end
        local class=object(path)
        if not class then return {} end -- Only resident classes; never load here.
        local result={}
        if class:IsChildOf(assert(object('/Script/Engine.Actor'),'Actor class unavailable')) then
            assert(object('/Script/Engine.Default__GameplayStatics'),'GameplayStatics unavailable')
                :GetAllActorsOfClass(context,class,result)
        elseif class:IsChildOf(assert(object('/Script/UMG.UserWidget'),'UserWidget class unavailable')) then
            assert(object('/Script/UMG.Default__WidgetBlueprintLibrary'),'WidgetBlueprintLibrary unavailable')
                :GetAllWidgetsOfClass(context,result,class,false)
        elseif class:IsChildOf(assert(object('/Script/Engine.GameInstance'),'GameInstance class unavailable')) then
            local gi=assert(object('/Script/Engine.Default__GameplayStatics')):GetGameInstance(context)
            if gi and gi:IsValid() and gi:IsA(class) then result[1]=gi end
        end
        -- Reflected UObject-array OUT elements are RemoteUnrealParam wrappers.
        -- Resolve them here, before lifetime checks or registry admission.
        for index,value in ipairs(result) do result[index]=Values.unwrap(value) end
        return result
    end
end
return M
