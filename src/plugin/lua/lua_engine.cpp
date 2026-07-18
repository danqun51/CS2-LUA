#include "lua_engine.hpp"
#include "embedded_libraries.hpp"
#include <iostream>
#include <string>
#include <windows.h>
#include <algorithm>
#include <fstream>
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

std::string PathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool IsVirtualKeyDown(int key) {
  return key > 0 && key < 256 && (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool AnyBindableKeyDown() {
  for (int key = 1; key < 256; ++key)
    if (IsVirtualKeyDown(key)) return true;
  return false;
}

std::string WideToUtf8(const wchar_t* text) {
  if (!text || !*text) return {};
  const int length = static_cast<int>(wcslen(text));
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length,
                                        nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string result(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), bytes,
                      nullptr, nullptr);
  return result;
}

std::string VirtualKeyName(int key) {
  switch (key) {
    case 0: return "未绑定";
    case VK_LBUTTON: return "鼠标左键";
    case VK_RBUTTON: return "鼠标右键";
    case VK_MBUTTON: return "鼠标中键";
    case VK_XBUTTON1: return "鼠标侧键 1";
    case VK_XBUTTON2: return "鼠标侧键 2";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_ESCAPE: return "Esc";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
  }
  UINT scan = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
  LONG key_name_parameter = static_cast<LONG>(scan << 16);
  switch (key) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
    case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU:
      key_name_parameter |= 1 << 24;
      break;
  }
  wchar_t name[96]{};
  if (GetKeyNameTextW(key_name_parameter, name,
                      static_cast<int>(_countof(name))) > 0)
    return WideToUtf8(name);
  char fallback[24]{};
  sprintf_s(fallback, "VK 0x%02X", key);
  return fallback;
}

int LoadUnicodeLuaFile(lua_State* L, const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    const std::string display = PathToUtf8(path);
    lua_pushfstring(L, "无法打开 Lua 文件：%s", display.c_str());
    return LUA_ERRFILE;
  }

  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    const std::string display = PathToUtf8(path);
    lua_pushfstring(L, "读取 Lua 文件失败：%s", display.c_str());
    return LUA_ERRFILE;
  }
  // luaL_loadfile normally handles the UTF-8 BOM itself. Since Unicode paths
  // are loaded through the wide Windows filesystem API, strip it here before
  // handing the source buffer to LuaJIT.
  if (source.size() >= 3 &&
      static_cast<unsigned char>(source[0]) == 0xEF &&
      static_cast<unsigned char>(source[1]) == 0xBB &&
      static_cast<unsigned char>(source[2]) == 0xBF) {
    source.erase(0, 3);
  }
  const std::string chunk_name = "@" + PathToUtf8(path);
  return luaL_loadbuffer(L, source.data(), source.size(), chunk_name.c_str());
}

void ApplyCallerEnvironment(lua_State* L, int function_index) {
  lua_Debug frame{};
  if (!lua_getstack(L, 1, &frame) || !lua_getinfo(L, "f", &frame)) return;
  lua_getfenv(L, -1);
  lua_setfenv(L, function_index);
  lua_pop(L, 1);
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
  // LuaJIT's stock Windows file loader uses the active ANSI code page. Replace
  // it with a filesystem::path based loader so UTF-8 Lua paths work everywhere.
  lua_pushcfunction(L_, lua_unicode_loadfile);
  lua_setglobal(L_, "loadfile");
  lua_pushcfunction(L_, lua_unicode_dofile);
  lua_setglobal(L_, "dofile");
  lua_getglobal(L_, "package");
  lua_getfield(L_, -1, "loaders");
  if (lua_istable(L_, -1)) {
    const int count = static_cast<int>(lua_objlen(L_, -1));
    for (int i = count + 1; i > 2; --i) {
      lua_rawgeti(L_, -1, i - 1);
      lua_rawseti(L_, -2, i);
    }
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, lua_unicode_module_searcher, 1);
    lua_rawseti(L_, -2, 2);
  }
  lua_pop(L_, 2);
  register_events_api();
  register_utils_native_api();
  register_files_native_api();
  register_network_native_api();
  register_ui_native_api();
  std::cout << "[LuaEngine] initialized Lua " << LUA_VERSION << "\n";
}

void LuaEngine::set_scripts_dir(const std::filesystem::path& dir) {
  std::lock_guard lock(mutex_);
  scripts_dir_ = std::filesystem::absolute(dir).lexically_normal();
  load_autoload_settings();
  if (L_) configure_package_path(scripts_dir_);
}

void LuaEngine::load_autoload_settings() {
  autoload_scripts_.clear();
  if (scripts_dir_.empty()) return;
  std::ifstream input(scripts_dir_ / L".cs2lua_autoload", std::ios::binary);
  if (!input) return;
  std::string line;
  bool first = true;
  while (std::getline(input, line)) {
    if (first && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }
    first = false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto path = std::filesystem::absolute(
        scripts_dir_ / std::filesystem::u8path(line)).lexically_normal();
    if (std::find(autoload_scripts_.begin(), autoload_scripts_.end(), path) ==
        autoload_scripts_.end()) {
      autoload_scripts_.push_back(path);
    }
  }
}

