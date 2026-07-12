#pragma once
#include <mutex>
#include <string>
#include <vector>

class LuaEngine;
class IGameEvent;
class IGameEventManager2;

class GameEventBridge {
 public:
  bool initialize(LuaEngine& lua);
  void poll();
  void shutdown();
  void on_game_event(IGameEvent* event);
  void on_console_line(const char* line);
  void attach_legacy_manager(IGameEventManager2* manager);

 private:
  class Listener;
  LuaEngine* lua_{};
  IGameEventManager2* manager_{};
  Listener* listener_{};
  void dispatch_console_line(const std::string& line);
  std::mutex chat_mutex_;
  std::vector<std::string> pending_chat_lines_;
};
