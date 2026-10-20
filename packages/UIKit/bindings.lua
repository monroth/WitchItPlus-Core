-- WitchIt-specific menu integration. Nothing is installed until explicitly bound.
local M={}
local PLAY='/Game/UserInterface/Play/'
local RULES=PLAY..'GameRules/'
function M.new(ui,api,E)
    local B={}
    local function binding(asset,folder,spec,build,refresh)
        assert(type(spec.build)=='function','binding requires build(scope, rows, screen)')
        assert(spec.enabled==nil or type(spec.enabled)=='function','binding enabled must be a predicate')
        local function enabled() return spec.enabled==nil or spec.enabled()==true end
        local handle={records={},pending={},births={},closed=false}
        local stops,paths={},{}
        local function retry_hooks()
            -- API 2 retries pending BP hooks after map load. An explicit ready /
            -- bootstrap can precede another map: briefly subscribe to request
            -- installation through the public API, then remove only the probe.
            for path in pairs(paths) do local cancel=api.hook(path,function() end); cancel() end
        end
        function handle.close()
            handle.closed=true
            for _,stop in ipairs(stops) do stop() end; stops={}
            for _,stop in pairs(handle.pending) do stop() end; handle.pending={}
            local errors={};handle.births={}
            for key,r in pairs(handle.records) do
                local done,why=r.scope.close()
                if done~=false then handle.records[key]=nil else errors[#errors+1]='binding '..asset..' '..key..': '..tostring(why) end
            end
            return #errors==0,table.concat(errors,' | ')
        end
        ui.own(handle.close)
        local function discard(key)
            local r=handle.records[key]; handle.records[key]=nil
            if r then r.scope.close() end
        end
        function handle.ready(w)
            if not enabled() or handle.closed or ui.transition or not api.is_active() or not E.valid(w) or not E.open(w) then return end
            retry_hooks()
            local key=E.key(w); if not key then return end
            local r=handle.records[key]
            if r and r.scope.closed then handle.records[key]=nil; r=nil end
            if r then
                local ok=pcall(r.attached)
                if not ok then discard(key); r=nil end
            end
            if not r then
                local scope=ui.scope(w)
                local ok,record=pcall(function()
                    local result=build(scope,w)
                    spec.build(scope,result.rows,w,result)
                    if spec.after_build then spec.after_build(scope,result,w) end
                    return result
                end)
                if not ok then scope.close(); api.log('UI binding skipped '..asset..': '..tostring(record)); return end
                r=record; r.scope=scope; r.screen=w; handle.records[key]=r
            end
            local ok,err=pcall(refresh,r,w)
            if not ok then discard(key); api.log('UI binding refresh failed '..asset..': '..tostring(err)) end
            return r
        end
        local function schedule(self)
            if not enabled() or ui.transition then return end
            local w=E.unwrap(self); local key=E.key(w)
            if handle.closed or not key or handle.pending[key] then return end
            -- Native POST subscribers finish before injection; one pending job/screen.
            handle.pending[key]=api.delay(1,function()
                handle.pending[key]=nil
                handle.ready(w)
            end)
        end
        local function hook(fn,callback,options)
            local path=E.path(asset,fn,folder); paths[path]=true
            stops[#stops+1]=api.hook(path,function(...)
                if not enabled() or ui.transition then return end
                return callback(...)
            end,options)
        end
        stops[#stops+1]=api.notify(folder..asset..'.'..asset..'_C',function(w)
            -- No reflection for disabled bindings or during native travel teardown.
            if not enabled() or ui.transition then return end
            -- Birth is identity only. Construct/ready events permit mutation.
            local key=E.key(w); if key then handle.births[key]=true end
            if spec.birth_ready then schedule(w) end
        end)
        hook('Construct',schedule)
        -- These menus inherit Destruct; no child UFunction exists in the cooked dumps.
        stops[#stops+1]=api.hook('/Script/UMG.UserWidget:Destruct',function(self)
            if ui.transition then return end -- world reset already canceled/dropped records
            local key=E.key(E.unwrap(self)); if not key then return end
            if handle.pending[key] then handle.pending[key](); handle.pending[key]=nil end
            handle.births[key]=nil; discard(key)
        end,{ungated=true})
        for _,fn in ipairs(spec.events or {}) do hook(fn,schedule) end
        local function travel_begin()
            for _,stop in pairs(handle.pending) do stop() end; handle.pending={}
            handle.records={}; handle.births={} -- ui's pre-travel listener closes scopes
        end
        stops[#stops+1]=api.on('load_map_pre',travel_begin,{ungated=true})
        stops[#stops+1]=api.on('world_changed',travel_begin,{ungated=true})
        function handle.resync()
            if not enabled() or ui.transition then return end
            -- Explicit bootstrap for enabling with an already-open menu. Never a loop.
            for _,w in ipairs(E.values(api.find_all(asset..'_C'))) do handle.ready(w) end
        end
        handle.hook=hook
        return handle
    end
    -- Native menu anchors are cooked bound children; the panel remains in the
    -- existing menu session. No ChangeMenu, subordinate stack or input-mode switch.
    function B.menu(spec)
        local settings={}; for k,v in pairs(spec) do settings[k]=v end
        settings.birth_ready=true
        return binding(spec.asset,spec.folder,settings,function(s,w)
            local anchor=E.need(spec.anchor(w),'Mods menu anchor')
            local parent=spec.parent and anchor or E.call(anchor,'GetParent')
            if spec.direct_entry then
                local r={rows=E.need(parent,'Mods menu parent'),anchor=anchor}
                r.attached=function()
                    assert(E.same(E.call(E.need(r.widget,'Mods entry'),'GetParent'),parent),'Mods entry detached')
                end
                return r
            end
            local root=ui.section(s,E.need(parent,'Mods menu parent'))
            return {widget=root,rows=root,attached=function()
                assert(E.same(E.call(root,'GetParent'),parent),'Mods entry detached')
            end}
        end,function(r) r.attached() end)
    end
    function B.custom_match(spec)
        local settings={}; for k,v in pairs(spec) do settings[k]=v end
        settings.events={'UpdateWidgetVisibility','SetGameModeDefaultValues'}
        local function matches(w)
            if type(spec.mode)=='function' then return spec.mode(w,api.context())==true end
            assert(type(spec.mode)=='string','custom_match needs native mode class name or predicate')
            local options=E.read(w,'NextMatchOptions'); local mode=options and E.read(options,'GameMode')
            return E.valid(mode) and E.text(E.call(mode,'GetFName'))==spec.mode
        end
        return binding('WB_Custommatch',PLAY,settings,function(s,w)
            local anchor=spec.anchor and spec.anchor(w) or w.Slider_WitchLives
            local parent=E.call(E.need(anchor,'Custom Match anchor'),'GetParent')
            local root=ui.section(s,parent,spec.title)
            local r={widget=root,rows=root}
            s.enabled=function() return matches(w) end
            r.attached=function() assert(E.same(E.call(root,'GetParent'),parent),'custom section detached') end
            return r
        end,function(r,w)
            r.attached(); E.call(r.widget,'SetVisibility',matches(w) and 0 or 1)
        end)
    end
    function B.game_rules_tab(spec)
        local settings={}; for k,v in pairs(spec) do settings[k]=v end
        settings.events={'PreConstruct'}
        local handle=binding('WB_GameRulesTabs',RULES,settings,function(s,w)
            local tab=ui.tab(s,w,spec)
            s.enabled=function() return E.same(E.call(w.RulesSwitcher,'GetActiveWidget'),tab.widget) end
            tab.attached=tab.reconcile
            return tab
        end,function(r) r.reconcile() end)
        local function active(self)
            local w=E.unwrap(self); local r=handle.records[E.key(w)]
            if r and not r.scope.closed and E.valid(r.widget) and E.open(w)
                and E.same(E.call(w.RulesSwitcher,'GetActiveWidget'),r.widget) then return r end
        end
        handle.hook('GetInitialFocusWidget',function(self,out)
            local r=active(self)
            -- Reflected OUT is UserWidget. Caller supplies a cooked row, not TextBox.
            local target=r and r.focus_target()
            if target and E.valid(target) then
                assert(E.call(target,'IsA',E.class('/Script/UMG.UserWidget')),'initial_focus must be UserWidget')
                out:set(target)
            end
        end,{out_params={2}})
        for _,fn in ipairs({'SetActiveTab','HandleRefocus'}) do
            handle.hook(fn,function(self) local r=active(self); if r then r.focus() end end)
        end
        return handle
    end
    B.match_rules_tab=B.game_rules_tab
    return B
end
return M
