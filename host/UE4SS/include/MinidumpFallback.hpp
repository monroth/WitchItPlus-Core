#pragma once
namespace RC {
// Retry without optional DbgHelp flags. Reset the partial file before retrying;
// preserve each error before any CloseHandle or diagnostic call can replace it.
template<class Write,class Reset,class Error>
bool WriteMinidumpWithFallback(Write write,Reset reset,Error error,unsigned long& first,unsigned long& last) {
    if(write(false))return true;
    first=error();
    if(!reset()){last=error();return false;}
    if(write(true))return true;
    last=error();return false;
}
}