void LuaEngine::save_autoload_settings() const {
  if (scripts_dir_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(scripts_dir_, ec);
  std::ofstream output(scripts_dir_ / L".cs2lua_autoload",
                       std::ios::binary | std::ios::trunc);
  if (!output) return;
  for (const auto& path : autoload_scripts_) {
    auto relative = std::filesystem::relative(path, scripts_dir_, ec);
    if (ec) { ec.clear(); relative = path.filename(); }
    output << PathToUtf8(relative) << '\n';
  }
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
  if (LoadUnicodeLuaFile(L_, path) != LUA_OK || lua_pcall(L_, 0, 0, 0) != LUA_OK) {
    report_error("compat library", &path); return false;
  }
  if (std::find(library_files_.begin(), library_files_.end(), path) == library_files_.end()) library_files_.push_back(path);
  return true;
}

bool LuaEngine::load_table_library(const std::filesystem::path& path, const char* global_name) {
  std::lock_guard lock(mutex_);
  if (!L_) initialize();
  if (LoadUnicodeLuaFile(L_, path) != LUA_OK || lua_pcall(L_, 0, 1, 0) != LUA_OK) {
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
  if (path) out += " [" + PathToUtf8(path->filename()) + "]";
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
  script_states_.clear();
  event_callbacks_.clear();
  ui_items_.clear(); next_ui_id_ = 1;
  initialize();
  for (const auto& [name, global] : embedded_libraries_) {
    if (!execute_embedded_library(name, global)) {
      ConsoleLine("[Lua Error] failed to restore embedded library: " + name);
    }
  }
  for (const auto& library : library_files_) {
    if (LoadUnicodeLuaFile(L_, library) != LUA_OK || lua_pcall(L_, 0, 0, 0) != LUA_OK)
      lua_pop(L_, 1);
  }
  for (const auto& [library, global] : table_library_files_) {
    if (LoadUnicodeLuaFile(L_, library) == LUA_OK && lua_pcall(L_, 0, 1, 0) == LUA_OK && lua_istable(L_, -1))
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

int LuaEngine::lua_unicode_loadfile(lua_State* L) {
  const char* filename = luaL_checkstring(L, 1);
  const std::filesystem::path path = std::filesystem::u8path(filename);
  lua_settop(L, 0);
  if (LoadUnicodeLuaFile(L, path) == LUA_OK) {
    ApplyCallerEnvironment(L, 1);
    return 1;
  }
  lua_pushnil(L);
  lua_insert(L, -2);
  return 2;
}

int LuaEngine::lua_unicode_dofile(lua_State* L) {
  const char* filename = luaL_checkstring(L, 1);
  const std::filesystem::path path = std::filesystem::u8path(filename);
  lua_settop(L, 0);
  if (LoadUnicodeLuaFile(L, path) != LUA_OK) return lua_error(L);
  ApplyCallerEnvironment(L, 1);
  if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) return lua_error(L);
  return lua_gettop(L);
}

int LuaEngine::lua_unicode_module_searcher(lua_State* L) {
  auto* self = static_cast<LuaEngine*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  const char* raw_name = luaL_checkstring(L, 1);
  if (!self || self->scripts_dir_.empty()) {
    lua_pushfstring(L, "\n\tLua 脚本目录尚未配置：%s", raw_name);
    return 1;
  }

  std::string relative = raw_name;
  std::replace(relative.begin(), relative.end(), '.', '/');
  const auto module_path = std::filesystem::u8path(relative);
  auto module_file = module_path;
  module_file += L".lua";
  const std::filesystem::path candidates[] = {
      self->scripts_dir_ / module_file,
      self->scripts_dir_ / module_path / L"init.lua",
  };
  std::error_code ec;
  for (const auto& candidate : candidates) {
    if (!std::filesystem::is_regular_file(candidate, ec)) {
      ec.clear();
      continue;
    }
    if (LoadUnicodeLuaFile(L, candidate) != LUA_OK) return lua_error(L);
    return 1;
  }

  const std::string display = PathToUtf8(self->scripts_dir_ / module_path);
  lua_pushfstring(L, "\n\t找不到 UTF-8 Lua 模块：%s（搜索位置：%s）",
                  raw_name, display.c_str());
  return 1;
}

int LuaEngine::lua_script_require(lua_State* L) {
  auto* self = static_cast<LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
  const char* raw_name = luaL_checkstring(L, 1);
  lua_getfield(L, lua_upvalueindex(2), "__loaded_modules");
  const int loaded_index = lua_gettop(L);
  lua_getfield(L, loaded_index, raw_name);
  if (!lua_isnil(L, -1)) {
    lua_remove(L, loaded_index);
    return 1;
  }
  lua_pop(L, 1);

  std::string relative = raw_name;
  std::replace(relative.begin(), relative.end(), '.', '/');
  const auto module_path = std::filesystem::u8path(relative);
  auto module_file = module_path;
  module_file += L".lua";
  const std::filesystem::path candidates[] = {
      self->scripts_dir_ / module_file,
      self->scripts_dir_ / module_path / L"init.lua",
  };
  std::error_code ec;
  for (const auto& candidate : candidates) {
    if (!std::filesystem::is_regular_file(candidate, ec)) { ec.clear(); continue; }
    if (LoadUnicodeLuaFile(L, candidate) != LUA_OK) return lua_error(L);
    const int function_index = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_setfenv(L, function_index);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) return lua_error(L);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_pushvalue(L, -1);
    lua_setfield(L, loaded_index, raw_name);
    lua_remove(L, loaded_index);
    return 1;
  }

  // Native/preloaded modules such as ffi and bit are process-wide and safe to
  // share. Lua modules from the scripts directory never take this fallback.
  lua_getglobal(L, "require");
  lua_pushstring(L, raw_name);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) return lua_error(L);
  lua_pushvalue(L, -1);
  lua_setfield(L, loaded_index, raw_name);
  lua_remove(L, loaded_index);
  return 1;
}

void LuaEngine::set_active_script(const std::filesystem::path& path) {
  if (path.empty()) {
    lua_pushnil(L_);
  } else {
    const std::string value = PathToUtf8(path);
    lua_pushlstring(L_, value.data(), value.size());
  }
  lua_setglobal(L_, "__cs2lua_active_script");
}

std::filesystem::path LuaEngine::active_script_owner() const {
  if (!current_script_.empty()) return current_script_;
  lua_getglobal(L_, "__cs2lua_active_script");
  size_t length = 0;
  const char* value = lua_tolstring(L_, -1, &length);
  std::filesystem::path result;
  if (value && length) result = std::filesystem::u8path(std::string(value, length));
  lua_pop(L_, 1);
  return result;
}

bool LuaEngine::call_script_noarg(const std::filesystem::path& path, const char* fn) {
  const auto state = std::find_if(script_states_.begin(), script_states_.end(),
      [&](const ScriptState& value) { return value.path == path; });
  if (state == script_states_.end()) return true;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, state->environment_ref);
  lua_getfield(L_, -1, fn);
  lua_remove(L_, -2);
  if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return true; }
  const auto previous = current_script_;
  current_script_ = path;
  set_active_script(path);
  const bool ok = lua_pcall(L_, 0, 0, 0) == LUA_OK;
  if (!ok) report_error(fn, &path);
  current_script_ = previous;
  set_active_script(previous);
  return ok;
}

