-- Pure-Lua invalidation. Observing a revision never dereferences a UObject.
local M={}
function M.new(L)
local E={revision=0}
-- Owned list pagination paints existing widgets. Do not wake world maintenance
-- for those clicks; mutations explicitly increment maps_shared.revision.
local passiveUI={}
function E.passiveUI(path,o)
    passiveUI[path]=passiveUI[path] or {}
    local key=L.key(o); if key then passiveUI[path][key]=true end
end
function E.passiveUiEvent(path,o)
    local list=passiveUI[path]
    return list and o and list[L.key(o)]==true or false
end
function E.clearPassiveUI() passiveUI={} end
function E.changed() E.revision=E.revision+1 end
function E.hot(path)
    -- Native UpdateIngamePresence repeats this pure lookup every five seconds.
    -- Keep its selection decision hook; the observation must not dirty world work.
    if path=='/Game/Blueprints/Match/B_MapManager.B_MapManager_C:GetSpecificMapFromLevelName' then return true end
    if path:match(':CheckTimerForMatchStart$') or path:match(':CheckRemainingSeconds$') or path:match(':ActivateUtilitySkillPressed$')
        or path:match(':PossessTrace$') or path:match(':Press$') or path:match(':InitializeRules$')
        or path:match(':ShowPossessNotWorkingHelp$') or path:match(':GetItemName$') then return true end
    -- Read-only POSS1 observations must not wake Creative/map reconciliation.
    if path=='/Game/Blueprints/Player/Pawns/B_Witch.B_Witch_C:ActivateUtilitySkillPressed'
        or path=='/Game/Blueprints/Player/Pawns/B_PropPawnPhysics.B_PropPawnPhysics_C:ActivateUtilitySkillPressed' then return true end
    if path=='/Game/Blueprints/Player/Libraries/B_WitchHuntFunctionLibrary.B_WitchHuntFunctionLibrary_C:PossessTrace'
        or path=='/Game/Blueprints/Skills/Witch/Possess/B_PossessSkill.B_PossessSkill_C:Press'
        or path=='/Game/Blueprints/Skills/Witch/Possess/B_PossessSkill.B_PossessSkill_C:InitializeRules'
        or path=='/Game/Blueprints/Skills/Witch/Possess/B_PossessSkill.B_PossessSkill_C:ShowPossessNotWorkingHelp' then return true end
    if path=='/Game/UserInterface/UIElements/WB_SearchBar.WB_SearchBar_C:GetItemName' then return true end
    return path:match(':UpdateChatMessagesPerSecond$') or path:match(':ReceiveTick$') or path:match(':Tick$') or path:match(':TickFillState$')
        or path:match(':CanActivate$') or path:match(':CheckForOverTime$') or path:match(':CheckWitchesAlive$') or path:match(':IsMatchDead$') or path:match(':IsMatchIntact$') or path:match(':GetHuntersCount$') or path:match(':Get_CreateMapCycle_Visibility_0$')
        or path:match(':GetQuantityText$') or path:match(':GetQuantityVisibility$')
        or path:match(':GetPlayernumberText$') or path:match(':Get_Playernumber_Text_0$')
        or path:match(':Get_Button_UserMaps_Visibility_0$')
        or path=="/Game/UserInterface/ObjectiveMode/WB_LifeBlock.WB_LifeBlock_C:GetVisibility_0"
        or path=="/Game/Blueprints/ScoreStats/B_ScoreStatHelper.B_ScoreStatHelper_C:AddHunterScoreStatChecked"
        -- Native Objective UI Tick calls UpdateStats; these hooks observe/paint
        -- cached lines and must not wake Creative world maintenance every frame.
        or path=="/Game/UserInterface/MatchResult/ObjectivemodeResult/WB_ObjectiveResultScreen.WB_ObjectiveResultScreen_C:UpdateStats"
        or path=="/Game/UserInterface/MatchResult/ObjectivemodeResult/WB_ObjectiveResultScreen.WB_ObjectiveResultScreen_C:GetWinReasonResultLine"
        or path=="/Game/UserInterface/MatchResult/ObjectivemodeResult/WB_ObjectiveResultScreen.WB_ObjectiveResultScreen_C:Get_Victory_Text_0"
        or path=="/Game/UserInterface/PlayerListAndTeamSelection/WB_TeamSelectionObjective.WB_TeamSelectionObjective_C:Get_BalanceTeamSelection_Text_0"
end
-- A module's own events plus construction/removal invalidate its cached work.
function E.forbiddenHook(path)
    return path:match(':UpdateChatMessagesPerSecond$') or path:match(':CanActivate$') or path:match(':PossessTrace$') or path:match(':ReceiveTick$') or path:match(':Tick$') or path:match(':TickFillState$')
        or path:match(':MarkPropertyDirtyFromRepIndex$') or path:match(':GetWorldSubsystem$') or path:match(':GetAllActorsOfClass$')
        or path:match(':UpdateStats$') or path:match(':GetWinReasonResultLine$') or path:match(':Get_Victory_Text_0$')
        or path:match(':AddHunterScoreStatChecked$') or path:match(':ConsumeOwnersMana$') or path:match(':CheckForOverTime$')
        or path:match(':GetVisibility_0$') or path:match(':Get_CreateMapCycle_Visibility_0$') or path:match(':Get_Button_UserMaps_Visibility_0$')
        or path:match(':Get_Playernumber_Text_0$') or path:match(':Get_BalanceTeamSelection_Text_0$')
        or path:match(':Get_Name_Text_0$') or path:match(':Get_Description_Text_0$')
end
function E.scope(H)
    local proxy=setmetatable({revision=0},{__index=H})
    function proxy.hook(path,cb,...)
        local args=table.pack(...)
        return H.hook(path,function(...)
            -- Native utility getters are observations, not state changes. Their
            -- subscribers filter arguments and repair directly when needed.
            if path:sub(1,6)=='/Game/' and not E.hot(path) then proxy.revision=proxy.revision+1 end
            return cb(...)
        end,table.unpack(args,1,args.n))
    end
    return proxy
end
function E.poll(api,H,Registry,pending)
    local poll=api.poll
    local seen,objects,enabled
    api.invalidate=function() seen=nil end
    api.poll=function(...)
        local revision=H.revision or E.revision
        if seen==revision and objects==Registry.revision and enabled==(H.C and H.C.CreativeMaps) and not (pending and pending()) then return end
        -- Consume before running: events raised by work schedule a later pass.
        seen,objects,enabled=revision,Registry.revision,H.C and H.C.CreativeMaps
        return poll(...)
    end
    local reset=api.reset
    api.reset=function(...) seen=nil; if reset then return reset(...) end end
end
return E
end
return M
