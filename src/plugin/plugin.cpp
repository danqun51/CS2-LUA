#include "plugin.hpp"

#include <windows.h>
#include <chrono>
#include <iostream>
#include <thread>

extern HMODULE g_module;

PluginRuntime::PluginRuntime() = default;
PluginRuntime::~PluginRuntime() = default;

void PluginRuntime::setup_console() {
  AllocConsole();
  FILE* fp = nullptr;
  freopen_s(&fp, "CONOUT$", "w", stdout);
  freopen_s(&fp, "CONOUT$", "w", stderr);
  std::cout << "[CS2LuaPlugin] console attached\n";
}

void PluginRuntime::teardown_console() {
  std::cout << "[CS2LuaPlugin] unloading\n";
  FreeConsole();
}

std::filesystem::path PluginRuntime::module_dir() const {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(g_module, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

void PluginRuntime::request_stop() { stop_requested_.store(true); }

void PluginRuntime::run() {
  wchar_t exe_path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  const auto scripts_dir = std::filesystem::path(exe_path).parent_path() / L"lua";

  lua_.initialize();
  lua_.load_library_file(module_dir() / L"scripts" / L"nl_compat.lua");
  lua_.load_table_library(module_dir() / L"scripts" / L"offsets.lua", "offsets");
  lua_.load_table_library(module_dir() / L"scripts" / L"entity_offsets.lua", "entity_offsets");
  lua_.load_library_file(module_dir() / L"scripts" / L"entity_compat.lua");
  lua_.set_scripts_dir(scripts_dir);
  lua_.load_all();
  menu_.initialize(lua_);
  game_events_.initialize(lua_);

  auto last = std::chrono::steady_clock::now();
  while (!stop_requested_.load()) {
    game_events_.poll();
    if (GetAsyncKeyState(VK_HOME) & 1) {
      menu_.toggle();
    }
    if (GetAsyncKeyState(VK_F5) & 1) {
      std::cout << "[CS2LuaPlugin] Load scripts requested\n";
      lua_.load_all();
      menu_.print_script_list(lua_);
    }
    if (GetAsyncKeyState(VK_F6) & 1) {
      std::cout << "[CS2LuaPlugin] Unload scripts requested\n";
      lua_.unload_all();
      menu_.print_script_list(lua_);
    }
    if (GetAsyncKeyState(VK_F7) & 1) {
      std::cout << "[CS2LuaPlugin] Reload scripts requested\n";
      lua_.reload_all();
      menu_.print_script_list(lua_);
    }
    if (GetAsyncKeyState(VK_F8) & 1) {
      menu_.print_script_list(lua_);
    }
    if (GetAsyncKeyState(VK_END) & 1) {
      request_stop();
    }

    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    lua_.tick(dt);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  game_events_.shutdown();
  menu_.shutdown();
  lua_.unload_all();
}
