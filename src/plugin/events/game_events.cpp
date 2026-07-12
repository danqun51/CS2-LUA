#include "game_events.hpp"
#include "lua/lua_engine.hpp"
#include <windows.h>
#include <MinHook.h>
#include <string>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace {
uint32_t MurmurHash2Lower(const char* text) {
  std::string lower = text ? text : "";
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto* data = reinterpret_cast<const unsigned char*>(lower.data());
  uint32_t len = static_cast<uint32_t>(lower.size()), h = 0x31415926u ^ len;
  constexpr uint32_t m=0x5bd1e995u; constexpr int r=24;
  while (len >= 4) { uint32_t k; memcpy(&k,data,4); k*=m; k^=k>>r; k*=m; h*=m; h^=k; data+=4; len-=4; }
  switch(len) { case 3:h^=data[2]<<16; [[fallthrough]]; case 2:h^=data[1]<<8; [[fallthrough]]; case 1:h^=data[0];h*=m; }
  h^=h>>13; h*=m; h^=h>>15; return h;
}
struct GameEventKeySymbol {
  uint32_t hash{};
  int32_t symbol_id{-1};
  const char* text{};
  explicit GameEventKeySymbol(const char* value) : hash(MurmurHash2Lower(value)), text(value) {}
};
}

class IGameEvent {
 public:
  virtual ~IGameEvent() = default;
  virtual const char* GetName() const = 0;
  virtual int GetID() const = 0;
  virtual bool IsReliable() const = 0;
  virtual bool IsLocal() const = 0;
  virtual bool IsEmpty(const GameEventKeySymbol&) = 0;
  virtual bool GetBool(const GameEventKeySymbol&, bool defaultValue = false) = 0;
  virtual int GetInt(const GameEventKeySymbol&, int defaultValue = 0) = 0;
  virtual unsigned long long GetUint64(const GameEventKeySymbol&, unsigned long long defaultValue = 0) = 0;
  virtual float GetFloat(const GameEventKeySymbol&, float defaultValue = 0.0f) = 0;
  virtual const char* GetString(const GameEventKeySymbol&, const char* defaultValue = "") = 0;
};

class IGameEventListener2 {
 public:
  virtual ~IGameEventListener2() = default;
  virtual void FireGameEvent(IGameEvent* event) = 0;
  virtual int GetEventDebugID() { return 42; }
};

class IGameEventManager2 {
 public:
  virtual ~IGameEventManager2() = default;
  virtual int LoadEventsFromFile(const char*) = 0;
  virtual void Reset() = 0;
  virtual bool AddListener(IGameEventListener2*, const char*, bool) = 0;
  virtual bool FindListener(IGameEventListener2*, const char*) = 0;
  virtual void RemoveListener(IGameEventListener2*) = 0;
};

