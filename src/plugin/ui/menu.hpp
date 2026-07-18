#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <vector>

class LuaEngine;

enum class ScriptActionType {
  LoadAll, UnloadAll, ReloadAll, LoadScript, UnloadScript, ReloadScript
};

struct ScriptAction {
  ScriptActionType type;
  std::filesystem::path path;
};

class MenuController {
 public:
  bool initialize(LuaEngine& lua);
  void shutdown();
  void toggle();
  bool visible() const;

  void print_script_list(LuaEngine& lua) const;
  void request_script_action(ScriptActionType type,
                             std::filesystem::path path = {});
  std::vector<ScriptAction> consume_script_actions();

 private:
  std::atomic_bool visible_{false};
  std::atomic_bool initialized_{false};
  std::mutex action_mutex_;
  std::vector<ScriptAction> pending_actions_;
};
