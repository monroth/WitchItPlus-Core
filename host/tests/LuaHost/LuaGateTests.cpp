#include <lua.hpp>
#include <iostream>
#include <thread>
#include <vector>
extern "C" void LuaLockInitial(lua_State*);
extern "C" void LuaLock(lua_State*);
extern "C" void LuaUnlock(lua_State*);
extern "C" int gate_initializations();
extern "C" int gate_deletions();
extern "C" int gate_depth();
int main(){
    std::vector<std::thread> initializers;
    for(int i=0;i<16;++i)initializers.emplace_back([]{LuaLockInitial(nullptr);});
    for(auto& thread:initializers)thread.join();
    if(gate_initializations()!=1)return 9;
    auto* a=luaL_newstate();auto* b=luaL_newstate();
    if(!a||!b)return 2;
    lua_close(b);
    if(gate_deletions()!=0||gate_depth()!=0){
        std::cerr<<"FAIL one VM close deleted gate or left it acquired\n";return 1;
    }
    lua_pushinteger(a,7);
    if(lua_tointeger(a,-1)!=7||gate_depth()!=0)return 3;
    std::thread worker([&]{lua_pushinteger(a,9);});worker.join();
    if(lua_tointeger(a,-1)!=9)return 4;
    LuaLock(a);LuaLock(a);
    if(gate_depth()!=2)return 5;
    LuaUnlock(a);LuaUnlock(a);
    lua_close(a);
    if(gate_depth()!=0||gate_deletions()!=0||gate_initializations()!=1)return 6;
    auto* c=luaL_newstate();
    if(!c||gate_initializations()!=1)return 7;
    lua_close(c);
    if(gate_depth()!=0)return 8;
    std::cout<<"PASS bundled Lua: close one VM, other VM usable, cross-thread entry, recursive gate, balanced close, reopen\n";
}
