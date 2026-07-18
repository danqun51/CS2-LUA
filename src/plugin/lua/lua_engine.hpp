#pragma once
#include <filesystem>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <thread>
#include <functional>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#ifndef LUA_OK
#define LUA_OK 0
#endif

class LuaEngine {
 public:
  struct UiItem {
    int id{}; std::filesystem::path script; std::string group, type, name, text;
    bool bool_value{}, initial_bool{}, visible{true}, disabled{};
    double number_value{}, initial_number{}, min{}, max{}, scale{1.0};
    int index_value{}, initial_index{}; std::vector<std::string> options;
    std::vector<bool> selected;
    // Hotkey uses Win32 virtual-key codes, matching Neverlose's default_key.
    // mode: 1 = hold, 2 = toggle.
    int hotkey_mode{1};
    bool hotkey_active{}, hotkey_previous_down{};
    bool hotkey_capturing{}, hotkey_capture_wait_release{};
    int callback_ref{LUA_NOREF};
  };
  LuaEngine();
  ~LuaEngine();

  void initialize();
  void set_scripts_dir(const std::filesystem::path& dir);
  bool load_embedded_library(const char* name);
  bool load_embedded_table_library(const char* name, const char* global_name);
  bool load_library_file(const std::filesystem::path& path);
  bool load_table_library(const std::filesystem::path& path, const char* global_name);
  bool load_script(const std::filesystem::path& path);
  bool unload_script(const std::filesystem::path& path);
  bool reload_script(const std::filesystem::path& path);
  void load_directory(const std::filesystem::path& dir);
  void load_all();
  void load_autoload_scripts();
  void unload_all();
  void reload_all();
  void tick(float dt);
  void fire_event(const std::string& name);
  // Non-blocking variant for the render thread: skips the frame if the Lua mutex
  // is held elsewhere.
  bool try_fire_event(const std::string& name);
  void fire_player_hurt(int userid, int attacker, int health, int armor,
                        int damage_health, int damage_armor, int hitgroup,
                        const char* weapon);
  void fire_player_chat(int userid, bool team_only, const char* text, const char* username);
  [[nodiscard]] std::vector<std::filesystem::path> loaded_scripts() const;
  [[nodiscard]] std::vector<std::filesystem::path> available_scripts() const;
  [[nodiscard]] bool has_loaded_scripts() const;
  [[nodiscard]] bool script_autoload_enabled(const std::filesystem::path& path) const;
  void set_script_autoload(const std::filesystem::path& path, bool enabled);
  [[nodiscard]] bool script_has_ui(const std::filesystem::path& path) const;
  void render_script_ui(const std::filesystem::path& path);

 private:
  bool execute_embedded_library(const std::string& name, const std::string& global_name);
  void reset_state();
  void configure_package_path(const std::filesystem::path& dir);
  void load_autoload_settings();
  void save_autoload_settings() const;
  void register_events_api();
  void register_utils_native_api();
  void register_files_native_api();
  void register_network_native_api();
  void register_ui_native_api();
  void join_network_workers();
  void report_error(const char* context, const std::filesystem::path* path = nullptr);
  void set_active_script(const std::filesystem::path& path);
  std::filesystem::path active_script_owner() const;
  bool call_script_noarg(const std::filesystem::path& path, const char* fn);
  bool call_script_number(const std::filesystem::path& path, const char* fn, double value);
  void fire_event_for_script(const std::string& name, const std::filesystem::path& path);
  void remove_script_resources(const std::filesystem::path& path);
  void flush_script_v8(const std::filesystem::path& path);
  void cancel_script_timers(const std::filesystem::path& path);
  void set_event_callback(const std::string& name, int function_index);
  void unset_event_callback(const std::string& name);

  static int lua_event_set(lua_State* L);
  static int lua_event_unset(lua_State* L);
  static int lua_event_call(lua_State* L);
  static int lua_unicode_loadfile(lua_State* L);
  static int lua_unicode_dofile(lua_State* L);
  static int lua_unicode_module_searcher(lua_State* L);
  static int lua_script_require(lua_State* L);
  static int lua_utils_create_interface(lua_State* L);
  static int lua_utils_opcode_scan(lua_State* L);
  static int lua_utils_is_readable(lua_State* L);
  static int lua_utils_console_exec(lua_State* L);
  static int lua_files_read(lua_State* L);
  static int lua_files_write(lua_State* L);
  static int lua_files_append(lua_State* L);
  static int lua_files_exists(lua_State* L);
  static int lua_files_list(lua_State* L);
  static int lua_files_create_directory(lua_State* L);
  static int lua_files_remove(lua_State* L);
  static int lua_files_rename(lua_State* L);
  static int lua_files_size(lua_State* L);
  static int lua_network_get(lua_State* L);
  static int lua_network_post(lua_State* L);
  static int lua_ui_create_item(lua_State* L);
  static int lua_ui_get(lua_State* L);
  static int lua_ui_set(lua_State* L);
  static int lua_ui_set_callback(lua_State* L);
  static int lua_ui_set_state(lua_State* L);
  static int lua_ui_hotkey_info(lua_State* L);

  lua_State* L_{nullptr};
  std::filesystem::path scripts_dir_;
  struct ScriptState { std::filesystem::path path; int environment_ref{LUA_NOREF}; };
  struct EventCallback { std::filesystem::path script; int function_ref{LUA_NOREF}; };
  std::vector<std::filesystem::path> loaded_scripts_;
  std::vector<ScriptState> script_states_;
  std::vector<std::filesystem::path> autoload_scripts_;
  std::vector<std::filesystem::path> library_files_;
  std::vector<std::pair<std::filesystem::path, std::string>> table_library_files_;
  std::vector<std::pair<std::string, std::string>> embedded_libraries_;
  mutable std::recursive_mutex mutex_;
  std::unordered_map<std::string, std::vector<EventCallback>> event_callbacks_;
  std::vector<std::string> pending_console_commands_;
  struct NetworkResult { int callback_ref; std::filesystem::path script; std::string body; unsigned long status; bool ok; };
  std::mutex network_mutex_;
  std::vector<NetworkResult> network_results_;
  std::vector<std::thread> network_workers_;
  std::vector<UiItem> ui_items_;
  int next_ui_id_{1};
  std::filesystem::path current_script_;

};