namespace {
using ConsoleMsgFn = void(__cdecl*)(const char*, ...);
using FireEventFn = bool(__fastcall*)(IGameEventManager2*, IGameEvent*, bool);
using FireEventClientFn = bool(__fastcall*)(IGameEventManager2*, IGameEvent*);
using ManagerInitFn = void(__fastcall*)(IGameEventManager2*);
GameEventBridge* g_bridge{};
FireEventFn g_original_fire{};
FireEventClientFn g_original_fire_client{};
void* g_fire_target{};
void* g_fire_client_target{};
ManagerInitFn g_original_manager_init{};
void* g_manager_init_target{};
using ConMsgFn = void(__cdecl*)(const char*, ...);
ConMsgFn g_original_conmsg{};
void* g_conmsg_target{};
ConMsgFn g_original_msg{};
void* g_msg_target{};
thread_local bool g_inside_conmsg{};
void EventLog(const char* text) {
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  auto msg = tier0 ? reinterpret_cast<ConsoleMsgFn>(GetProcAddress(tier0, "Msg")) : nullptr;
  if (!msg && tier0) msg = reinterpret_cast<ConsoleMsgFn>(GetProcAddress(tier0, "ConMsg"));
  if (msg) msg("[CS2Lua events] %s\n", text);
  OutputDebugStringA(text);
}

void __cdecl HookConMsg(const char* format, ...) {
  char rendered[4096]{};
  va_list args; va_start(args, format);
  vsnprintf_s(rendered, sizeof(rendered), _TRUNCATE, format ? format : "", args);
  va_end(args);
  if (g_original_conmsg) g_original_conmsg("%s", rendered);
  if (!g_inside_conmsg && g_bridge) {
    g_inside_conmsg = true; g_bridge->on_console_line(rendered); g_inside_conmsg = false;
  }
}
void __cdecl HookMsg(const char* format, ...) {
  char rendered[4096]{};
  va_list args; va_start(args, format);
  vsnprintf_s(rendered, sizeof(rendered), _TRUNCATE, format ? format : "", args);
  va_end(args);
  if (g_original_msg) g_original_msg("%s", rendered);
  if (!g_inside_conmsg && g_bridge) {
    g_inside_conmsg = true; g_bridge->on_console_line(rendered); g_inside_conmsg = false;
  }
}

bool __fastcall HookFireEvent(IGameEventManager2* self, IGameEvent* event, bool dont_broadcast) {
  if (g_bridge) g_bridge->on_game_event(event);
  return g_original_fire(self, event, dont_broadcast);
}
bool __fastcall HookFireEventClient(IGameEventManager2* self, IGameEvent* event) {
  if (g_bridge) g_bridge->on_game_event(event);
  return g_original_fire_client(self, event);
}
void __fastcall HookManagerInit(IGameEventManager2* manager) {
  g_original_manager_init(manager);
  if (g_bridge) g_bridge->attach_legacy_manager(manager);
}

void* PatternScan(HMODULE module, const int* pattern, size_t count) {
  if (!module) return nullptr;
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const size_t size = nt->OptionalHeader.SizeOfImage;
  for (size_t i=0; i+count<=size; ++i) {
    bool ok=true; for (size_t j=0;j<count;++j) if (pattern[j]>=0 && base[i+j]!=pattern[j]) {ok=false;break;}
    if (ok) return base+i;
  }
  return nullptr;
}
}

class GameEventBridge::Listener final : public IGameEventListener2 {
 public:
  explicit Listener(GameEventBridge& owner) : owner_(owner) {}
  void FireGameEvent(IGameEvent* event) override { owner_.on_game_event(event); }
 private:
  GameEventBridge& owner_;
};

bool GameEventBridge::initialize(LuaEngine& lua) {
  lua_ = &lua;
  g_bridge = this;
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  // tier0 exports ConMsg with its MSVC decorated name on current CS2 builds.
  g_conmsg_target = tier0 ? reinterpret_cast<void*>(GetProcAddress(tier0, "?ConMsg@@YAXPEBDZZ")) : nullptr;
  if (g_conmsg_target) {
    MH_Initialize();
    if (MH_CreateHook(g_conmsg_target, HookConMsg, reinterpret_cast<void**>(&g_original_conmsg)) == MH_OK &&
        (MH_EnableHook(g_conmsg_target) == MH_OK || MH_EnableHook(g_conmsg_target) == MH_ERROR_ENABLED))
      EventLog("ConMsg chat fallback hook installed");
    else { g_conmsg_target=nullptr; EventLog("ConMsg chat fallback hook failed"); }
  } else EventLog("tier0 decorated ConMsg export not found");
  g_msg_target = tier0 ? reinterpret_cast<void*>(GetProcAddress(tier0, "Msg")) : nullptr;
  if (g_msg_target) {
    if (MH_CreateHook(g_msg_target, HookMsg, reinterpret_cast<void**>(&g_original_msg)) == MH_OK &&
        (MH_EnableHook(g_msg_target) == MH_OK || MH_EnableHook(g_msg_target) == MH_ERROR_ENABLED))
      EventLog("tier0!Msg chat fallback hook installed");
    else { g_msg_target=nullptr; EventLog("tier0!Msg hook failed"); }
  }
  using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
  HMODULE engine = GetModuleHandleW(L"engine2.dll");
  auto create = engine ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
  manager_ = create ? static_cast<IGameEventManager2*>(create("GAMEEVENTSMANAGER002", nullptr)) : nullptr;
  if (!manager_) { EventLog("GAMEEVENTSMANAGER002 factory unavailable; waiting for CGameEventManager_Init"); poll(); return true; }
  attach_legacy_manager(manager_);
  return true;
}

