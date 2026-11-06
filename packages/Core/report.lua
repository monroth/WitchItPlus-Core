-- A bug report: the logs, recent crash reports and mod settings in one zip archive.
-- Plain Lua: the game also runs under Proton, with no archiver to call.
return function(R)
    local table32={}
    for i=0,255 do
        local c=i
        for _=1,8 do c=(c>>1)~((c&1)==1 and 0xedb88320 or 0) end
        table32[i]=c
    end
    local function crc32(bytes)
        local c=0xffffffff
        for i=1,#bytes do c=table32[(c~bytes:byte(i))&255]~(c>>8) end
        return c~0xffffffff
    end
    -- Stored entries: the archive is small and needs no compressor.
    local function zip(files)
        local body,directory,offset={},{},0
        for _,file in ipairs(files) do
            local crc,size,name=crc32(file.bytes),#file.bytes,file.name
            local entry=string.pack('<I4I2I2I2I2I2I4I4I4I2I2',0x04034b50,20,0,0,0,0x21,crc,size,size,#name,0)..name
            body[#body+1]=entry;body[#body+1]=file.bytes
            directory[#directory+1]=string.pack('<I4I2I2I2I2I2I2I4I4I4I2I2I2I2I2I4I4',0x02014b50,20,20,0,0,0,0x21,crc,size,size,#name,0,0,0,0,0,offset)..name
            offset=offset+#entry+size
        end
        local listing=table.concat(directory)
        return table.concat(body)..listing..string.pack('<I4I2I2I2I2I4I4I2',0x06054b50,0,0,#files,#files,#listing,offset,0)
    end
    local function add(files,name,path)
        local bytes=R.native.file_read(path)
        if bytes then files[#files+1]={name=name,bytes=bytes} end
    end
    local function collect(mods)
        local files={}
        add(files,'UE4SS.log',mods..'../UE4SS.log')
        add(files,'UE4SS-shutdown.log',mods..'../UE4SS-shutdown.log')
        for _,id in ipairs(R.native.file_list(R.saved_root)) do
            add(files,'settings/'..id..'.json',R.saved_root..id..'/settings.json')
        end
        local crashes=R.saved_root..'../Crashes/'
        local folders=R.native.file_list(crashes)
        for i=#folders,math.max(1,#folders-2),-1 do
            add(files,'crashes/'..folders[i]..'.xml',crashes..folders[i]..'/CrashContext.runtime-xml')
        end
        if folders[#folders] then add(files,'crashes/latest.dmp',crashes..folders[#folders]..'/UEMinidump.dmp') end
        return files
    end
    -- Saves the archive in the player's Downloads folder and returns its path.
    return {zip=zip,crc32=crc32,save=function(mods)
        local home=(os.getenv('USERPROFILE') or os.getenv('HOME')):gsub('\\','/')
        local path=home..'/Downloads/WitchItPlus-report-'..os.date('%Y%m%d-%H%M%S')..'.zip'
        local ok,why=R.native.file_write(path,zip(collect(mods)))
        assert(ok,why)
        return path
    end}
end
