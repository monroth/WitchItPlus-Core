return function(R)
    local raw=WitchItNativePrimitives()
    local N={}
    local saves,loads={},{}
    local function subscription(op,...)
        local id=raw(op,...)
        return function() raw('cancel',id) end
    end
    function N.intercept(path,fn) return subscription('intercept',path,fn) end
    function N.on_asset(path,fn) return subscription('await_asset',path,fn) end
    function N.on_save(fn) saves[#saves+1]=fn end
    function N.on_load(fn) loads[#loads+1]=fn end
    function N.on_input(fn) return subscription('input',fn) end
    function N.on_update(fn) return subscription('await_update',fn) end
    function N.on_overlap(actor,fn) return subscription('overlap',actor,fn) end
    function N.load_asset(path)
        local resident=R.valid(StaticFindObject(path))
        if resident then return resident end
        return raw('load_asset',path)
    end
    function N.retain(object)
        local id=raw('retain',object)
        return function() raw('unretain',id) end
    end
    function N.spawn(class,world,p,initialize)
        if type(class)=='string' then class=raw('load_class',class,world) end
        local init=initialize and function(actor) initialize(actor);return true end or nil
        return raw('spawn',class,world,p.X,p.Y,p.Z,p.Yaw or 0,p.Pitch or 0,p.Roll or 0,
            p.ScaleX or 1,p.ScaleY or 1,p.ScaleZ or 1,init)
    end
    function N.destroy(actor) return raw('destroy',actor) end
    function N.capsule_probe(context,ignored,a,b,radius,height)
        local blocked,penetrating,x,y,z,nx,ny,nz=raw('respawn_probe',context,ignored,a.X,a.Y,a.Z,b.X,b.Y,b.Z,radius,height)
        return {blocked=blocked,penetrating=penetrating,position=blocked and {X=x,Y=y,Z=z},normal=blocked and {X=nx,Y=ny,Z=nz}}
    end
    function N.skin_rpc(pawn,ids)
        if ids then
            local parts={};for i,id in ipairs(ids) do parts[i]=id.type..':'..id.name end
            return raw('skin_rpc',pawn,table.concat(parts,'|'))
        end
        local result={}
        for kind,name in raw('skin_rpc',pawn):gmatch('([^:|]+):([^|]+)') do result[#result+1]={type=kind,name=name} end
        return result
    end
    function N.clipboard_get() return WitchItNativeClipboard('read') end
    function N.clipboard_set(bytes) return WitchItNativeClipboard('write',bytes) end
    function N.save_load(save,load)
        assert(WitchItNativeProducerAttach(function(phase,serial,owner,requested,compressed,autosave,target,success,...)
            local event={phase=phase,serial=serial,owner=owner,target_path=target,autosave=autosave,
                complete=phase=='complete',success=success}
            for _,fn in ipairs(saves) do fn(event) end
            return save(phase,serial,owner,requested,compressed,autosave,target,success,...)
        end,function(phase,serial,owner,target,compressed,recovery,intent,reader_done,reader_success,applied,...)
            local event={phase=phase,serial=serial,owner=owner,target_path=target,
                complete=phase=='return',success=reader_done and reader_success and applied}
            for _,fn in ipairs(loads) do fn(event) end
            return load(phase,serial,owner,target,compressed,recovery,intent,reader_done,reader_success,applied,...)
        end),'Save/load callbacks could not attach')
    end
    function N.teleporter_pair(a,b) return raw('teleporter_pair',a,b) end
    function N.teleporter_exit(zone,exit) return raw('teleporter_exit',zone,exit) end
    function N.source_actors(rows,world,states)
        local result=raw('source_actors',world,#rows,function(i)
            local row=rows[i];local t=row.transform
            local names={};for _,component in ipairs(row.components or {}) do names[#names+1]=component.name end
            local values={row.source,row.class,row.role,row.parent and row.parent.source or '',row.parent and row.parent.component or ''}
            for j=1,9 do values[5+j]=t[j] end
            values[15]=table.concat(names,'|');values[16]=R.json.encode(states[row.state],1250000)
            return table.unpack(values,1,16)
        end)
        if result==false then return false end
        local actors={};local i=1
        while result[tostring(i)] do actors[i]=result[tostring(i)];i=i+1 end
        return actors
    end
    return N
end