void GameEventBridge::attach_legacy_manager(IGameEventManager2* manager) {
  if (!manager || (manager_ == manager && listener_)) return;
  manager_ = manager;
  // Listener dispatch is sufficient once the real manager has been recovered.
  // Avoid detouring FireEvent: its ABI changed in CS2 and a mismatched detour
  // can corrupt registers when the first event is fired.
  if (listener_) { delete listener_; listener_=nullptr; }
  listener_ = new Listener(*this);
  // The recovered manager belongs to server.dll, so this must be registered
  // as a server-side listener. Registering with false succeeds but never
  // receives the locally hosted server's player_hurt dispatch.
  if (!manager_->AddListener(listener_, "player_hurt", true)) {
    delete listener_; listener_ = nullptr; manager_ = nullptr;
    EventLog("AddListener(player_hurt) failed"); return;
  }
  EventLog(manager_->FindListener(listener_, "player_hurt")
      ? "player_hurt server-side listener registered and verified"
      : "player_hurt AddListener returned success but FindListener failed");
  const bool chat_server = manager_->AddListener(listener_, "player_chat", true);
  const bool chat_client = manager_->AddListener(listener_, "player_chat", false);
  const bool say_server = manager_->AddListener(listener_, "player_say", true);
  const bool say_client = manager_->AddListener(listener_, "player_say", false);
  char chat_status[160]{};
  std::snprintf(chat_status, sizeof(chat_status),
      "chat listeners: player_chat server=%d client=%d, player_say server=%d client=%d",
      chat_server, chat_client, say_server, say_client);
  EventLog(chat_status);
}

void GameEventBridge::poll() {
  std::vector<std::string> chat_lines;
  {
    std::lock_guard lock(chat_mutex_);
    chat_lines.swap(pending_chat_lines_);
  }
  for (const auto& line : chat_lines) dispatch_console_line(line);
  if (manager_ || g_manager_init_target) return;
  HMODULE server = GetModuleHandleW(L"server.dll");
  if (!server) return;
  static const int sig[] = {0x40,0x53,0x48,0x83,0xEC,-1,0x48,0x8B,0x01,0x48,0x8B,0xD9,0xFF,0x50,-1,0x48,0x8B,0x03};
  g_manager_init_target = PatternScan(server, sig, sizeof(sig)/sizeof(sig[0]));
  if (!g_manager_init_target) { EventLog("CGameEventManager_Init signature not found"); return; }
  // Recover the already-created legacy manager from a caller shaped as:
  //   mov rcx, qword ptr [rip + global_manager]
  //   call CGameEventManager_Init
  // This works even when the DLL is injected after server initialization.
  auto* base = reinterpret_cast<unsigned char*>(server);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  for (size_t i=7; i+5<=image_size; ++i) {
    if (base[i] != 0xE8 || base[i-7] != 0x48 || base[i-6] != 0x8B || base[i-5] != 0x0D) continue;
    const auto call_disp = *reinterpret_cast<int32_t*>(base+i+1);
    if (base+i+5+call_disp != g_manager_init_target) continue;
    const auto global_disp = *reinterpret_cast<int32_t*>(base+i-4);
    auto** global_slot = reinterpret_cast<IGameEventManager2**>(base+i+global_disp);
    // RIP after mov is base+i, because the mov starts at i-7 and is 7 bytes.
    if (global_slot && *global_slot) {
      EventLog("recovered existing IGameEventManager2 from server global");
      g_manager_init_target = nullptr;
      attach_legacy_manager(*global_slot);
      return;
    }
  }
  MH_Initialize();
  if (MH_CreateHook(g_manager_init_target, HookManagerInit, reinterpret_cast<void**>(&g_original_manager_init)) == MH_OK &&
      (MH_EnableHook(g_manager_init_target) == MH_OK || MH_EnableHook(g_manager_init_target) == MH_ERROR_ENABLED))
    EventLog("CGameEventManager_Init hook installed; start/restart local map to capture manager");
  else { EventLog("CGameEventManager_Init hook failed"); g_manager_init_target=nullptr; }
}

