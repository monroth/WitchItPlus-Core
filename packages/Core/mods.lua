local dir=debug.getinfo(1,'S').source:match('^@(.+[\\/])')
return function(R,C,T,session_policy)
    local M={peers={}}
    local listeners={}
    function M.on_change(fn)listeners[fn]=true;return function()listeners[fn]=nil end end
    local function changed()for fn in pairs(listeners)do fn()end end
    local P=dofile(dir..'modpack.lua')(R.json)
    local names,wires,packageOf=P.names,P.wires,P.packageOf
    local mismatch,public_map,parse_advert=P.mismatch,P.public_map,P.parse_advert
    M.names,M.wires,M.packageOf,M.compare,M.parse_advert=names,wires,packageOf,P.compare,parse_advert
    local installed,packages={},{}
    local config=R.settings('Core',{VanillaServer=true,CreativeVanillaServer=false,RequireModpack=true,KickAfterSeconds=10,auto_update=true})
    local private=false
    for _,id in ipairs({'Core','UIKit','FillAPotPlus','MatchRulesPlusPlus','BaseWorldPlus','PrivateCosmetics','CreativeUnlocks'}) do
        local path=dir..'../'..id..'/manifest.lua'
        local file=io.open(path,'rb')
        if file then
            file:close();local manifest=dofile(path)
            local infrastructure=id=='Core' or id=='UIKit'
            local enabled=infrastructure or manifest.enabled~=false
            installed[#installed+1]={id=id,name=names[id],version=manifest.version,enabled=enabled,required=infrastructure}
            if wires[id] and enabled then packages[wires[id]]=manifest.version end
            if id=='PrivateCosmetics' or id=='CreativeUnlocks' then private=true end
        end
    end
    local gp=StaticFindObject('/Script/Engine.Default__GameplayStatics')
    local identity={protocol=3,packages=packages,required={},vanilla=true,require_modpack=false}
    M.identity=identity
    local hosting,host_vanilla,host_setting=false,true,'VanillaServer'
    local remote
    local peers={}
    M.config=config
    local function compatibility(p)
        local message
        if identity.vanilla then message=nil
        elseif p.packages then message=mismatch(identity.required,p.packages) end
        p.message=message
        M.peers[p.pc:GetAddress()]={name=p.name,compatible=identity.vanilla or p.packages and message==nil,message=message}
        changed()
    end
    local function cancel(p)
        if p.deadline then p.deadline();p.deadline=nil end
    end
    local function enforcing()
        return hosting and not identity.vanilla and config.RequireModpack
    end
    local installation='This server requires the Witch It+ modpack.\nGet it at https://github.com/monroth/WitchItPlus-Core'
    local function kick(p)
        if p.kicked or not enforcing() then return end
        cancel(p);p.kicked=true
        R.log('Core','Kick %s',p.name)
        local reason=p.packages and mismatch(identity.required,p.packages) or installation
        if p.packages then
            p.pc:ClientSetRevisionNumber('WIR2:K:'..reason)
            gp:GetGameState(p.pc):SendChatMessageMulticast(FText(p.name..': '..reason),p.pc.PlayerState,0,{})
        else
            p.pc:ClientSetWasKicked(FName('None'))
        end
        local lib=StaticFindObject('/Script/PropWitchHuntModule.Default__ProjectVersionBlueprint')
        lib:KickPlayer(gp:GetGameMode(p.pc),p.pc,FText(reason))
    end
    local function clear_peers()
        for _,p in pairs(peers) do cancel(p) end
        peers,M.peers={},{}
        changed()
    end
    local function peer(pc)
        local key=pc:GetAddress()
        local p=peers[key]
        if not p then
            local connection=pc.NetConnection
            local name=pc.PlayerState:GetPlayerName()
            p={pc=pc,transport=connection,name=type(name)=='string' and name or name:ToString()}
            peers[key]=p;compatibility(p)
        end
        return p
    end
    local function announce(pc)
        T.send(pc,'core.identity',identity)
    end
    local function correct_rate(p)
        local connection=p.transport
        if connection.CurrentNetSpeed~=20000 then return end
        local driver=connection.Driver
        local cap=math.min(100000,driver.MaxClientRate,driver.MaxInternetClientRate)
        if cap>20000 then connection.CurrentNetSpeed=cap end
    end
    local PREFIX='[MODDED] '
    local function clean_name(name)
        while name:sub(1,#PREFIX)==PREFIX do name=name:sub(#PREFIX+1) end
        return name
    end
    local tagged=false
    local function advertise(settings)
        identity.require_modpack=not identity.vanilla and config.RequireModpack
        if identity.vanilla and not tagged then return end
        tagged=not identity.vanilla
        local entries=settings.Settings
        local key=FName('SessionName')
        if entries:Contains(key) then
            local old=entries:Find(key):get();local value=old.Data.AsString
            local name=type(value)=='string' and value or value:ToString()
            local desired=(identity.vanilla and '' or PREFIX)..clean_name(name)
            if desired~=name then entries:Add(key,{Data={Type=6,AsString=desired},AdvertisementType=3,ID=old.ID}) end
        end
        if not identity.vanilla then
            entries:Add(FName('WitchItModpack'),{Data={Type=6,AsString=R.json.encode(identity)},AdvertisementType=3,ID=0})
        elseif entries:Contains(FName('WitchItModpack')) then
            entries:Remove(FName('WitchItModpack'))
        end
    end
    local SESSION='/Script/OnlineSubsystemBlueprints.'
    R.pre(SESSION..'OnlineSessionSubsystemCreateSession:CreateSession',function(_,subsystem,pc,name,settings)
        hosting=true;remote=nil
        -- Both hosting dialogs put the options being hosted in UsedMatchOptions before HostGame.
        local options=gp:GetGameInstance(subsystem).UsedMatchOptions
        local mode=options.GameMode:GetFName():ToString()
        host_setting=mode=='B_CreativeGameMode_C' and 'CreativeVanillaServer' or 'VanillaServer'
        host_vanilla=config[host_setting]
        local required={}
        if not host_vanilla or packages.fillapotplus and mode=='B_ObjectiveGameMode_C' and C.is_plus(options) then
            for _,id in ipairs({'fillapotplus','matchrulesplusplus','baseworldplus'}) do required[id]=packages[id] end
        end
        identity={protocol=3,packages=packages,required=required,vanilla=next(required)==nil,
            require_modpack=next(required)~=nil and config.RequireModpack}
        M.identity=identity;session_policy(identity.vanilla)
        advertise(settings)
    end)
    R.pre(SESSION..'OnlineSessionSubsystemUpdateSession:UpdateSession',function(_,subsystem,name,settings)
        if not hosting then return end
        advertise(settings)
    end)
    R.pre(SESSION..'OnlineSessionSubsystemJoinSession:JoinSession',function(_,subsystem,pc,name,result)
        hosting=false
        local entries=result.Session.SessionSettings.Settings
        remote=nil;M.message=nil
        if entries:Contains(FName('WitchItModpack')) then
            local value=entries:Find(FName('WitchItModpack')):get().Data.AsString
            remote=parse_advert(type(value)=='string' and value or value:ToString())
            if not remote then M.message='This server uses a different modpack version. Update the modpack and restart the game.' end
        end
        if remote then M.message=mismatch(remote.required,packages) end
        session_policy(not remote or remote.vanilla)
    end)
    R.post('/Game/UserInterface/WB_MainMenu.WB_MainMenu_C:Construct',function()
        hosting,remote,M.message=false,nil,nil
        clear_peers()
        session_policy(true)
    end)
    C.net.watch('core.identity',function(value)
        if hosting then return end
        local parsed=parse_advert(R.json.encode(value))
        if not parsed then M.message='The host uses a different modpack version. Update and restart the game.';return end
        remote=parsed
        session_policy(parsed.vanilla)
        M.message=mismatch(parsed.required,packages)
        changed()
    end)
    C.net.on_request('core.inventory',function(ps,value)
        if not hosting or not R.valid(ps) or not public_map(value) or not value.core then return end
        local pc=R.valid(ps:GetOwner());if not pc or pc:IsLocalPlayerController() then return end
        local p=peer(pc);p.packages=value
        compatibility(p)
        R.log('Core','Modpack from %s: %s',p.name,p.message and 'mismatch' or 'ok')
        if p.message then kick(p) else cancel(p) end
        if not identity.vanilla and not p.kicked then
            T.add(pc)
            announce(pc)
            T.replay(pc)
            correct_rate(p)
        end
    end)
    -- The match as a join or a leave found it, for the log.
    local function moment(pc)
        local gs=C.session().gs
        return string.format('state=%s round=%s kicked=%s',gs and tostring(gs.State),gs and tostring(gs.CurrentRound),
            tostring(pc.IsBeingKickedDueMatchHasStarted))
    end
    local function login(ps)
        if not hosting or not R.valid(ps) then return end
        local pc=R.valid(ps:GetOwner())
        if not pc or pc:IsLocalPlayerController() then return end
        R.log('Core','Join %s %s',peer(pc).name,moment(pc))
    end
    R.on('player_joined',login)
    R.on('player_left',function(ps)
        if not R.valid(ps) then return end
        local pc=R.valid(ps:GetOwner());if not pc then return end
        local p=peers[pc:GetAddress()];if p then cancel(p);R.log('Core','Left %s %s',p.name,moment(pc)) end
        peers[pc:GetAddress()],M.peers[pc:GetAddress()]=nil,nil
        T.remove(pc)
        changed()
    end)
    local creative_pc='/Game/Blueprints/Gamemodes/CreativeMode/B_CreativePlayerController.B_CreativePlayerController_C'
    local function ready(pc,possessed)
        if not hosting or not R.valid(pc) or not pc:HasAuthority() or pc:IsLocalPlayerController() or identity.vanilla then return end
        -- Creative's game-message widget is constructed by the possessed spectator.
        -- Its initial null-pawn acknowledgement is an unpossession, not that event.
        if pc:IsA(creative_pc)~=possessed then return end
        local p=peer(pc)
        if p.kicked then return end
        if not p.loaded then p.loaded=true;R.log('Core','World loaded by %s (modpack %s)',p.name,p.packages and 'known' or 'not yet') end
        if p.packages and not p.message then correct_rate(p) end
        if enforcing() and not p.started and not p.packages then
            p.started=true
            -- Native nameless announcement: no player sender, chat-enabled, mute or block filters.
            local notice=installation..'\nYou will be kicked in '..config.KickAfterSeconds..' seconds.'
            gp:GetGameState(pc):SendGameMessageMulticast(FText(notice),FText(''),12)
            R.log('Core','Installation notice %s (%ds)',p.name,config.KickAfterSeconds)
            p.deadline=R.after(config.KickAfterSeconds*1000,function()
                p.deadline=nil;kick(p)
            end)
        end
    end
    R.post('/Script/Engine.PlayerController:ServerShortTimeout',function(pc) ready(pc,false) end)
    R.post('/Script/Engine.PlayerController:ServerNotifyLoadedWorld',function(pc,worldPackage,finalDestination)
        if finalDestination then ready(pc,false) end
    end)
    R.post('/Script/Engine.PlayerController:ServerAcknowledgePossession',function(pc,pawn)
        if R.valid(pawn) then ready(pc,true) end
    end)
    R.on('local_player',function(pc)
        if not pc:HasAuthority() and not C.session().is_vanilla_session then T.request(pc,'core.inventory',packages) end
    end)
    R.post('/Game/Blueprints/Player/B_PlayerController.B_PlayerController_C:ClientSetRevisionNumber',function(pc,text)
        text=type(text)=='string' and text or text:ToString()
        if not pc:HasAuthority() and pc:IsLocalPlayerController() and text:sub(1,7)=='WIR2:K:' then M.refusal=text:sub(8) end
    end)
    R.on('world_end',clear_peers)
    local function update_settings()
        local enabled={}
        for _,row in ipairs(installed) do
            local selected=dofile(dir..'../'..row.id..'/manifest.lua').enabled~=false
            if wires[row.id] and (row.required or selected) then enabled[row.id]=true end
        end
        WitchItNativeUpdateFile('settings',R.json.encode({auto_update=config.auto_update,enabled_packages=enabled}))
    end
    local settings_signature=R.json.encode(config)
    function M.status()
        local signature=R.json.encode(config)
        if signature~=settings_signature then
            settings_signature=signature
            if not private then update_settings() end
        end
        local rows={};local restart=false
        for _,row in ipairs(installed) do
            local metadata=dofile(dir..'../'..row.id..'/manifest.lua')
            local next_enabled=metadata.enabled~=false
            next_enabled=row.required or next_enabled
            rows[#rows+1]={id=row.id,name=row.name,version=row.version,enabled=row.enabled,next_enabled=next_enabled,required=row.required}
            if row.enabled~=next_enabled then restart=true end
        end
        local bytes=WitchItNativeUpdater('status')
        return {packages=rows,identity=identity,host=remote,peers=M.peers,message=M.message,settings=config,
            restart_required=restart,update=bytes and R.json.decode(bytes),private=private,
            rehost_required=hosting and config[host_setting]~=host_vanilla}
    end
    local function check()
        update_settings()
        local result=WitchItNativeUpdater('check')
        M.downloading=true
        R.native.on_update(function() M.downloading=false;changed() end)
        return result
    end
    -- The running download's package, bytes done and total, or nil between files.
    function M.progress()
        local bytes=WitchItNativeUpdater('progress')
        return bytes and R.json.decode(bytes)
    end
    function M.request(kind,host_packages)
        if private then return false,'Use the local rebuild and install process to update this private build.' end
        if kind=='check' then return check() end
        local selected={}
        if kind=='exact' then
            if host_packages then selected=host_packages
            else
                for id,version in pairs(remote.packages) do selected[packageOf[id]]=version end
            end
        end
        update_settings()
        WitchItNativeUpdateFile('request',R.json.encode({request=kind,packages=selected,created_utc=os.date('!%Y-%m-%dT%H:%M:%SZ')}))
        check()
        changed()
        return true,'Downloading the update. You can keep playing.'
    end
    -- Reaching the menu confirms an update on trial and starts the one check of this launch.
    if not private then
        local checked=false
        R.post('/Game/UserInterface/WB_MainMenu.WB_MainMenu_C:Construct',function()
            if not checked then checked=true;M.request('check') end
        end)
    end
    return M
end