bool LuaEngine::call_script_number(const std::filesystem::path& path, const char* fn,
                                   double value) {
  const auto state = std::find_if(script_states_.begin(), script_states_.end(),
      [&](const ScriptState& entry) { return entry.path == path; });
  if (state == script_states_.end()) return true;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, state->environment_ref);
  lua_getfield(L_, -1, fn);
  lua_remove(L_, -2);
  if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return true; }
  const auto previous = current_script_;
  current_script_ = path;
  set_active_script(path);
  lua_pushnumber(L_, value);
  const bool ok = lua_pcall(L_, 1, 0, 0) == LUA_OK;
  if (!ok) report_error(fn, &path);
  current_script_ = previous;
  set_active_script(previous);
  return ok;
}

void LuaEngine::fire_event_for_script(const std::string& name,
                                      const std::filesystem::path& path) {
  const auto found = event_callbacks_.find(name);
  if (found == event_callbacks_.end()) return;
  const auto callbacks = found->second;
  for (const auto& callback : callbacks) {
    if (callback.script != path) continue;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, callback.function_ref);
    const auto previous = current_script_;
    current_script_ = path;
    set_active_script(path);
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
      const std::string context = "events." + name;
      report_error(context.c_str(), &path);
    }
    current_script_ = previous;
    set_active_script(previous);
  }
}

void LuaEngine::cancel_script_timers(const std::filesystem::path& path) {
  lua_getglobal(L_, "__nl_cancel_script_timers");
  if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return; }
  const std::string owner = PathToUtf8(path);
  lua_pushlstring(L_, owner.data(), owner.size());
  if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
    report_error("cancel script timers", &path);
}

void LuaEngine::flush_script_v8(const std::filesystem::path& path) {
  lua_getglobal(L_, "panorama");
  if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
  lua_getfield(L_, -1, "_flush_script");
  lua_remove(L_, -2);
  if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return; }
  const std::string owner = PathToUtf8(path);
  lua_pushlstring(L_, owner.data(), owner.size());
  if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
    report_error("Panorama V8 cleanup", &path);
}

void LuaEngine::remove_script_resources(const std::filesystem::path& path) {
  for (auto it = event_callbacks_.begin(); it != event_callbacks_.end();) {
    auto& callbacks = it->second;
    for (auto callback = callbacks.begin(); callback != callbacks.end();) {
      if (callback->script == path) {
        luaL_unref(L_, LUA_REGISTRYINDEX, callback->function_ref);
        callback = callbacks.erase(callback);
      } else ++callback;
    }
    if (callbacks.empty()) it = event_callbacks_.erase(it); else ++it;
  }
  for (auto item = ui_items_.begin(); item != ui_items_.end();) {
    if (item->script == path) {
      if (item->callback_ref != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, item->callback_ref);
      item = ui_items_.erase(item);
    } else ++item;
  }
  for (auto& worker : network_workers_) if (worker.joinable()) worker.join();
  network_workers_.clear();
  std::lock_guard network_lock(network_mutex_);
  for (auto result = network_results_.begin(); result != network_results_.end();) {
    if (result->script == path) {
      luaL_unref(L_, LUA_REGISTRYINDEX, result->callback_ref);
      result = network_results_.erase(result);
    } else ++result;
  }
}

