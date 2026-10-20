#pragma once
#include <stdexcept>
namespace witchit::actors {
struct Placement {double x{},y{},z{},yaw{},pitch{},roll{},sx{1},sy{1},sz{1};};
template<class Engine,class Initialize,class Publish,class RetainFailure>
void spawn(Engine& engine,const typename Engine::Context& context,Placement position,
           Initialize initialize,Publish publish,RetainFailure retain_failure) {
 typename Engine::Object object{};bool acquired=false;
 try {
  engine.spawn_owned(context,position,[&](auto value){if(acquired)throw std::runtime_error("Native.spawn returned twice");object=value;acquired=true;});
  if(!acquired || !engine.current(context) || !engine.alive(context,object))throw std::runtime_error("Native.spawn context changed");
  initialize(object);
  if(!engine.current(context) || !engine.alive(context,object))throw std::runtime_error("Native.spawn initializer retired actor");
  engine.finish_owned(context,object,position);
  publish(object);
 } catch(...) {
  if(acquired && engine.world_current(context)) {
   try {if(engine.alive(context,object) && !engine.destroy_owned(context,object))retain_failure(context,object);}
   catch(...) {retain_failure(context,object);}
  }
  throw;
 }
}
}
