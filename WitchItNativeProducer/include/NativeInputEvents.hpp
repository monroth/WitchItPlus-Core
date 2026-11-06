#pragma once
#include <UE4SSProgram.hpp>
#include <deque>
#include <memory>
#include <mutex>
#include <functional>
#include <tuple>
namespace witchit {
// The SDK input source produces key-down edges. Its OS thread queues scalars;
// the existing owned host dispatcher enters Lua once on the game thread.
class NativeInputEvents {
 struct State {
  std::mutex mutex;
  std::deque<std::tuple<unsigned,unsigned,std::int64_t>> keys;
  std::function<void()> signal;
  std::function<void(unsigned,unsigned,std::int64_t)> deliver;
  std::int64_t sequence{};
  bool active{true},queued{};
 };
 std::shared_ptr<State> state_;
 static constexpr std::uint8_t marker=0xa6;
public:
 void start(std::function<void(unsigned,unsigned,std::int64_t)> deliver) {
  if(state_)return;
  auto state=std::make_shared<State>();state->deliver=std::move(deliver);
  std::weak_ptr<State> weak=state;
  auto& program=RC::UE4SSProgram::get_program();
  state->signal=program.make_game_thread_event([weak]{
   auto s=weak.lock();if(!s)return;
   std::deque<std::tuple<unsigned,unsigned,std::int64_t>> keys;
   {std::lock_guard lock(s->mutex);s->queued=false;if(!s->active)return;keys.swap(s->keys);}
   for(auto [key,mask,sequence]:keys){
    {std::lock_guard lock(s->mutex);if(!s->active)return;}
    s->deliver(key,mask,sequence);
   }
  },false);
  if(!state->signal)throw std::runtime_error("input game-thread dispatch unavailable");
  state_=state;
  // Each exact chord uses the host's Windows input-event delivery. No
  // IsInputKeyDown queries, timer, or key scan is installed on the game thread.
  for(unsigned key=1;key<255;++key)for(unsigned mask=0;mask<8;++mask){
   RC::Input::Handler::ModifierKeyArray mods{};unsigned index=0;
   if(mask&1)mods[index++]=RC::Input::ModifierKey::SHIFT;
   if(mask&2)mods[index++]=RC::Input::ModifierKey::CONTROL;
   if(mask&4)mods[index++]=RC::Input::ModifierKey::ALT;
   program.register_keydown_event(static_cast<RC::Input::Key>(key),mods,[weak,key,mask]{
    auto s=weak.lock();if(!s)return;bool signal=false;
    {std::lock_guard lock(s->mutex);if(!s->active)return;
     // Bounded scalar queue. Input edges may coalesce while the game is loading;
     // retain the latest presses, never invoke a Lua closure on this thread.
     if(s->keys.size()==256)s->keys.pop_front();
     s->keys.emplace_back(key,mask,++s->sequence);signal=!s->queued;s->queued=true;}
    if(signal)s->signal();
   },marker,state.get());
  }
 }
 std::int64_t mark()const {auto state=state_;if(!state)return 0;std::lock_guard lock(state->mutex);return state->sequence;}
 void stop() {
  auto state=std::move(state_);if(!state)return;
  {std::lock_guard lock(state->mutex);state->active=false;state->keys.clear();}
  RC::UE4SSProgram::get_program().get_all_input_events([token=state.get()](auto& keys){
   for(auto& [key,entries]:keys.key_data)
    std::erase_if(entries,[token](const auto& entry){return entry.custom_data==marker && entry.custom_data2==token;});
  });
 }
};
}
