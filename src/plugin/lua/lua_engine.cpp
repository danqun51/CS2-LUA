#include "lua_engine.hpp"
#include "embedded_libraries.hpp"
#include <iostream>
#include <string>
#include <windows.h>
#include <algorithm>
#include <sstream>
#include <cstdlib>
#include <winhttp.h>
#include <imgui.h>

namespace {
using EngineMsgFn = void(__cdecl*)(const char*, ...);
const char* CompatToString(lua_State* L, int index, size_t* length) {
  lua_getglobal(L, "tostring");
  lua_pushvalue(L, index < 0 ? index - 1 : index);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    if (length) *length = 0;
    return nullptr;
  }
  return lua_tolstring(L, -1, length);
}
void PushTraceback(lua_State* L, const char* message, int level) {
  lua_getglobal(L, "debug");
  if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushstring(L, message); return; }
  lua_getfield(L, -1, "traceback");
  lua_remove(L, -2);
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_pushstring(L, message); return; }
  lua_pushstring(L, message);
  lua_pushinteger(L, level);
  if (lua_pcall(L, 2, 1, 0) != LUA_OK) { lua_pop(L, 1); lua_pushstring(L, message); }
}
void ConsoleLine(const std::string& line) {
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  auto msg = tier0 ? reinterpret_cast<EngineMsgFn>(GetProcAddress(tier0, "Msg")) : nullptr;
  if (!msg && tier0) msg = reinterpret_cast<EngineMsgFn>(GetProcAddress(tier0, "ConMsg"));
  if (msg) msg("%s\n", line.c_str());
  OutputDebugStringA((line + "\n").c_str());
}

void PublishTableLibrary(lua_State* L, const char* name) {
  const int table_index = lua_gettop(L);
  lua_pushvalue(L, table_index);
  lua_setglobal(L, name);

  // Support both the legacy global API and normal require("name") calls.
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "loaded");
    if (lua_istable(L, -1)) {
      lua_pushvalue(L, table_index);
      lua_setfield(L, -2, name);
    }
  }
  lua_settop(L, table_index - 1);
}
int LuaConsolePrint(lua_State* L) {
  std::string line;
  const int count = lua_gettop(L);
  for (int i = 1; i <= count; ++i) {
    size_t len = 0;
    const char* value = CompatToString(L, i, &len);
    if (i > 1) line.push_back('\t');
    if (value) line.append(value, len);
    lua_pop(L, 1);
  }
  ConsoleLine(line);
  return 0;
}
}

LuaEngine::LuaEngine() = default;
LuaEngine::~LuaEngine() {
  join_network_workers();
  unload_all();
  if (L_) lua_close(L_);
}

void LuaEngine::initialize() {
  std::lock_guard lock(mutex_);
  if (L_) return;
  L_ = luaL_newstate();
  luaL_openlibs(L_);
  // Neverlose-compatible current features: expose LuaJIT FFI and BitOp globally.
  if (luaL_dostring(L_, "ffi=require('ffi'); bit=require('bit')") != LUA_OK) {
    report_error("LuaJIT libraries");
  }
  lua_pushcfunction(L_, LuaConsolePrint);
  lua_setglobal(L_, "print");
  register_events_api();
  register_utils_native_api();
  register_files_native_api();
  register_network_native_api();
  register_ui_native_api();
  std::cout << "[LuaEngine] initialized Lua " << LUA_VERSION << "\n";
}

void LuaEngine::set_scripts_dir(const std::filesystem::path& dir) {
  std::lock_guard lock(mutex_);
  scripts_dir_ = dir;
  if (L_) configure_package_path(scripts_dir_);
}

bool LuaEngine::execute_embedded_library(const std::string& name,
                                         const std::string& global_name) {
  const auto source = embedded_libraries::find(name);
  if (!source) {
    ConsoleLine("[Lua Error] embedded library not found: " + name);
    return false;
  }

  const std::string chunk_name = "@embedded/" + name;
  const int result_count = global_name.empty() ? 0 : 1;
  if (luaL_loadbuffer(L_, source->data(), source->size(), chunk_name.c_str()) != LUA_OK ||
      lua_pcall(L_, 0, result_count, 0) != LUA_OK) {
    report_error(name.c_str());
    return false;
  }

  if (!global_name.empty()) {
    if (!lua_istable(L_, -1)) {
      lua_pop(L_, 1);
      ConsoleLine("[Lua Error] embedded library " + name +
                  " did not return a table");
      return false;
    }
    PublishTableLibrary(L_, global_name.c_str());
  }
  return true;
}

bool LuaEngine::load_embedded_library(const char* name) {
  std::lock_guard lock(mutex_);
  if (!L_) initialize();
  const std::string library_name = name ? name : "";
  if (library_name.empty() || !execute_embedded_library(library_name, "")) return false;
  const auto registration = std::make_pair(library_name, std::string{});
  if (std::find(embedded_libraries_.begin(), embedded_libraries_.end(), registration) ==
      embedded_libraries_.end()) {
    embedded_libraries_.push_back(registration);
  }
  return true;
}