bool LuaEngine::load_script(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  const auto normalized = std::filesystem::absolute(path).lexically_normal();
  if (std::find(loaded_scripts_.begin(), loaded_scripts_.end(), normalized) != loaded_scripts_.end()) return true;
  if (!L_) initialize();
  if (LoadUnicodeLuaFile(L_, normalized) != LUA_OK) {
    report_error("load failed", &normalized);
    return false;
  }
  const int function_index = lua_gettop(L_);
  lua_newtable(L_);
  const int environment_index = lua_gettop(L_);
  lua_newtable(L_);
  lua_pushvalue(L_, LUA_GLOBALSINDEX);
  lua_setfield(L_, -2, "__index");
  lua_setmetatable(L_, environment_index);
  lua_pushvalue(L_, environment_index);
  lua_setfield(L_, environment_index, "_G");
  const std::string script_name = PathToUtf8(normalized);
  lua_pushlstring(L_, script_name.data(), script_name.size());
  lua_setfield(L_, environment_index, "_SCRIPT_PATH");
  lua_newtable(L_);
  lua_setfield(L_, environment_index, "__loaded_modules");
  lua_pushlightuserdata(L_, this);
  lua_pushvalue(L_, environment_index);
  lua_pushcclosure(L_, lua_script_require, 2);
  lua_setfield(L_, environment_index, "require");
  lua_pushvalue(L_, environment_index);
  lua_setfenv(L_, function_index);
  lua_pushvalue(L_, environment_index);
  const int environment_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  lua_remove(L_, environment_index);
  current_script_ = normalized;
  set_active_script(normalized);
  if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
    cancel_script_timers(normalized);
    flush_script_v8(normalized);
    remove_script_resources(normalized);
    luaL_unref(L_, LUA_REGISTRYINDEX, environment_ref);
    current_script_.clear();
    set_active_script({});
    report_error("load failed", &normalized);
    return false;
  }
  current_script_.clear();
  set_active_script({});
  script_states_.push_back({normalized, environment_ref});
  loaded_scripts_.push_back(normalized);
  std::cout << "[LuaEngine] loaded: " << PathToUtf8(path) << "\n";
  call_script_noarg(normalized, "on_load");
  fire_event_for_script("load", normalized);
  return true;
}

bool LuaEngine::unload_script(const std::filesystem::path& path) {
  std::lock_guard lock(mutex_);
  const auto target = std::filesystem::absolute(path).lexically_normal();
  if (std::find(loaded_scripts_.begin(), loaded_scripts_.end(), target) == loaded_scripts_.end()) return true;
  fire_event_for_script("shutdown", target);
  const bool ok = call_script_noarg(target, "on_unload");
  cancel_script_timers(target);
  flush_script_v8(target);
  remove_script_resources(target);
  if (const auto state = std::find_if(script_states_.begin(), script_states_.end(),
          [&](const ScriptState& value) { return value.path == target; });
      state != script_states_.end()) {
    luaL_unref(L_, LUA_REGISTRYINDEX, state->environment_ref);
    script_states_.erase(state);
  }
  loaded_scripts_.erase(std::remove(loaded_scripts_.begin(), loaded_scripts_.end(), target),
                         loaded_scripts_.end());
  lua_gc(L_, LUA_GCCOLLECT, 0);
  set_active_script({});
  std::cout << "[LuaEngine] unloaded: " << PathToUtf8(target) << "\n";
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
  const auto scripts = loaded_scripts_;
  for (const auto& script : scripts) unload_script(script);
  std::cout << "[LuaEngine] unloaded all scripts\n";
}

void LuaEngine::reload_all() {
  std::lock_guard lock(mutex_);
  const auto dir = scripts_dir_;
  unload_all();
  scripts_dir_ = dir;
  load_directory(scripts_dir_);
  std::cout << "[LuaEngine] reloaded scripts\n";
}

void LuaEngine::tick(float dt) {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  // Hotkeys keep running even while the script settings page and the whole
  // plugin menu are closed. A toggle reacts only to the physical down edge;
  // a hold bind mirrors the key state continuously.
  std::vector<std::pair<std::filesystem::path,int>> hotkey_callbacks;
  for (auto& item : ui_items_) {
    if (item.type != "hotkey") continue;
    const bool was_active=item.hotkey_active;
    if (item.index_value <= 0 || item.index_value >= 256) {
      item.hotkey_active = false;
      item.hotkey_previous_down = false;
    } else {
      const bool down = IsVirtualKeyDown(item.index_value);
      if (!item.hotkey_capturing) {
        if (item.hotkey_mode == 2) {
          if (down && !item.hotkey_previous_down)
            item.hotkey_active = !item.hotkey_active;
        } else {
          item.hotkey_active = down;
        }
      }
      item.hotkey_previous_down = down;
    }
    if (was_active!=item.hotkey_active && item.callback_ref!=LUA_NOREF)
      hotkey_callbacks.emplace_back(item.script,item.callback_ref);
  }
  for (const auto& [script,callback] : hotkey_callbacks) {
    if (std::find(loaded_scripts_.begin(),loaded_scripts_.end(),script)==
        loaded_scripts_.end()) continue;
    lua_rawgeti(L_,LUA_REGISTRYINDEX,callback);
    const auto previous=current_script_;
    current_script_=script;
    set_active_script(script);
    if(lua_pcall(L_,0,0,0)!=LUA_OK) report_error("hotkey callback",&script);
    current_script_=previous;
    set_active_script(previous);
  }
  std::vector<NetworkResult> network_results;
  {
    std::lock_guard network_lock(network_mutex_);
    network_results.swap(network_results_);
  }
  for (auto& result : network_results) {
    if (std::find(loaded_scripts_.begin(), loaded_scripts_.end(), result.script) ==
        loaded_scripts_.end()) {
      luaL_unref(L_, LUA_REGISTRYINDEX, result.callback_ref);
      continue;
    }
    lua_rawgeti(L_, LUA_REGISTRYINDEX, result.callback_ref);
    lua_pushlstring(L_, result.body.data(), result.body.size());
    lua_pushinteger(L_, result.status);
    const auto previous = current_script_;
    current_script_ = result.script;
    set_active_script(result.script);
    if (lua_pcall(L_, 2, 0, 0) != LUA_OK) report_error("network callback");
    current_script_ = previous;
    set_active_script(previous);
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
  const auto scripts = loaded_scripts_;
  for (const auto& script : scripts) call_script_number(script, "on_tick", dt);
}

void LuaEngine::fire_event(const std::string& name) {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  const auto it = event_callbacks_.find(name);
  if (it == event_callbacks_.end()) return;
  const auto callbacks = it->second;
  for (const auto& callback : callbacks) {
    if (std::find(loaded_scripts_.begin(), loaded_scripts_.end(), callback.script) ==
        loaded_scripts_.end()) continue;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, callback.function_ref);
    const auto previous = current_script_;
    current_script_ = callback.script;
    set_active_script(callback.script);
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
      const std::string context = "events." + name;
      report_error(context.c_str(), &callback.script);
    }
    current_script_ = previous;
    set_active_script(previous);
  }
}

