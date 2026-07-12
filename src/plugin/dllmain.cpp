#include <windows.h>
#include <memory>
#include "plugin.hpp"

HMODULE g_module = nullptr;
static HANDLE g_thread = nullptr;
static std::unique_ptr<PluginRuntime> g_runtime;

static DWORD WINAPI plugin_thread(LPVOID) {
  g_runtime = std::make_unique<PluginRuntime>();
  g_runtime->run();
  g_runtime.reset();
  FreeLibraryAndExitThread(g_module, 0);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
    g_thread = CreateThread(nullptr, 0, plugin_thread, nullptr, 0, nullptr);
  } else if (reason == DLL_PROCESS_DETACH) {
    if (g_thread) CloseHandle(g_thread);
  }
  return TRUE;
}
