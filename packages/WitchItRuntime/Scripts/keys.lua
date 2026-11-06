return function(R,controller)
    local settings=R.settings('WitchItRuntime',{hotkeys={}})
    local bindings={}
    local capture
    function R.capture_hotkey(fn)
        capture=fn
        return function() if capture==fn then capture=nil end end
    end
    local names={SPACE=32,SPACEBAR=32,TAB=9,ENTER=13,ESCAPE=27,INSERT=45,DELETE=46,
        HOME=36,END=35,PAGEUP=33,PAGEDOWN=34,LEFTMOUSEBUTTON=1,RIGHTMOUSEBUTTON=2,MIDDLEMOUSEBUTTON=4}
    for name,code in pairs({BACKSPACE=8,PAUSE=19,CAPSLOCK=20,LEFT=37,UP=38,RIGHT=39,DOWN=40,
        LEFTSHIFT=160,RIGHTSHIFT=161,LEFTCONTROL=162,RIGHTCONTROL=163,LEFTALT=164,RIGHTALT=165,
        SEMICOLON=186,EQUALS=187,COMMA=188,HYPHEN=189,PERIOD=190,SLASH=191,TILDE=192,
        LEFTBRACKET=219,BACKSLASH=220,RIGHTBRACKET=221,APOSTROPHE=222})do names[name]=code end
    for i,name in ipairs({'ZERO','ONE','TWO','THREE','FOUR','FIVE','SIX','SEVEN','EIGHT','NINE'})do names[name]=i+47 end
    for i=48,90 do names[string.char(i)]=i end
    for i=1,24 do names['F'..i]=111+i end
    local mods={fap='Fill a Pot+',mrpp='Match Rules ++',bwp='Editor tweaks'}
    local labels={['fap.pause']='Pause or resume the round timer',['mrpp.rules']='Open Match Rules ++',
        ['bwp.select']='Hide tool: aim mode on/off',['bwp.hide']='Hide tool: hide aimed object',['bwp.undo']='Hide tool: undo last hide'}
    local contexts={['fap.pause']='Fill a Pot+ match (host)', ['mrpp.rules']='Custom Match, lobby, Creative',
        ['bwp.select']='Creative', ['bwp.hide']='Creative', ['bwp.undo']='Creative'}
    local function parse(chord)
        if chord=='Disabled' then return nil end
        local code,mask=nil,0
        for part in chord:upper():gmatch('[^+]+') do
            if part=='CTRL' or part=='CONTROL' then mask=mask|2
            elseif part=='SHIFT' then mask=mask|1
            elseif part=='ALT' then mask=mask|4
            else code=names[part];if not code then return nil,nil,'Unknown key' end end
        end
        if code==116 then return nil,nil,'F5 belongs to the game.' end
        if not code then return nil,nil,'Choose a key.' end
        return code,mask
    end
    function R.key(id,chord,fn)
        local current=settings.hotkeys[id] or chord
        local code,mask,why=parse(current);assert(not why,why)
        settings.hotkeys[id]=current
        bindings[id]={id=id,mod=mods[id:match('^([^.]+)')] or id,label=labels[id] or id,
            default=chord,current=current,code=code,mask=mask,fn=fn}
    end
    function R.hotkeys()
        local rows={}
        for _,b in pairs(bindings) do rows[#rows+1]={id=b.id,mod=b.mod,label=b.label,context=contexts[b.id],default=b.default,current=b.current} end
        table.sort(rows,function(a,b) return a.id<b.id end);return rows
    end
    function R.set_hotkey(id,chord)
        local b=assert(bindings[id],'Unknown hotkey')
        local code,mask,why=parse(chord);if why then return false,why end
        if code then for other,row in pairs(bindings) do
            if other~=id and row.code==code and row.mask==mask then return false,'Already used by '..row.mod..': '..row.label end
        end end
        b.current,b.code,b.mask=chord,code,mask;settings.hotkeys[id]=chord
        R.save_settings('WitchItRuntime');return true
    end
    R.native.on_input(function(code,mask)
        if capture then capture(code,mask);return end
        if not controller() then return end
        for _,binding in pairs(bindings) do
            if code==binding.code and mask==binding.mask then R.call(binding.fn) end
        end
    end)
end