void LuaEngine::load_autoload_scripts() {
  std::lock_guard lock(mutex_);
  if (scripts_dir_.empty()) return;
  if (!std::filesystem::exists(scripts_dir_)) {
    std::filesystem::create_directories(scripts_dir_);
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(scripts_dir_)) {
    if (!entry.is_regular_file() || entry.path().extension() != L".lua") continue;
    const auto normalized = std::filesystem::absolute(entry.path()).lexically_normal();
    if (std::find(autoload_scripts_.begin(), autoload_scripts_.end(), normalized) !=
        autoload_scripts_.end()) {
      load_script(normalized);
    }
  }
}

bool LuaEngine::try_fire_event(const std::string& name) {
  std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return false;  // plugin thread busy; skip this frame
  if (!L_) return true;
  const auto it = event_callbacks_.find(name);
  if (it == event_callbacks_.end()) return true;
  const auto callbacks = it->second;
  for (const auto& callback : callbacks) {
    if (std::find(loaded_scripts_.begin(), loaded_scripts_.end(), callback.script) ==
        loaded_scripts_.end()) continue;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, callback.function_ref);
    const auto previous = current_script_;
    current_script_ = callback.script;
    set_active_script(callback.script);
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
      const std::string context = "events." + name;
      report_error(context.c_str(), &callback.script);
    }
    current_script_ = previous;
    set_active_script(previous);
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
  const auto callbacks = it->second;
  for (const auto& callback : callbacks) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, callback.function_ref);
    lua_newtable(L_);
    auto integer = [&](const char* key, lua_Integer value) { lua_pushinteger(L_, value); lua_setfield(L_, -2, key); };
    integer("userid", userid); integer("attacker", attacker); integer("health", health);
    integer("armor", armor); integer("dmg_health", damage_health); integer("dmg_armor", damage_armor);
    integer("hitgroup", hitgroup);
    lua_pushstring(L_, weapon ? weapon : ""); lua_setfield(L_, -2, "weapon");
    const auto previous = current_script_; current_script_ = callback.script; set_active_script(callback.script);
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) report_error("events.player_hurt", &callback.script);
    current_script_ = previous; set_active_script(previous);
  }
}

void LuaEngine::fire_player_chat(int userid, bool team_only, const char* text, const char* username) {
  std::lock_guard lock(mutex_);
  if (!L_) return;
  const auto it = event_callbacks_.find("player_chat");
  if (it == event_callbacks_.end()) return;
  const auto callbacks = it->second;
  for (const auto& callback : callbacks) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, callback.function_ref);
    lua_newtable(L_);
    lua_pushinteger(L_, userid); lua_setfield(L_, -2, "userid");
    lua_pushboolean(L_, team_only); lua_setfield(L_, -2, "teamonly");
    lua_pushstring(L_, text ? text : ""); lua_setfield(L_, -2, "text");
    lua_pushstring(L_, username ? username : ""); lua_setfield(L_, -2, "name");
    lua_pushstring(L_, username ? username : "");
    lua_pushcclosure(L_, [](lua_State* state) -> int { lua_pushvalue(state, lua_upvalueindex(1)); return 1; }, 1);
    lua_setfield(L_, -2, "get_username");
    const auto previous = current_script_; current_script_ = callback.script; set_active_script(callback.script);
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) report_error("events.player_chat", &callback.script);
    else lua_pop(L_, 1);
    current_script_ = previous; set_active_script(previous);
  }
}

void LuaEngine::set_event_callback(const std::string& name, int function_index) {
  std::lock_guard lock(mutex_);
  const auto owner = active_script_owner();
  auto& callbacks = event_callbacks_[name];
  for (auto it = callbacks.begin(); it != callbacks.end();) {
    if (it->script == owner) {
      luaL_unref(L_, LUA_REGISTRYINDEX, it->function_ref);
      it = callbacks.erase(it);
    } else ++it;
  }
  lua_pushvalue(L_, function_index);
  callbacks.push_back({owner, luaL_ref(L_, LUA_REGISTRYINDEX)});
}