bool LuaEngine::load_embedded_table_library(const char* name, const char* global_name) {
  std::lock_guard lock(mutex_);
  if (!L_) initialize();
  const std::string library_name = name ? name : "";
  const std::string global = global_name ? global_name : "";
  if (library_name.empty() || global.empty() ||
      !execute_embedded_library(library_name, global)) return false;
  const auto registration = std::make_pair(library_name, global);
  if (std::find(embedded_libraries_.begin(), embedded_libraries_.end(), registration) ==
      embedded_libraries_.end()) {
    embedded_libraries_.push_back(registration);
  }
  return true;
}

bool LuaEngine::load_library_file(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  if (!L_) initialize();
  const auto utf8 = path.u8string();
  if (luaL_dofile(L_, reinterpret_cast<const char*>(utf8.c_str())) != LUA_OK) {
    report_error("compat library", &path); return false;
  }
  if (std::find(library_files_.begin(), library_files_.end(), path) == library_files_.end()) library_files_.push_back(path);
  return true;
}

bool LuaEngine::load_table_library(const std::filesystem::path& path, const char* global_name) {
  std::lock_guard lock(mutex_);
  if (!L_) initialize();
  const auto utf8 = path.u8string();
  if (luaL_loadfile(L_, reinterpret_cast<const char*>(utf8.c_str())) != LUA_OK || lua_pcall(L_, 0, 1, 0) != LUA_OK) {
    report_error(global_name, &path); return false;
  }
  if (!lua_istable(L_, -1)) { lua_pop(L_, 1); ConsoleLine(std::string("[Lua Error] ") + global_name + " did not return a table"); return false; }
  PublishTableLibrary(L_, global_name);
  const auto key = std::make_pair(path, std::string(global_name));
  if (std::find(table_library_files_.begin(), table_library_files_.end(), key) == table_library_files_.end()) table_library_files_.push_back(key);
  return true;
}

void LuaEngine::report_error(const char* context, const std::filesystem::path* path) {
  const char* raw = lua_tostring(L_, -1);
  const std::string message = raw ? raw : "unknown Lua error";
  PushTraceback(L_, message.c_str(), 1);
  const char* trace = lua_tostring(L_, -1);
  std::string out = "[Lua Error] ";
  out += context ? context : "runtime";
  if (path) out += " [" + path->filename().string() + "]";
  out += ": "; out += trace ? trace : message;
  ConsoleLine(out);
  lua_pop(L_, 2);
}

void LuaEngine::reset_state() {
  join_network_workers();
  if (L_) {
    lua_close(L_);
    L_ = nullptr;
  }
  loaded_scripts_.clear();
  event_callbacks_.clear();
  ui_items_.clear(); next_ui_id_ = 1;
  initialize();
  for (const auto& [name, global] : embedded_libraries_) {
    if (!execute_embedded_library(name, global)) {
      ConsoleLine("[Lua Error] failed to restore embedded library: " + name);
    }
  }
  for (const auto& library : library_files_) {
    const auto utf8 = library.u8string();
    if (luaL_dofile(L_, reinterpret_cast<const char*>(utf8.c_str())) != LUA_OK) lua_pop(L_, 1);
  }
  for (const auto& [library, global] : table_library_files_) {
    const auto utf8 = library.u8string();
    if (luaL_loadfile(L_, reinterpret_cast<const char*>(utf8.c_str())) == LUA_OK && lua_pcall(L_, 0, 1, 0) == LUA_OK && lua_istable(L_, -1))
      PublishTableLibrary(L_, global.c_str());
    else { if (lua_gettop(L_) > 0) lua_pop(L_, 1); }
  }
  if (!scripts_dir_.empty()) configure_package_path(scripts_dir_);
}

void LuaEngine::configure_package_path(const std::filesystem::path& dir) {
  if (!L_) initialize();
  const auto package_dir = (dir / "?.lua").u8string();
  lua_getglobal(L_, "package");
  lua_getfield(L_, -1, "path");
  std::string current_path = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "";
  const std::string script_path = reinterpret_cast<const char*>(package_dir.c_str());
  if (current_path.find(script_path) == std::string::npos) {
    current_path += ";";
    current_path += script_path;
  }
  lua_pop(L_, 1);
  lua_pushstring(L_, current_path.c_str());
  lua_setfield(L_, -2, "path");
  lua_pop(L_, 1);
}

bool LuaEngine::load_script(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  const auto normalized = std::filesystem::absolute(path).lexically_normal();
  if (std::find(loaded_scripts_.begin(), loaded_scripts_.end(), normalized) != loaded_scripts_.end()) return true;
  if (!L_) initialize();
  const auto utf8 = normalized.u8string();
  current_script_ = normalized;
  if (luaL_dofile(L_, reinterpret_cast<const char*>(utf8.c_str())) != LUA_OK) {
    current_script_.clear();
    report_error("load failed", &normalized);
    return false;
  }
  current_script_.clear();
  loaded_scripts_.push_back(normalized);
  std::cout << "[LuaEngine] loaded: " << path.string() << "\n";
  call_noarg("on_load");
  fire_event("load");
  return true;
}

bool LuaEngine::unload_script(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  const auto target = std::filesystem::absolute(path).lexically_normal();
  if (std::find(loaded_scripts_.begin(), loaded_scripts_.end(), target) == loaded_scripts_.end()) return true;
  auto keep = loaded_scripts_;
  keep.erase(std::remove(keep.begin(), keep.end(), target), keep.end());
  call_noarg("on_unload");
  reset_state();
  bool ok = true;
  for (const auto& script : keep) ok = load_script(script) && ok;
  return ok;
}

