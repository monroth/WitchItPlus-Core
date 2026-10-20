#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace witchit {
struct Instruction {std::uint32_t at,end;bool removed;};
struct Branch {std::uint32_t old_at,new_at,old_target,new_target;};
struct Operand {std::uint32_t at,width;std::string kind,owner,name,path;};
struct RulesPlan {
    std::vector<std::uint8_t> bytes,mask;
    std::vector<Instruction> instructions;
    std::vector<Branch> branches;
    std::vector<Operand> operands;
};
inline std::uint32_t read_u32(std::span<const std::uint8_t> b,std::size_t at) {
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
// Pure construction: retain the actual relocated operands from this loaded
// function, and rewrite only the 14 verified absolute control-flow operands.
// Resolver must compare every dynamic operand against exact live metadata.
template<class Resolve>
bool prepare_rules(const RulesPlan& p,std::span<const std::uint8_t> original,
                   Resolve&& resolve,std::vector<std::uint8_t>& candidate) {
    candidate.clear();
    if(original.size()!=2066 || p.bytes.size()!=2066 || p.mask.size()!=2066 ||
       p.instructions.size()!=81 || p.branches.size()!=14)return false;
    for(std::size_t i=0;i<original.size();++i)
        if(p.mask[i] && original[i]!=p.bytes[i])return false;
    for(const auto& op:p.operands) {
        if(op.at>original.size() || op.width>original.size()-op.at ||
           !resolve(op,original.subspan(op.at,op.width)))return false;
    }
    std::uint32_t cursor=0;
    for(const auto& ins:p.instructions) {
        if(ins.at!=cursor || ins.end<=ins.at || ins.end>original.size())return false;
        cursor=ins.end;
        if(!ins.removed)candidate.insert(candidate.end(),original.begin()+ins.at,original.begin()+ins.end);
    }
    if(cursor!=2066 || candidate.size()!=1887){candidate.clear();return false;}
    for(const auto& b:p.branches) {
        if(b.old_at+5>original.size() || b.new_at+5>candidate.size() || b.new_target>=candidate.size() ||
           read_u32(original,b.old_at+1)!=b.old_target){candidate.clear();return false;}
        for(int i=0;i<4;++i)candidate[b.new_at+1+i]=std::uint8_t(b.new_target>>(i*8));
    }
    return true;
}
}