void LuaEngine::unset_event_callback(const std::string& name) {
  std::lock_guard lock(mutex_);
  const auto owner = active_script_owner();
  const auto found = event_callbacks_.find(name);
  if (found == event_callbacks_.end()) return;
  auto& callbacks = found->second;
  for (auto it = callbacks.begin(); it != callbacks.end();) {
    if (it->script == owner) { luaL_unref(L_, LUA_REGISTRYINDEX, it->function_ref); it = callbacks.erase(it); }
    else ++it;
  }
  if (callbacks.empty()) event_callbacks_.erase(found);
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
  const auto owner = self->active_script_owner();
  const auto callback = std::find_if(it->second.begin(), it->second.end(),
      [&](const EventCallback& value) { return value.script == owner; });
  if (callback == it->second.end()) return 0;
  const int top = lua_gettop(L);
  const int first_arg = lua_istable(L, 1) ? 2 : 1;
  lua_rawgeti(L, LUA_REGISTRYINDEX, callback->function_ref);
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

int LuaEngine::lua_files_read(lua_State* L) {
  size_t path_length = 0;
  const char* raw_path = luaL_checklstring(L, 1, &path_length);
  const auto path = std::filesystem::u8path(
      std::string(raw_path, path_length));
  std::ifstream input(path, std::ios::binary);
  if (!input) { lua_pushnil(L); return 1; }
  const std::string data((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  lua_pushlstring(L, data.data(), data.size());
  return 1;
}

int LuaEngine::lua_files_write(lua_State* L) {
  size_t path_length = 0;
  size_t data_length = 0;
  const char* raw_path = luaL_checklstring(L, 1, &path_length);
  const char* data = luaL_checklstring(L, 2, &data_length);
  const auto path = std::filesystem::u8path(
      std::string(raw_path, path_length));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (output) output.write(data, static_cast<std::streamsize>(data_length));
  lua_pushboolean(L, output.good());
  return 1;
}

int LuaEngine::lua_files_append(lua_State* L) {
  size_t path_length = 0;
  size_t data_length = 0;
  const char* raw_path = luaL_checklstring(L, 1, &path_length);
  const char* data = luaL_checklstring(L, 2, &data_length);
  const auto path = std::filesystem::u8path(
      std::string(raw_path, path_length));
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (output) output.write(data, static_cast<std::streamsize>(data_length));
  lua_pushboolean(L, output.good());
  return 1;
}

int LuaEngine::lua_files_exists(lua_State* L) {
  size_t path_length = 0;
  const char* raw_path = luaL_checklstring(L, 1, &path_length);
  std::error_code ec;
  const bool exists = std::filesystem::exists(
      std::filesystem::u8path(std::string(raw_path, path_length)), ec);
  lua_pushboolean(L, exists && !ec);
  return 1;
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
  lua_pushcfunction(L_, lua_files_read); lua_setfield(L_, -2, "read");
  lua_pushcfunction(L_, lua_files_write); lua_setfield(L_, -2, "write");
  lua_pushcfunction(L_, lua_files_append); lua_setfield(L_, -2, "append");
  lua_pushcfunction(L_, lua_files_exists); lua_setfield(L_, -2, "exists");
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
  const auto owner=self->active_script_owner();
  const auto headers=LuaHeaders(L,2); lua_pushvalue(L,callback); const int ref=luaL_ref(L,LUA_REGISTRYINDEX);
  self->network_workers_.emplace_back([self,url,headers,ref,owner] {
    std::string response; DWORD status=0; const bool ok=HttpRequest("GET",url,"",headers,response,status);
    if(!ok) response="network.get failed: "+std::to_string(GetLastError());
    std::lock_guard lock(self->network_mutex_); self->network_results_.push_back({ref,owner,std::move(response),status,ok});
  });
  lua_pushboolean(L,1); return 1;
}
int LuaEngine::lua_network_post(lua_State* L) {
  const std::string url=luaL_checkstring(L,1); std::string body;
  if(lua_isstring(L,2)) body=lua_tostring(L,2); else if(lua_istable(L,2)) { lua_getglobal(L,"json"); lua_getfield(L,-1,"encode"); lua_pushvalue(L,2); if(lua_pcall(L,1,1,0)!=LUA_OK)return lua_error(L); body=lua_tostring(L,-1); lua_pop(L,2); }
  int callback=lua_isfunction(L,2)?2:(lua_isfunction(L,3)?3:(lua_isfunction(L,4)?4:0));
  if (!callback) return luaL_error(L,"network.post requires callback(response, status)");
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1)));
  const auto owner=self->active_script_owner();
  auto headers=LuaHeaders(L,3); if(headers.empty()) headers=L"Content-Type: application/json\r\n";
  lua_pushvalue(L,callback); const int ref=luaL_ref(L,LUA_REGISTRYINDEX);
  self->network_workers_.emplace_back([self,url,body=std::move(body),headers=std::move(headers),ref,owner] {
    std::string response; DWORD status=0; const bool ok=HttpRequest("POST",url,body,headers,response,status);
    if(!ok) response="network.post failed: "+std::to_string(GetLastError());
    std::lock_guard lock(self->network_mutex_); self->network_results_.push_back({ref,owner,std::move(response),status,ok});
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
  std::lock_guard lock(network_mutex_);
  if (L_) for (const auto& result : network_results_)
    luaL_unref(L_, LUA_REGISTRYINDEX, result.callback_ref);
  network_results_.clear();
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

bool LuaEngine::script_autoload_enabled(const std::filesystem::path& path) const {
  std::lock_guard lock(mutex_);
  const auto target = std::filesystem::absolute(path).lexically_normal();
  return std::find(autoload_scripts_.begin(), autoload_scripts_.end(), target) !=
         autoload_scripts_.end();
}

void LuaEngine::set_script_autoload(const std::filesystem::path& path, bool enabled) {
  std::lock_guard lock(mutex_);
  const auto target = std::filesystem::absolute(path).lexically_normal();
  const auto found = std::find(autoload_scripts_.begin(), autoload_scripts_.end(), target);
  if (enabled && found == autoload_scripts_.end()) autoload_scripts_.push_back(target);
  if (!enabled && found != autoload_scripts_.end()) autoload_scripts_.erase(found);
  save_autoload_settings();
}

namespace {
LuaEngine::UiItem* FindUiItem(std::vector<LuaEngine::UiItem>& items, int id) {
  for (auto& item : items) if (item.id == id) return &item;
  return nullptr;
}

bool RenderHotkeyItem(LuaEngine::UiItem& item) {
  bool changed=false;
  const float height=ImGui::GetFrameHeight()+4.0f;
  const float width=std::max(120.0f,ImGui::GetContentRegionAvail().x);
  const ImVec2 position=ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##hotkey",{width,height},
      ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
  const bool hovered=ImGui::IsItemHovered();
  const bool left_clicked=ImGui::IsItemClicked(ImGuiMouseButton_Left);
  const bool right_clicked=ImGui::IsItemClicked(ImGuiMouseButton_Right);

  if (left_clicked) {
    item.hotkey_capturing=true;
    item.hotkey_capture_wait_release=true;
    item.hotkey_active=false;
  }
  if (right_clicked) {
    item.hotkey_capturing=false;
    ImGui::OpenPopup("##hotkey_mode");
  }

  if (item.hotkey_capturing) {
    if (item.hotkey_capture_wait_release) {
      if (!AnyBindableKeyDown()) item.hotkey_capture_wait_release=false;
    } else if (IsVirtualKeyDown(VK_ESCAPE)) {
      item.hotkey_capturing=false;
    } else {
      for (int key=1;key<256;++key) {
        if (!IsVirtualKeyDown(key)) continue;
        item.index_value=key;
        item.hotkey_active=false;
        item.hotkey_previous_down=true; // binding press must not trigger toggle
        item.hotkey_capturing=false;
        item.hotkey_capture_wait_release=false;
        changed=true;
        break;
      }
    }
  }

  ImDrawList* draw=ImGui::GetWindowDrawList();
  const ImU32 background=ImGui::GetColorU32(
      hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
  draw->AddRectFilled(position,{position.x+width,position.y+height},
                      background,ImGui::GetStyle().FrameRounding);
  if (item.hotkey_active)
    draw->AddRectFilled(position,{position.x+3.0f,position.y+height},
                        ImGui::GetColorU32(ImGuiCol_CheckMark),
                        ImGui::GetStyle().FrameRounding);

  const ImVec2 text_size=ImGui::CalcTextSize(item.name.c_str());
  const float text_y=position.y+(height-text_size.y)*0.5f;
  const ImU32 label_color=ImGui::GetColorU32(
      item.hotkey_active ? ImGuiCol_CheckMark : ImGuiCol_Text);
  draw->AddText({position.x+10.0f,text_y},label_color,item.name.c_str());

  const std::string value=item.hotkey_capturing
      ? "按下任意按键..."
      : "["+VirtualKeyName(item.index_value)+"]";
  const ImVec2 value_size=ImGui::CalcTextSize(value.c_str());
  const ImU32 value_color=ImGui::GetColorU32(
      item.hotkey_capturing ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
  draw->AddText({position.x+width-value_size.x-10.0f,text_y},
                value_color,value.c_str());

  if (hovered)
    ImGui::SetTooltip("左键：设置按键\n右键：选择触发模式");

  if (ImGui::BeginPopup("##hotkey_mode")) {
    ImGui::TextDisabled("触发模式");
    ImGui::Separator();
    if (ImGui::Selectable("长按触发",item.hotkey_mode==1)) {
      item.hotkey_mode=1;
      item.hotkey_active=IsVirtualKeyDown(item.index_value);
      item.hotkey_previous_down=item.hotkey_active;
      changed=true;
    }
    if (ImGui::Selectable("按下按键切换",item.hotkey_mode==2)) {
      item.hotkey_mode=2;
      item.hotkey_active=false;
      item.hotkey_previous_down=IsVirtualKeyDown(item.index_value);
      changed=true;
    }
    ImGui::EndPopup();
  }
  return changed;
}
}

int LuaEngine::lua_ui_create_item(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1)));
  UiItem item{}; item.id=self->next_ui_id_++; item.script=self->active_script_owner();
  item.group=luaL_checkstring(L,1); item.type=luaL_checkstring(L,2); item.name=luaL_checkstring(L,3);
  if (item.type=="switch") item.bool_value=item.initial_bool=lua_toboolean(L,4)!=0;
  else if (item.type=="slider") {
    item.number_value=item.initial_number=luaL_optnumber(L,4,0); item.min=luaL_optnumber(L,5,0);
    item.max=luaL_optnumber(L,6,100); item.scale=luaL_optnumber(L,7,1);
  } else if (item.type=="input") item.text=luaL_optstring(L,4,"");
  else if (item.type=="hotkey") {
    item.index_value=item.initial_index=std::clamp(
        static_cast<int>(luaL_optinteger(L,4,0)),0,255);
    item.hotkey_previous_down=IsVirtualKeyDown(item.index_value);
  }
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
  if(item->type=="hotkey"){lua_pushboolean(L,item->hotkey_active);return 1;} lua_pushnil(L);return 1;
}
int LuaEngine::lua_ui_set(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1))); auto* item=FindUiItem(self->ui_items_,luaL_checkinteger(L,1)); if(!item)return 0;
  if(item->type=="switch")item->bool_value=lua_toboolean(L,2)!=0; else if(item->type=="slider")item->number_value=luaL_checknumber(L,2);
  else if(item->type=="input")item->text=luaL_checkstring(L,2);
  else if(item->type=="selectable"||item->type=="listable"){
    std::fill(item->selected.begin(),item->selected.end(),false);
    auto select=[&](int arg){int index=-1;if(lua_isnumber(L,arg))index=static_cast<int>(lua_tointeger(L,arg))-1;else if(const char* name=lua_tostring(L,arg)){for(int i=0;i<(int)item->options.size();++i)if(item->options[i]==name){index=i;break;}}if(index>=0&&index<(int)item->selected.size())item->selected[index]=true;};
    if(lua_istable(L,2)){const int count=static_cast<int>(lua_objlen(L,2));for(int i=1;i<=count;++i){lua_rawgeti(L,2,i);select(lua_gettop(L));lua_pop(L,1);}}else for(int arg=2;arg<=lua_gettop(L);++arg)select(arg);
  } else if(item->type=="hotkey") {
    item->index_value=std::clamp(static_cast<int>(luaL_checkinteger(L,2)),0,255);
    item->hotkey_active=false;
    item->hotkey_previous_down=IsVirtualKeyDown(item->index_value);
  } else item->index_value=static_cast<int>(luaL_checkinteger(L,2))-1; return 0;
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
int LuaEngine::lua_ui_hotkey_info(lua_State* L) {
  auto* self=static_cast<LuaEngine*>(lua_touserdata(L,lua_upvalueindex(1)));
  auto* item=FindUiItem(self->ui_items_,luaL_checkinteger(L,1));
  if(!item||item->type!="hotkey"){lua_pushnil(L);return 1;}
  lua_pushinteger(L,item->index_value);
  lua_pushinteger(L,item->hotkey_mode);
  lua_pushboolean(L,item->hotkey_active);
  return 3;
}
void LuaEngine::register_ui_native_api() {
  lua_newtable(L_); auto add=[&](const char* name,lua_CFunction fn){lua_pushlightuserdata(L_,this);lua_pushcclosure(L_,fn,1);lua_setfield(L_,-2,name);};
  add("_create_item",lua_ui_create_item);add("_get",lua_ui_get);add("_set",lua_ui_set);add("_set_callback",lua_ui_set_callback);add("_state",lua_ui_set_state);add("_hotkey_info",lua_ui_hotkey_info);lua_setglobal(L_,"ui_native");
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
      std::string preview;for(int i=0;i<(int)item.options.size();++i)if(item.selected[i]){if(!preview.empty())preview+=", ";preview+=item.options[i];}if(preview.empty())preview="无";
      if(ImGui::BeginCombo(item.name.c_str(),preview.c_str())){for(int i=0;i<(int)item.options.size();++i){bool selected=item.selected[i];if(ImGui::Selectable(item.options[i].c_str(),selected,ImGuiSelectableFlags_DontClosePopups)){item.selected[i]=!selected;changed=true;}}ImGui::EndCombo();}
    }
    else if(item.type=="combo"||item.type=="list"){const char* preview=item.index_value>=0&&item.index_value<(int)item.options.size()?item.options[item.index_value].c_str():"";if(ImGui::BeginCombo(item.name.c_str(),preview)){for(int i=0;i<(int)item.options.size();++i){bool selected=i==item.index_value;if(ImGui::Selectable(item.options[i].c_str(),selected)){item.index_value=i;changed=true;}if(selected)ImGui::SetItemDefaultFocus();}ImGui::EndCombo();}}
    else if(item.type=="button")clicked=ImGui::Button(item.name.c_str());
    else if(item.type=="input"){char buffer[512]{};strncpy_s(buffer,item.text.c_str(),_TRUNCATE);if(ImGui::InputText(item.name.c_str(),buffer,sizeof(buffer))){item.text=buffer;changed=true;}}
    else if(item.type=="hotkey")changed=RenderHotkeyItem(item);
    else if(item.type=="label")ImGui::TextUnformatted(item.name.c_str());
    if(item.disabled)ImGui::EndDisabled(); const int callback=item.callback_ref; ImGui::PopID();
    if((changed||clicked)&&callback!=LUA_NOREF){const auto previous=current_script_;current_script_=target;set_active_script(target);lua_rawgeti(L_,LUA_REGISTRYINDEX,callback);if(lua_pcall(L_,0,0,0)!=LUA_OK)report_error("ui callback",&target);current_script_=previous;set_active_script(previous);}
  }
}