bool LuaEngine::reload_script(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  const auto target = std::filesystem::absolute(path).lexically_normal();
  unload_script(target);
  return load_script(target);
}

void LuaEngine::load_directory(const std::filesystem::path& dir) {
  std::lock_guard lock(mutex_);
  set_scripts_dir(dir);
  if (!std::filesystem::exists(dir)) {
    std::wcout << L"[LuaEngine] script directory not found, creating: " << dir << L"\n";
    std::filesystem::create_directories(dir);
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != L".lua") continue;
    std::wstring name = entry.path().filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    // Diagnostic scripts are visible in the menu but must be started manually.
    // Auto-running entity/network/FFI tests inside DllMain initialization made
    // injection execute scans and HTTP requests before the game was ready.
    if (name.size() >= 9 && name.ends_with(L"_test.lua")) continue;
    load_script(entry.path());
  }
}

void LuaEngine::load_all() {
  std::lock_guard lock(mutex_);
  if (scripts_dir_.empty()) return;
  if (has_loaded_scripts()) {
    std::cout << "[LuaEngine] scripts already loaded; use reload_all for refresh\n";
    return;
  }
  load_directory(scripts_dir_);
}

void LuaEngine::unload_all() {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  fire_event("shutdown");
  call_noarg("on_unload");
  loaded_scripts_.clear();
  std::cout << "[LuaEngine] unloaded all scripts\n";
}

void LuaEngine::reload_all() {
  std::lock_guard lock(mutex_);
  const auto dir = scripts_dir_;
  unload_all();
  reset_state();
  scripts_dir_ = dir;
  load_directory(scripts_dir_);
  std::cout << "[LuaEngine] reloaded scripts\n";
}

void LuaEngine::tick(float dt) {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  std::vector<NetworkResult> network_results;
  {
    std::lock_guard network_lock(network_mutex_);
    network_results.swap(network_results_);
  }
  for (auto& result : network_results) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, result.callback_ref);
    lua_pushlstring(L_, result.body.data(), result.body.size());
    lua_pushinteger(L_, result.status);
    if (lua_pcall(L_, 2, 0, 0) != LUA_OK) report_error("network callback");
    luaL_unref(L_, LUA_REGISTRYINDEX, result.callback_ref);
  }
  // Chat callbacks can run from inside tier0!Msg/ConMsg. Executing an engine
  // command re-entrantly from that logging call is ignored by InputService.
  // Drain queued commands later from the plugin update loop instead.
  if (!pending_console_commands_.empty()) {
    using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
    using ExecCmdFn = void(__fastcall*)(void*, int, const char*, int);
    HMODULE engine = GetModuleHandleW(L"engine2.dll");
    auto create = engine ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
    void* input = create ? create("InputService_001", nullptr) : nullptr;
    void** vtable = input ? *reinterpret_cast<void***>(input) : nullptr;
    auto exec = vtable && vtable[25] ? reinterpret_cast<ExecCmdFn>(vtable[25]) : nullptr;
    auto commands = std::move(pending_console_commands_);
    pending_console_commands_.clear();
    if (exec) for (const auto& command : commands) exec(input, 5, command.c_str(), 0);
    else ConsoleLine("[Lua Error] utils.console_exec: queued command failed, InputService unavailable");
  }
  fire_event("tick");
  lua_getglobal(L_, "__nl_process_timers");
  if (lua_isfunction(L_, -1)) {
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) report_error("utils.execute_after");
  } else lua_pop(L_, 1);
  lua_getglobal(L_, "on_tick");
  if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return; }
  lua_pushnumber(L_, dt);
  if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
    report_error("on_tick");
  }
}

void LuaEngine::fire_event(const std::string& name) {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  const auto it = event_callbacks_.find(name);
  if (it == event_callbacks_.end()) return;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
  if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
    const std::string context = "events." + name;
    report_error(context.c_str());
  }
}

bool LuaEngine::try_fire_event(const std::string& name) {
  std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return false;  // plugin thread busy; skip this frame
  if (!L_) return true;
  const auto it = event_callbacks_.find(name);
  if (it == event_callbacks_.end()) return true;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
  if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
    const std::string context = "events." + name;
    report_error(context.c_str());
  }
  return true;
}

void LuaEngine::fire_player_hurt(int userid, int attacker, int health, int armor,
                                 int damage_health, int damage_armor, int hitgroup,
                                 const char* weapon) {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  const auto it = event_callbacks_.find("player_hurt");
  if (it == event_callbacks_.end()) return;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
  lua_newtable(L_);
  auto integer = [&](const char* key, lua_Integer value) { lua_pushinteger(L_, value); lua_setfield(L_, -2, key); };
  integer("userid", userid); integer("attacker", attacker); integer("health", health);
  integer("armor", armor); integer("dmg_health", damage_health); integer("dmg_armor", damage_armor);
  integer("hitgroup", hitgroup);
  lua_pushstring(L_, weapon ? weapon : ""); lua_setfield(L_, -2, "weapon");
  if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
    report_error("events.player_hurt");
  }
}

