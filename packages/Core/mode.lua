-- The single answer to "which mode is this session in". Every package scope reads it.
-- isPlus(options) tells whether those MatchOptions carry the Fill a Pot+ choice;
-- hostMode is the mode the host published, as seen by a client.
return function(s,isPlus,hostMode)
    if s.role~='offline' and s.is_vanilla_session then return 'vanilla' end
    if s.is_creative then return s.is_vanilla_session and 'vanilla' or 'creative' end
    if s.role=='offline' then return 'none' end
    if s.role=='client' then return hostMode or 'vanilla' end
    if s.gs and s.gs:GetClass():GetFName():ToString()=='B_ObjectiveGameState_C' and isPlus(s.gi.UsedMatchOptions) then return 'fap+' end
    return s.mode_id or 'none'
end
