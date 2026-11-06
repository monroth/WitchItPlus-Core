#pragma once
#include "SaveRow.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
namespace witchit::save {
inline unsigned marker_kind(const std::vector<std::uint8_t>& b) {
    auto signature=[&](std::size_t at,char fourth,unsigned version,unsigned kind){return at+6<=b.size() && b[at]=='F' && b[at+1]=='A' && b[at+2]=='P' && b[at+3]==fourth && b[at+4]==version && b[at+5]==kind;};
    if(b.size()>=6 && (signature(b.size()-6,'R',1,2) || signature(b.size()-6,'M',1,2)))return 2;
    bool extended=b.size()>=8 && b[b.size()-4]=='F' && b[b.size()-3]=='A' && b[b.size()-2]=='P' && (b.back()=='5' || b.back()=='7' || b.back()=='8');
    auto u=[&](std::size_t at,unsigned n){if(at+n>b.size())throw std::runtime_error("truncated marker");std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[at+i])<<(i*8);return v;};
    for(unsigned version:{3u,2u,1u})if(b.size()>=(version==3?29:22+version)) {
        const auto at=b.size()-(version==3?29:22+version);
        if(signature(at,'T',version,3) || signature(at,'T',version,4)) {
            if(b[at+6]>1)throw std::runtime_error("invalid teleporter colour source");
            for(unsigned i=0;i<4;++i){const auto f=std::bit_cast<float>(std::uint32_t(u(at+7+4*i,4)));if(!std::isfinite(f) || f<0 || f>1 || (i==3 && f!=1))throw std::runtime_error("invalid teleporter colour");}
            if(version>=2 && b[at+23]>4)throw std::runtime_error("invalid teleporter look");
            if(version==3 && (!u(at+24,4) || b[at+28]>1))throw std::runtime_error("invalid teleporter identity");
            return b[at+5];
        }
    }
    std::size_t length=39;
    if(extended){length=u(b.size()-8,4);if(length<49 || length>33000 || length>b.size())throw std::runtime_error("invalid extended marker size");}
    if(b.size()<length)return 0;
    const auto at=b.size()-length;
    if(!signature(at,'M',extended?unsigned(b.back()-'0'):4,1))return 0;
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
        if(b.back()=='7') {
            if(pos+12!=b.size()-8)throw std::runtime_error("invalid seek coefficient size");
            const auto coefficient=std::bit_cast<float>(std::uint32_t(u(pos,4)));
            if(!std::isfinite(coefficient) || std::abs(coefficient)>600 || u(pos+4,8)>9007199254740991ULL)throw std::runtime_error("invalid seek coefficient");
            pos+=12;
        }
        if(b.back()=='8') {
            if(pos+2!=b.size()-8 || u(pos,2)>10000)throw std::runtime_error("invalid prop exclusion radius");
            pos+=2;
        }
        if(pos!=b.size()-8)throw std::runtime_error("trailing exclusion bytes");
    }
    return 1;
}
inline bool copy_carrier_payload(witchit::SaveRow& row,const std::vector<std::uint8_t>& payload) {
    const auto kind=marker_kind(payload);if(!kind)return false;
    row.materials=payload;
    // For v1 the version is 19 bytes from the end; for v2 this slot is
    // the endpoint kind (3/4), so an already-versioned look remains untouched.
    if(kind>=3 && payload.size()>=23 && payload[payload.size()-23]=='F' && payload[payload.size()-22]=='A' && payload[payload.size()-21]=='P' && payload[payload.size()-20]=='T' && payload[payload.size()-19]==1) {
        row.materials[payload.size()-19]=2;row.materials.push_back(0); // v1 -> None
    }
    return true;
}
// Called only for the current native save projection, after all carrier bytes
// have been copied. Legacy RGB ordering matches Lua's unsigned byte ordering.
inline void upgrade_endpoint_rows(std::vector<witchit::SaveRow>& rows) {
    std::set<std::uint32_t> used;std::map<std::string,std::uint32_t> legacy;
    auto offset=[](const auto& b){return b.size()-(b.size()>=29 && b[b.size()-29]=='F' && b[b.size()-28]=='A' && b[b.size()-27]=='P' && b[b.size()-26]=='T' && b[b.size()-25]==3?29:24);};
    for(const auto& row:rows)if(marker_kind(row.materials)>=3) {
        const auto& b=row.materials;const auto at=offset(b);
        if(b[at+4]==3){std::uint32_t id{};for(unsigned i=0;i<4;++i)id|=std::uint32_t(b[at+24+i])<<(8*i);used.insert(id);}
        else legacy.emplace(std::string(b.begin()+at+7,b.begin()+at+19),0);
    }
    std::uint32_t next=1;
    // std::string comparison uses unsigned char semantics, as Lua byte strings do.
    for(auto& [key,id]:legacy){while(used.contains(next))++next;id=next;used.insert(next);}
    for(auto& row:rows)if(marker_kind(row.materials)>=3){auto& b=row.materials;const auto at=offset(b);if(b[at+4]==3)continue;
        const auto id=legacy.at(std::string(b.begin()+at+7,b.begin()+at+19));b[at+4]=3;
        for(unsigned i=0;i<4;++i){b.push_back(std::uint8_t(id>>(8*i)));}
        b.push_back(0);
    }
}
// The native loader consumes material indices as slots; endpoint metadata must
// be restored afterwards even when no Lua LoadStatics observer was invoked.
inline bool restore_endpoint_payload(std::vector<std::uint8_t>& current,const std::vector<std::uint8_t>& saved) {
    if(marker_kind(saved)<3 || current==saved)return false;
    current=saved;return true;
}

}