void LuaEngine::fire_player_chat(int userid, bool team_only, const char* text, const char* username) {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  const auto it = event_callbacks_.find("player_chat");
  if (it == event_callbacks_.end()) return;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
  lua_newtable(L_);
  lua_pushinteger(L_, userid); lua_setfield(L_, -2, "userid");
  lua_pushboolean(L_, team_only); lua_setfield(L_, -2, "teamonly");
  lua_pushstring(L_, text ? text : ""); lua_setfield(L_, -2, "text");
  lua_pushstring(L_, username ? username : ""); lua_setfield(L_, -2, "name");
  lua_pushstring(L_, username ? username : "");
  lua_pushcclosure(L_, [](lua_State* state) -> int {
    lua_pushvalue(state, lua_upvalueindex(1)); return 1;
  }, 1);
  lua_setfield(L_, -2, "get_username");
  if (lua_pcall(L_, 1, 1, 0) != LUA_OK) { report_error("events.player_chat"); return; }
  lua_pop(L_, 1); // callback return value (false is advisory for received chat)
}

void LuaEngine::set_event_callback(const std::string& name, int function_index) {
  std::lock_guard lock(mutex_);
  if (auto it = event_callbacks_.find(name); it != event_callbacks_.end()) {
    luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    event_callbacks_.erase(it);
  }
  lua_pushvalue(L_, function_index);
  event_callbacks_[name] = luaL_ref(L_, LUA_REGISTRYINDEX);
}

void LuaEngine::unset_event_callback(const std::string& name) {
  std::lock_guard lock(mutex_);
  if (auto it = event_callbacks_.find(name); it != event_callbacks_.end()) {
    luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    event_callbacks_.erase(it);
  }
}

int LuaEngine::lua_event_set(lua_State* L) {
  auto* self = static_cast<LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* name = lua_tostring(L, lua_upvalueindex(2));
  const int fn = lua_isfunction(L, 2) ? 2 : 1;
  luaL_checktype(L, fn, LUA_TFUNCTION);
  self->set_event_callback(name, fn);
  return 0;
}

int LuaEngine::lua_event_unset(lua_State* L) {
  auto* self = static_cast<LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* name = lua_tostring(L, lua_upvalueindex(2));
  self->unset_event_callback(name);
  return 0;
}

int LuaEngine::lua_event_call(lua_State* L) {
  auto* self = static_cast<LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* name = lua_tostring(L, lua_upvalueindex(2));
  std::lock_guard lock(self->mutex_);
  const auto it = self->event_callbacks_.find(name);
  if (it == self->event_callbacks_.end()) return 0;
  const int top = lua_gettop(L);
  const int first_arg = lua_istable(L, 1) ? 2 : 1;
  lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
  int argc = 0;
  for (int i = first_arg; i <= top; ++i) { lua_pushvalue(L, i); ++argc; }
  if (lua_pcall(L, argc, LUA_MULTRET, 0) != LUA_OK) return lua_error(L);
  return lua_gettop(L) - top;
}

void LuaEngine::register_events_api() {
  static const char* names[] = {
    "render", "tick", "shutdown", "load", "createmove", "createmove_run",
    "level_init", "pre_render", "post_render", "net_update_start", "net_update_end",
    "console_input", "mouse_input", "player_hurt", "player_chat", "player_say", "player_death", "weapon_fire",
    "bullet_impact", "round_start", "round_end", "bomb_planted", "bomb_defused"
  };
  lua_newtable(L_);
  for (const char* name : names) {
    lua_newtable(L_);
    auto add = [&](const char* method, lua_CFunction fn) {
      lua_pushlightuserdata(L_, this); lua_pushstring(L_, name);
      lua_pushcclosure(L_, fn, 2); lua_setfield(L_, -2, method);
    };
    add("set", lua_event_set); add("unset", lua_event_unset); add("call", lua_event_call);
    lua_setfield(L_, -2, name);
  }
  lua_setglobal(L_, "events");
}

int LuaEngine::lua_utils_create_interface(lua_State* L) {
  const char* module_name = luaL_checkstring(L, 1);
  const char* interface_name = luaL_checkstring(L, 2);
  HMODULE module = GetModuleHandleA(module_name);
  if (!module) module = LoadLibraryA(module_name);
  using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
  auto create = module ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface")) : nullptr;
  void* result = create ? create(interface_name, nullptr) : nullptr;
  if (result) lua_pushlightuserdata(L, result); else lua_pushnil(L);
  return 1;
}

