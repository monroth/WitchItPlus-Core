#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
namespace witchit::save {
inline unsigned marker_kind(const std::vector<std::uint8_t>& b) {
    auto signature=[&](std::size_t at,char fourth,unsigned version,unsigned kind){return at+6<=b.size() && b[at]=='F' && b[at+1]=='A' && b[at+2]=='P' && b[at+3]==fourth && b[at+4]==version && b[at+5]==kind;};
    if(b.size()>=6 && (signature(b.size()-6,'R',1,2) || signature(b.size()-6,'M',1,2)))return 2;
    bool extended=b.size()>=8 && b[b.size()-4]=='F' && b[b.size()-3]=='A' && b[b.size()-2]=='P' && b.back()=='5';
    auto u=[&](std::size_t at,unsigned n){if(at+n>b.size())throw std::runtime_error("truncated marker");std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[at+i])<<(i*8);return v;};
    std::size_t length=39;
    if(extended){length=u(b.size()-8,4);if(length<49 || length>33000 || length>b.size())throw std::runtime_error("invalid extended marker size");}
    if(b.size()<length)return 0;
    const auto at=b.size()-length;
    if(!signature(at,'M',extended?5:4,1))return 0;
    if(b[at+6]>2)throw std::runtime_error("invalid marker colour source");
    for(unsigned i=0;i<4;++i){const auto f=std::bit_cast<float>(std::uint32_t(u(at+7+4*i,4)));if(!std::isfinite(f) || f<0 || f>1 || (i==3 && f!=1))throw std::runtime_error("invalid marker colour");}
    if(!u(at+23,4) || u(at+27,8)>9007199254740991ULL || u(at+35,2)>2000 || u(at+37,2)>2000)throw std::runtime_error("invalid marker target/extent");
    if(extended){
        const auto count=u(at+39,2);if(count>64)throw std::runtime_error("too many exclusions");std::size_t pos=at+41;std::set<std::string> paths;
        for(unsigned i=0;i<count;++i){const auto n=u(pos,2);pos+=2;if(!n || n>512 || pos+n>b.size()-8)throw std::runtime_error("invalid exclusion size");
            std::string s(b.begin()+pos,b.begin()+pos+n);pos+=n;
            if(!s.starts_with("/Game/") || s.find('.')==std::string::npos || !paths.insert(s).second)throw std::runtime_error("invalid/duplicate exclusion");
            for(unsigned char c:s)if(c<=32 || c==127)throw std::runtime_error("invalid exclusion path byte");
        }
        if(pos!=b.size()-8)throw std::runtime_error("trailing exclusion bytes");
    }
    return 1;
}
}
