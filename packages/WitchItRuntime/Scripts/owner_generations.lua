-- Runtime-local monotonic identities. No native call, timer, RNG or global nonce claim.
local M={}
function M.new(R)
    local serial=0
    local B={}
    function B.next()
        if not R.is_healthy() or serial>=2147483647 then return nil end
        serial=serial+1
        return serial
    end
    function B.attach(p,A)
        function A.allocate_owner_generation(namespace)
            if not R.is_healthy() or not p.active or p.api~=A then return nil end
            assert(type(namespace)=='string' and #namespace>0 and #namespace<=128
                and not namespace:find('[%c]'),'invalid owner namespace')
            return B.next()
        end
    end
    return B
end
return M