int LuaEngine::lua_utils_opcode_scan(lua_State* L) {
  const char* module_name = luaL_checkstring(L, 1);
  const char* signature = luaL_checkstring(L, 2);
  const lua_Integer offset = luaL_optinteger(L, 3, 0);
  HMODULE module = GetModuleHandleA(module_name);
  if (!module) { lua_pushnil(L); return 1; }
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<unsigned char*>(module) + dos->e_lfanew);
  std::vector<int> pattern; std::istringstream stream(signature); std::string token;
  while (stream >> token) pattern.push_back(token == "?" || token == "??" ? -1 : static_cast<int>(std::strtoul(token.c_str(), nullptr, 16)));
  auto* begin = reinterpret_cast<unsigned char*>(module);
  // Scan committed PE sections only. Walking OptionalHeader.SizeOfImage byte by
  // byte can enter PAGE_NOACCESS gaps in client.dll and crash during injection.
  auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD section_index = 0; section_index < nt->FileHeader.NumberOfSections; ++section_index, ++section) {
    if (!(section->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ))) continue;
    auto* section_begin = begin + section->VirtualAddress;
    const size_t section_size = section->Misc.VirtualSize;
    auto* cursor = section_begin;
    auto* section_end = section_begin + section_size;
    while (cursor < section_end) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(cursor, &mbi, sizeof(mbi)) || !mbi.RegionSize) break;
      auto* region_begin = std::max(cursor, static_cast<unsigned char*>(mbi.BaseAddress));
      auto* region_end = std::min(section_end, static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize);
      const DWORD base_protect = mbi.Protect & 0xff;
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
          base_protect != PAGE_NOACCESS;
      if (readable && region_end > region_begin) {
        const size_t region_size = static_cast<size_t>(region_end - region_begin);
        for (size_t i = 0; !pattern.empty() && i + pattern.size() <= region_size; ++i) {
          bool match = true;
          for (size_t j = 0; j < pattern.size(); ++j) if (pattern[j] >= 0 && region_begin[i+j] != pattern[j]) { match=false; break; }
          if (match) { lua_pushlightuserdata(L, region_begin + i + offset); return 1; }
        }
      }
      cursor = region_end > cursor ? region_end : cursor + 0x1000;
    }
  }
  lua_pushnil(L); return 1;
}

int LuaEngine::lua_utils_console_exec(lua_State* L) {
  std::string command;
  const int count = lua_gettop(L);
  for (int i = 1; i <= count; ++i) {
    size_t len = 0; const char* part = CompatToString(L, i, &len);
    if (part) command.append(part, len);
    lua_pop(L, 1);
  }
  if (command.empty()) return luaL_error(L, "utils.console_exec: command is empty");
  auto* self = static_cast<LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
  if (!self) return luaL_error(L, "utils.console_exec: engine context unavailable");
  { std::lock_guard lock(self->mutex_); self->pending_console_commands_.push_back(std::move(command)); }
  return 0;
}

int LuaEngine::lua_utils_is_readable(lua_State* L) {
  const auto address = static_cast<uintptr_t>(luaL_checknumber(L, 1));
  const size_t size = static_cast<size_t>(luaL_optnumber(L, 2, sizeof(void*)));
  if (!address || !size) { lua_pushboolean(L, 0); return 1; }
  uintptr_t cursor = address, end = address + size;
  bool readable = end >= address;
  while (readable && cursor < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { readable = false; break; }
    const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (next <= cursor) { readable = false; break; }
    cursor = next;
  }
  lua_pushboolean(L, readable); return 1;
}

void LuaEngine::register_utils_native_api() {
  lua_newtable(L_);
  lua_pushcfunction(L_, lua_utils_create_interface); lua_setfield(L_, -2, "create_interface");
  lua_pushcfunction(L_, lua_utils_opcode_scan); lua_setfield(L_, -2, "opcode_scan");
  lua_pushcfunction(L_, lua_utils_is_readable); lua_setfield(L_, -2, "is_readable");
  lua_pushlightuserdata(L_, this);
  lua_pushcclosure(L_, lua_utils_console_exec, 1); lua_setfield(L_, -2, "console_exec");
  lua_setglobal(L_, "utils");
}

int LuaEngine::lua_files_list(lua_State* L) {
  const std::filesystem::path dir = std::filesystem::u8path(luaL_checkstring(L, 1));
  lua_newtable(L); lua_Integer index = 1;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().u8string();
    lua_pushlstring(L, reinterpret_cast<const char*>(name.data()), name.size());
    lua_rawseti(L, -2, index++);
  }
  return 1;
}
int LuaEngine::lua_files_create_directory(lua_State* L) {
  std::error_code ec; const bool ok = std::filesystem::create_directories(std::filesystem::u8path(luaL_checkstring(L,1)), ec) || !ec;
  lua_pushboolean(L, ok); return 1;
}
int LuaEngine::lua_files_remove(lua_State* L) {
  std::error_code ec; const auto count = std::filesystem::remove_all(std::filesystem::u8path(luaL_checkstring(L,1)), ec);
  lua_pushboolean(L, !ec && count > 0); return 1;
}
int LuaEngine::lua_files_rename(lua_State* L) {
  std::error_code ec; std::filesystem::rename(std::filesystem::u8path(luaL_checkstring(L,1)), std::filesystem::u8path(luaL_checkstring(L,2)), ec);
  lua_pushboolean(L, !ec); return 1;
}
int LuaEngine::lua_files_size(lua_State* L) {
  std::error_code ec; const auto size = std::filesystem::file_size(std::filesystem::u8path(luaL_checkstring(L,1)), ec);
  if (ec) lua_pushnil(L); else lua_pushinteger(L, static_cast<lua_Integer>(size)); return 1;
}
void LuaEngine::register_files_native_api() {
  lua_newtable(L_);
  lua_pushcfunction(L_, lua_files_list); lua_setfield(L_, -2, "list");
  lua_pushcfunction(L_, lua_files_create_directory); lua_setfield(L_, -2, "create_directory");
  lua_pushcfunction(L_, lua_files_remove); lua_setfield(L_, -2, "remove");
  lua_pushcfunction(L_, lua_files_rename); lua_setfield(L_, -2, "rename");
  lua_pushcfunction(L_, lua_files_size); lua_setfield(L_, -2, "size");
  lua_setglobal(L_, "files");
}

