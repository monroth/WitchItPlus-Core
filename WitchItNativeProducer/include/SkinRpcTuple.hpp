#pragma once
#include <string>
#include <vector>
#include <stdexcept>
namespace witchit::skins {
// Bounded scalar RPC input; never a class path or a Lua/native struct conversion.
inline std::vector<std::string> parse(const std::string& text,std::size_t count) {
 if((count!=5 && count!=7) || text.empty() || text.size()>4096)throw std::runtime_error("skin tuple size differs");
 std::vector<std::string> ids;std::size_t start{};
 while(start<text.size()) {
  const auto end=text.find('|',start);const auto part=text.substr(start,end==std::string::npos?end:end-start);
  if(!part.starts_with("Skin:") || part.size()<=5 || part.size()>261)throw std::runtime_error("skin identifier differs");
  auto name=part.substr(5);
  for(unsigned char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))throw std::runtime_error("skin name differs");
  if(name=="None")throw std::runtime_error("empty skin name");
  ids.push_back(name);
  if(end==std::string::npos)break;
  start=end+1;if(start==text.size())throw std::runtime_error("trailing skin separator");
 }
 if(ids.size()!=count)throw std::runtime_error("skin slot count differs");
 return ids;
}
}
