#pragma once
#include <atomic>

#include "lua/lua_engine.hpp"
#include "ui/menu.hpp"
#include "events/game_events.hpp"

class PluginRuntime {
 public:
  PluginRuntime();
  ~PluginRuntime();

  void run();
  void request_stop();
  LuaEngine& lua_engine() { return lua_; }
  MenuController& menu_controller() { return menu_; }

 private:
  void setup_console();
  void teardown_console();

  std::atomic_bool stop_requested_{false};
  LuaEngine lua_;
  MenuController menu_;
  GameEventBridge game_events_;
};
