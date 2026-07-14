#pragma once
#include <mutex>
#include <cstdint>
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
  IGameEventManager2* client_manager_{};
  IGameEventManager2* server_manager_{};
  Listener* client_listener_{};
  Listener* server_listener_{};
  void attach_client_manager(IGameEventManager2* manager);
  void attach_server_manager(IGameEventManager2* manager);
  void dispatch_console_line(const std::string& line);
  std::mutex chat_mutex_;
  std::vector<std::string> pending_chat_lines_;
  std::mutex hurt_mutex_;
  uint64_t last_hurt_time_{};
  int last_hurt_userid_{};
  int last_hurt_attacker_{};
  int last_hurt_health_{};
  int last_hurt_damage_{};
  int last_hurt_hitgroup_{};
};
