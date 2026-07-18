#include "plugin.hpp"

#include <windows.h>
#include <MinHook.h>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using HandleScopeCtorFn = void(__fastcall*)(void*, void*);
HandleScopeCtorFn g_original_handle_scope_ctor{};
void* g_handle_scope_target{};
void* g_panorama_isolate{};
PluginRuntime* g_runtime{};
thread_local bool g_inside_lua_v8_frame{};
bool g_scripts_loaded{};
std::atomic_bool g_v8_shutdown_requested{};
std::atomic_bool g_v8_shutdown_complete{};
std::chrono::steady_clock::time_point g_last_lua_frame{};

void ProcessLuaOnV8Thread() {
  if (!g_runtime) return;

  if (g_v8_shutdown_requested.load()) {
    if (!g_v8_shutdown_complete.load()) {
      g_runtime->lua_engine().unload_all();
      g_v8_shutdown_complete.store(true);
    }
    return;
  }

  // The hook runs only after V8 itself has successfully constructed a
  // HandleScope while holding the isolate lock. Any nested Panorama calls made
  // by Lua therefore run on the exact thread/lock owner used by Panorama.
  if (!g_scripts_loaded) {
    g_scripts_loaded = true;
    g_runtime->lua_engine().load_autoload_scripts();
    g_last_lua_frame = std::chrono::steady_clock::now();
  }

  for (auto& action : g_runtime->menu_controller().consume_script_actions()) {
    switch (action.type) {
      case ScriptActionType::LoadAll: g_runtime->lua_engine().load_all(); break;
      case ScriptActionType::UnloadAll: g_runtime->lua_engine().unload_all(); break;
      case ScriptActionType::ReloadAll: g_runtime->lua_engine().reload_all(); break;
      case ScriptActionType::LoadScript: g_runtime->lua_engine().load_script(action.path); break;
      case ScriptActionType::UnloadScript: g_runtime->lua_engine().unload_script(action.path); break;
      case ScriptActionType::ReloadScript: g_runtime->lua_engine().reload_script(action.path); break;
    }
  }

  const auto now = std::chrono::steady_clock::now();
  const float dt = g_last_lua_frame.time_since_epoch().count()
      ? std::chrono::duration<float>(now - g_last_lua_frame).count()
      : 0.0f;
  // Avoid ticking many times inside the same Panorama frame: V8 constructs
  // numerous nested HandleScopes per frame.
  if (dt >= 0.005f) {
    g_last_lua_frame = now;
    g_runtime->lua_engine().tick(dt);
    g_runtime->lua_engine().fire_event("render");
  }
}

void __fastcall HookHandleScopeCtor(void* self, void* isolate) {
  g_original_handle_scope_ctor(self, isolate);
  if (isolate != g_panorama_isolate || g_inside_lua_v8_frame) return;
  g_inside_lua_v8_frame = true;
  ProcessLuaOnV8Thread();
  g_inside_lua_v8_frame = false;
}

bool InstallV8ThreadHook(PluginRuntime& runtime) {
  using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
  using GetUIEngineFn = void*(__fastcall*)(void*);
  using GetIsolateFn = void*(__fastcall*)(void*);

  HMODULE panorama = GetModuleHandleW(L"panorama.dll");
  HMODULE v8 = GetModuleHandleW(L"v8.dll");
  auto create = panorama
      ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(panorama, "CreateInterface"))
      : nullptr;
  void* panorama_interface = create ? create("PanoramaUIEngine001", nullptr) : nullptr;
  void** interface_vtable = panorama_interface
      ? *reinterpret_cast<void***>(panorama_interface) : nullptr;
  void* ui_engine = interface_vtable
      ? reinterpret_cast<GetUIEngineFn>(interface_vtable[13])(panorama_interface)
      : nullptr;
  void** ui_vtable = ui_engine ? *reinterpret_cast<void***>(ui_engine) : nullptr;
  g_panorama_isolate = ui_vtable
      ? reinterpret_cast<GetIsolateFn>(ui_vtable[92])(ui_engine) : nullptr;
  g_handle_scope_target = v8
      ? reinterpret_cast<void*>(GetProcAddress(
          v8, "??0HandleScope@v8@@QEAA@PEAVIsolate@1@@Z"))
      : nullptr;
  if (!g_panorama_isolate || !g_handle_scope_target) return false;

  g_runtime = &runtime;
  const auto init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
  if (MH_CreateHook(g_handle_scope_target, HookHandleScopeCtor,
                    reinterpret_cast<void**>(&g_original_handle_scope_ctor)) != MH_OK)
    return false;
  return MH_EnableHook(g_handle_scope_target) == MH_OK;
}

void RemoveV8ThreadHook() {
  if (g_handle_scope_target) {
    MH_DisableHook(g_handle_scope_target);
    MH_RemoveHook(g_handle_scope_target);
  }
  g_handle_scope_target = nullptr;
  g_original_handle_scope_ctor = nullptr;
  g_panorama_isolate = nullptr;
  g_runtime = nullptr;
}
}  // namespace

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

void PluginRuntime::request_stop() { stop_requested_.store(true); }

void PluginRuntime::run() {
  wchar_t exe_path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  const auto scripts_dir = std::filesystem::path(exe_path).parent_path() / L"lua";

  lua_.initialize();
  lua_.load_embedded_library("nl_compat.lua");
  lua_.load_embedded_table_library("offsets.lua", "offsets");
  lua_.load_embedded_table_library("entity_offsets.lua", "entity_offsets");
  lua_.load_embedded_library("entity_compat.lua");
  lua_.load_embedded_table_library("panorama_compat.lua", "panorama");
  lua_.set_scripts_dir(scripts_dir);
  // Script loading and Lua ticking are performed by the Present hook.  The
  // old worker-thread load/tick path entered Panorama's V8 isolate from a
  // foreign thread; V8's Context/HandleScope stacks are thread-local and that
  // eventually aborts with Escape-twice / non-entered-context assertions.
  menu_.initialize(lua_);
  if (!InstallV8ThreadHook(*this))
    std::cout << "[CS2LuaPlugin] failed to install Panorama V8 thread hook\n";
  game_events_.initialize(lua_);

  auto last = std::chrono::steady_clock::now();
  while (!stop_requested_.load()) {
    game_events_.poll();
    if (GetAsyncKeyState(VK_HOME) & 1) {
      menu_.toggle();
    }
    // F5/F6/F7 used to mutate Lua on this worker thread. Use the menu buttons,
    // which execute on Present, so Panorama/V8 always sees one owner thread.
    if (GetAsyncKeyState(VK_F8) & 1) {
      menu_.print_script_list(lua_);
    }
    if (GetAsyncKeyState(VK_END) & 1) {
      request_stop();
    }

    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  // V8-backed Lua objects must be released from Panorama's own thread while
  // its isolate lock and HandleScope are active. Ask the hook to perform the
  // shutdown before removing it instead of closing Lua on this worker thread.
  g_v8_shutdown_requested.store(true);
  for (int i = 0; i < 200 && !g_v8_shutdown_complete.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!g_v8_shutdown_complete.load()) {
    std::cout << "[CS2LuaPlugin] timed out waiting for V8-thread cleanup\n";
    // Never unload a DLL while V8 still owns callbacks into it. If Panorama is
    // temporarily not producing frames, keep the module resident and wait for
    // the next safe V8 entry rather than leaving dangling hook/callback code.
    while (!g_v8_shutdown_complete.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  game_events_.shutdown();
  RemoveV8ThreadHook();
  menu_.shutdown();
}