void GameEventBridge::shutdown() {
  if (manager_ && listener_) manager_->RemoveListener(listener_);
  if (g_fire_target) { MH_DisableHook(g_fire_target); MH_RemoveHook(g_fire_target); g_fire_target = nullptr; }
  if (g_fire_client_target) { MH_DisableHook(g_fire_client_target); MH_RemoveHook(g_fire_client_target); g_fire_client_target = nullptr; }
  if (g_manager_init_target) { MH_DisableHook(g_manager_init_target); MH_RemoveHook(g_manager_init_target); g_manager_init_target=nullptr; }
  if (g_conmsg_target) { MH_DisableHook(g_conmsg_target); MH_RemoveHook(g_conmsg_target); g_conmsg_target=nullptr; }
  if (g_msg_target) { MH_DisableHook(g_msg_target); MH_RemoveHook(g_msg_target); g_msg_target=nullptr; }
  g_bridge = nullptr;
  delete listener_; listener_ = nullptr; manager_ = nullptr; lua_ = nullptr;
}

void GameEventBridge::on_console_line(const char* line) {
  if (!line) return;
  if (!std::strstr(line, "[ALL]") && !std::strstr(line, "[TEAM]")) return;
  // Never invoke Lua or InputService while inside tier0's variadic logging
  // function. Only copy the completed line; the plugin loop dispatches it.
  std::lock_guard lock(chat_mutex_);
  pending_chat_lines_.emplace_back(line);
}

void GameEventBridge::dispatch_console_line(const std::string& value) {
  if (!lua_) return;
  const bool all = value.find("[ALL]") != std::string::npos;
  const bool team = value.find("[TEAM]") != std::string::npos;
  if (!all && !team) return;
  const auto close = value.find(']');
  if (close == std::string::npos) return;
  std::string payload = value.substr(close + 1);
  while (!payload.empty() && (payload.front()==' ' || payload.front()=='\t')) payload.erase(payload.begin());
  size_t colon = payload.find(": "); size_t colon_size = 2;
  static constexpr const char* kFullwidthColon = "\xEF\xBC\x9A ";
  if (colon == std::string::npos) { colon = payload.find(kFullwidthColon); colon_size = 4; }
  if (colon == std::string::npos) return;
  std::string username = payload.substr(0, colon);
  std::string text = payload.substr(colon + colon_size);
  while (!text.empty() && (text.back()=='\r' || text.back()=='\n')) text.pop_back();
  lua_->fire_player_chat(0, team, text.c_str(), username.c_str());
}

void GameEventBridge::on_game_event(IGameEvent* event) {
  if (!lua_ || !event) return;
  const char* name = event->GetName();
  if (!name) return;
  if (lstrcmpiA(name, "player_chat") == 0 || lstrcmpiA(name, "player_say") == 0) {
    const GameEventKeySymbol userid("userid"), teamonly("teamonly"), text("text"), player_name("name");
    const int uid = event->GetInt(userid);
    const char* message = event->GetString(text);
    const char* username = event->GetString(player_name);
    EventLog("player chat event fired");
    lua_->fire_player_chat(uid, event->GetBool(teamonly), message, username && *username ? username : "player");
    return;
  }
  if (lstrcmpiA(name, "player_hurt") != 0) return;
  const GameEventKeySymbol userid("userid"), attacker("attacker"), health("health"), armor("armor"),
      dmg_health("dmg_health"), dmg_armor("dmg_armor"), hitgroup("hitgroup"), weapon("weapon");
  lua_->fire_player_hurt(event->GetInt(userid), event->GetInt(attacker),
    event->GetInt(health), event->GetInt(armor), event->GetInt(dmg_health),
    event->GetInt(dmg_armor), event->GetInt(hitgroup), event->GetString(weapon));
}