namespace {
std::wstring Utf8Wide(const std::string& text) {
  if (text.empty()) return {};
  const int count=MultiByteToWideChar(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),nullptr,0);
  std::wstring out(count,L'\0'); MultiByteToWideChar(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),out.data(),count); return out;
}
bool HttpRequest(const char* method, const std::string& url, const std::string& body,
                 const std::wstring& extra_headers, std::string& response, DWORD& status) {
  const auto wide=Utf8Wide(url); URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256]{}, path[2048]{}; parts.lpszHostName=host; parts.dwHostNameLength=255; parts.lpszUrlPath=path; parts.dwUrlPathLength=2047;
  if (!WinHttpCrackUrl(wide.c_str(),0,0,&parts)) return false;
  HINTERNET session=WinHttpOpen(L"CS2LuaPlugin/0.4",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0);
  if (!session) return false;
  HINTERNET connect=WinHttpConnect(session,host,parts.nPort,0);
  const DWORD flags=parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;
  const auto verb=Utf8Wide(method); HINTERNET request=connect?WinHttpOpenRequest(connect,verb.c_str(),path,nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags):nullptr;
  bool ok=false;
  if (request) {
    const wchar_t* headers=extra_headers.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:extra_headers.c_str();
    const DWORD headers_len=extra_headers.empty()?0:static_cast<DWORD>(extra_headers.size());
    ok=WinHttpSendRequest(request,headers,headers_len,body.empty()?WINHTTP_NO_REQUEST_DATA:(void*)body.data(),static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0) && WinHttpReceiveResponse(request,nullptr);
    if (ok) {
      DWORD size=sizeof(status); WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr);
      for (;;) { DWORD available=0; if(!WinHttpQueryDataAvailable(request,&available)||!available) break; size_t old=response.size(); response.resize(old+available); DWORD read=0; if(!WinHttpReadData(request,response.data()+old,available,&read)) {ok=false;break;} response.resize(old+read); }
    }
  }
  if(request)WinHttpCloseHandle(request); if(connect)WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return ok;
}
std::wstring LuaHeaders(lua_State* L,int index) {
  std::wstring out; if(!lua_istable(L,index)) return out;
  lua_pushnil(L); while(lua_next(L,index<0?index-1:index)) { const char* k=lua_tostring(L,-2); const char* v=lua_tostring(L,-1); if(k&&v) out+=Utf8Wide(std::string(k)+": "+v+"\r\n"); lua_pop(L,1); } return out;
}
}

int LuaEngine::lua_network_get(lua_State* L) {
  const std::string url=luaL_checkstring(L,1); int callback=lua_isfunction(L,2)?2:(lua_isfunction(L,3)?3:0);
  if (!callback) return luaL_error(L,"network.get requires callback(response, status)");
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1)));
  const auto headers=LuaHeaders(L,2); lua_pushvalue(L,callback); const int ref=luaL_ref(L,LUA_REGISTRYINDEX);
  self->network_workers_.emplace_back([self,url,headers,ref] {
    std::string response; DWORD status=0; const bool ok=HttpRequest("GET",url,"",headers,response,status);
    if(!ok) response="network.get failed: "+std::to_string(GetLastError());
    std::lock_guard lock(self->network_mutex_); self->network_results_.push_back({ref,std::move(response),status,ok});
  });
  lua_pushboolean(L,1); return 1;
}
int LuaEngine::lua_network_post(lua_State* L) {
  const std::string url=luaL_checkstring(L,1); std::string body;
  if(lua_isstring(L,2)) body=lua_tostring(L,2); else if(lua_istable(L,2)) { lua_getglobal(L,"json"); lua_getfield(L,-1,"encode"); lua_pushvalue(L,2); if(lua_pcall(L,1,1,0)!=LUA_OK)return lua_error(L); body=lua_tostring(L,-1); lua_pop(L,2); }
  int callback=lua_isfunction(L,2)?2:(lua_isfunction(L,3)?3:(lua_isfunction(L,4)?4:0));
  if (!callback) return luaL_error(L,"network.post requires callback(response, status)");
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1)));
  auto headers=LuaHeaders(L,3); if(headers.empty()) headers=L"Content-Type: application/json\r\n";
  lua_pushvalue(L,callback); const int ref=luaL_ref(L,LUA_REGISTRYINDEX);
  self->network_workers_.emplace_back([self,url,body=std::move(body),headers=std::move(headers),ref] {
    std::string response; DWORD status=0; const bool ok=HttpRequest("POST",url,body,headers,response,status);
    if(!ok) response="network.post failed: "+std::to_string(GetLastError());
    std::lock_guard lock(self->network_mutex_); self->network_results_.push_back({ref,std::move(response),status,ok});
  });
  lua_pushboolean(L,1); return 1;
}
void LuaEngine::register_network_native_api() {
  lua_newtable(L_);
  lua_pushlightuserdata(L_,this); lua_pushcclosure(L_,lua_network_get,1); lua_setfield(L_,-2,"get");
  lua_pushlightuserdata(L_,this); lua_pushcclosure(L_,lua_network_post,1); lua_setfield(L_,-2,"post");
  lua_setglobal(L_,"network");
}

