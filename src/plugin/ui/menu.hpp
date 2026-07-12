#pragma once
#include <atomic>
#include <filesystem>
#include <vector>

class LuaEngine;

class MenuController {
 public:
  bool initialize(LuaEngine& lua);
  void shutdown();
  void toggle();
  bool visible() const;

  void print_script_list(LuaEngine& lua) const;

 private:
  std::atomic_bool visible_{false};
  std::atomic_bool initialized_{false};
};
