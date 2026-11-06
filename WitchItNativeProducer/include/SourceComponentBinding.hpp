#pragma once
namespace witchit {
// Used by the real native leaf restore before and after FinishSpawning.
template<class Object,class Alias,class Named,class Root,class Matches>
Object* source_component_binding(Alias alias,Named named,Root root,bool is_root,Matches matches) {
 if(auto* value=alias())return value;
 if(auto* value=named())return value;
 if(is_root)if(auto* value=root();value && matches(value))return value;
 return nullptr;
}
}
