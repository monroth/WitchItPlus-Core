return function(R)
    local root=R.saved_root
    local F={}
    function F.saved_root(pkg) return root..pkg end
    function F.read_json(rel)
        local bytes=R.native.file_read(root..rel)
        if bytes then return R.json.decode(bytes,4194304) end
    end
    function F.write_json(rel,value) return R.native.file_write(root..rel,R.json.encode(value,4194304)) end
    function F.remove(rel) return R.native.file_remove(root..rel) end
    function F.list(rel) return R.native.file_list(root..rel) end
    return F
end