void LuaEngine::join_network_workers() {
  for (auto& worker : network_workers_) if (worker.joinable()) worker.join();
  network_workers_.clear();
  std::lock_guard lock(network_mutex_); network_results_.clear();
}

std::vector<std::filesystem::path> LuaEngine::loaded_scripts() const { std::lock_guard lock(mutex_); return loaded_scripts_; }

std::vector<std::filesystem::path> LuaEngine::available_scripts() const {
  std::lock_guard lock(mutex_);
  std::vector<std::filesystem::path> out;
  if (scripts_dir_.empty() || !std::filesystem::exists(scripts_dir_)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(scripts_dir_)) {
    if (entry.is_regular_file() && entry.path().extension() == L".lua") out.push_back(entry.path());
  }
  return out;
}

bool LuaEngine::has_loaded_scripts() const { std::lock_guard lock(mutex_); return !loaded_scripts_.empty(); }

namespace {
LuaEngine::UiItem* FindUiItem(std::vector<LuaEngine::UiItem>& items, int id) {
  for (auto& item : items) if (item.id == id) return &item;
  return nullptr;
}
}

int LuaEngine::lua_ui_create_item(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1)));
  UiItem item{}; item.id=self->next_ui_id_++; item.script=self->current_script_;
  item.group=luaL_checkstring(L,1); item.type=luaL_checkstring(L,2); item.name=luaL_checkstring(L,3);
  if (item.type=="switch") item.bool_value=item.initial_bool=lua_toboolean(L,4)!=0;
  else if (item.type=="slider") {
    item.number_value=item.initial_number=luaL_optnumber(L,4,0); item.min=luaL_optnumber(L,5,0);
    item.max=luaL_optnumber(L,6,100); item.scale=luaL_optnumber(L,7,1);
  } else if (item.type=="input") item.text=luaL_optstring(L,4,"");
  else if (item.type=="hotkey") item.index_value=item.initial_index=static_cast<int>(luaL_optinteger(L,4,0));
  else if (item.type=="combo" || item.type=="selectable" || item.type=="list" || item.type=="listable") {
    item.index_value=item.initial_index=static_cast<int>(luaL_optinteger(L,4,1))-1;
    // Lua compatibility layer uses the shared create signature:
    // group, type, name, initial, min, max, options.
    if (lua_istable(L,7)) { const int count=static_cast<int>(lua_objlen(L,7)); for(int i=1;i<=count;++i){lua_rawgeti(L,7,i); if(const char* s=lua_tostring(L,-1))item.options.emplace_back(s); lua_pop(L,1);} }
    if(item.type=="selectable"||item.type=="listable")item.selected.assign(item.options.size(),false);
  }
  self->ui_items_.push_back(std::move(item)); lua_pushinteger(L,self->ui_items_.back().id); return 1;
}
int LuaEngine::lua_ui_get(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1))); auto* item=FindUiItem(self->ui_items_,luaL_checkinteger(L,1));
  if(!item){lua_pushnil(L);return 1;} if(item->type=="switch"){lua_pushboolean(L,item->bool_value);return 1;}
  if(item->type=="slider"){lua_pushnumber(L,item->number_value);return 1;} if(item->type=="input"){lua_pushstring(L,item->text.c_str());return 1;}
  if(item->type=="selectable"||item->type=="listable"){
    if(lua_gettop(L)>=2){int index=-1;if(lua_isnumber(L,2))index=static_cast<int>(lua_tointeger(L,2))-1;else if(const char* name=lua_tostring(L,2)){for(int i=0;i<(int)item->options.size();++i)if(item->options[i]==name){index=i;break;}}lua_pushboolean(L,index>=0&&index<(int)item->selected.size()&&item->selected[index]);return 1;}
    lua_newtable(L);int out=1;for(int i=0;i<(int)item->selected.size();++i)if(item->selected[i]){lua_pushstring(L,item->options[i].c_str());lua_rawseti(L,-2,out++);}return 1;
  }
  if(item->type=="combo"||item->type=="list"){lua_pushinteger(L,item->index_value+1);return 1;}
  if(item->type=="hotkey"){lua_pushinteger(L,item->index_value);return 1;} lua_pushnil(L);return 1;
}
int LuaEngine::lua_ui_set(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1))); auto* item=FindUiItem(self->ui_items_,luaL_checkinteger(L,1)); if(!item)return 0;
  if(item->type=="switch")item->bool_value=lua_toboolean(L,2)!=0; else if(item->type=="slider")item->number_value=luaL_checknumber(L,2);
  else if(item->type=="input")item->text=luaL_checkstring(L,2);
  else if(item->type=="selectable"||item->type=="listable"){
    std::fill(item->selected.begin(),item->selected.end(),false);
    auto select=[&](int arg){int index=-1;if(lua_isnumber(L,arg))index=static_cast<int>(lua_tointeger(L,arg))-1;else if(const char* name=lua_tostring(L,arg)){for(int i=0;i<(int)item->options.size();++i)if(item->options[i]==name){index=i;break;}}if(index>=0&&index<(int)item->selected.size())item->selected[index]=true;};
    if(lua_istable(L,2)){const int count=static_cast<int>(lua_objlen(L,2));for(int i=1;i<=count;++i){lua_rawgeti(L,2,i);select(lua_gettop(L));lua_pop(L,1);}}else for(int arg=2;arg<=lua_gettop(L);++arg)select(arg);
  } else item->index_value=static_cast<int>(luaL_checkinteger(L,2))-(item->type=="hotkey"?0:1); return 0;
}
int LuaEngine::lua_ui_set_callback(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1))); auto* item=FindUiItem(self->ui_items_,luaL_checkinteger(L,1)); if(!item)return 0;
  if(item->callback_ref!=LUA_NOREF)luaL_unref(L,LUA_REGISTRYINDEX,item->callback_ref); item->callback_ref=LUA_NOREF;
  if(lua_isfunction(L,2)){lua_pushvalue(L,2);item->callback_ref=luaL_ref(L,LUA_REGISTRYINDEX);} return 0;
}
int LuaEngine::lua_ui_set_state(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1))); auto* item=FindUiItem(self->ui_items_,luaL_checkinteger(L,1)); if(!item){lua_pushnil(L);return 1;}
  const std::string field=luaL_checkstring(L,2); bool* value=field=="visible"?&item->visible:&item->disabled;
  if(lua_gettop(L)>=3)*value=lua_toboolean(L,3)!=0; lua_pushboolean(L,*value);return 1;
}
void LuaEngine::register_ui_native_api() {
  lua_newtable(L_); auto add=[&](const char* name,lua_CFunction fn){lua_pushlightuserdata(L_,this);lua_pushcclosure(L_,fn,1);lua_setfield(L_,-2,name);};
  add("_create_item",lua_ui_create_item);add("_get",lua_ui_get);add("_set",lua_ui_set);add("_set_callback",lua_ui_set_callback);add("_state",lua_ui_set_state);lua_setglobal(L_,"ui_native");
}
bool LuaEngine::script_has_ui(const std::filesystem::path& path) const {
  std::lock_guard lock(mutex_); const auto target=std::filesystem::absolute(path).lexically_normal();
  return std::any_of(ui_items_.begin(),ui_items_.end(),[&](const UiItem& item){return item.script==target;});
}
void LuaEngine::render_script_ui(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_); const auto target=std::filesystem::absolute(path).lexically_normal(); std::string group;
  for(size_t index=0;index<ui_items_.size();++index){auto& item=ui_items_[index];if(item.script!=target||!item.visible)continue;
    ImGui::PushID(item.id); if(item.group!=group){group=item.group;ImGui::SeparatorText(group.c_str());} if(item.disabled)ImGui::BeginDisabled(); bool changed=false,clicked=false;
    if(item.type=="switch")changed=ImGui::Checkbox(item.name.c_str(),&item.bool_value);
    else if(item.type=="slider"){float v=static_cast<float>(item.number_value);changed=ImGui::SliderFloat(item.name.c_str(),&v,static_cast<float>(item.min),static_cast<float>(item.max));item.number_value=v;}
    else if(item.type=="selectable"||item.type=="listable"){
      std::string preview;for(int i=0;i<(int)item.options.size();++i)if(item.selected[i]){if(!preview.empty())preview+=", ";preview+=item.options[i];}if(preview.empty())preview="None";
      if(ImGui::BeginCombo(item.name.c_str(),preview.c_str())){for(int i=0;i<(int)item.options.size();++i){bool selected=item.selected[i];if(ImGui::Selectable(item.options[i].c_str(),selected,ImGuiSelectableFlags_DontClosePopups)){item.selected[i]=!selected;changed=true;}}ImGui::EndCombo();}
    }
    else if(item.type=="combo"||item.type=="list"){const char* preview=item.index_value>=0&&item.index_value<(int)item.options.size()?item.options[item.index_value].c_str():"";if(ImGui::BeginCombo(item.name.c_str(),preview)){for(int i=0;i<(int)item.options.size();++i){bool selected=i==item.index_value;if(ImGui::Selectable(item.options[i].c_str(),selected)){item.index_value=i;changed=true;}if(selected)ImGui::SetItemDefaultFocus();}ImGui::EndCombo();}}
    else if(item.type=="button")clicked=ImGui::Button(item.name.c_str());
    else if(item.type=="input"){char buffer[512]{};strncpy_s(buffer,item.text.c_str(),_TRUNCATE);if(ImGui::InputText(item.name.c_str(),buffer,sizeof(buffer))){item.text=buffer;changed=true;}}
    else if(item.type=="hotkey"){int key=item.index_value;if(ImGui::InputInt(item.name.c_str(),&key)){item.index_value=key;changed=true;}}
    else if(item.type=="label")ImGui::TextUnformatted(item.name.c_str());
    if(item.disabled)ImGui::EndDisabled(); const int callback=item.callback_ref; ImGui::PopID();
    if((changed||clicked)&&callback!=LUA_NOREF){lua_rawgeti(L_,LUA_REGISTRYINDEX,callback);if(lua_pcall(L_,0,0,0)!=LUA_OK)report_error("ui callback");}
  }
}

void LuaEngine::call_noarg(const char* fn) {
  lua_getglobal(L_, fn);
  if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return; }
  if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
    report_error(fn);
  }
}
