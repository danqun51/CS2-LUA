#include "menu.hpp"
#include "lua/lua_engine.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern HMODULE g_module;

namespace {
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn g_original_present{};
Present1Fn g_original_present1{};
void* g_present_target{};
void* g_present1_target{};
MenuController* g_menu{};
LuaEngine* g_lua{};
HWND g_hwnd{};
WNDPROC g_original_wndproc{};
ID3D11Device* g_device{};
ID3D11DeviceContext* g_context{};
ID3D11RenderTargetView* g_rtv{};
bool g_imgui_ready{};

void LogHook(const char* text) {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(g_module, path, MAX_PATH);
  std::filesystem::path log = std::filesystem::path(path).parent_path() / L"CS2LuaPlugin-hook.log";
  std::ofstream out(log, std::ios::app); out << text << '\n';
}

void ApplyNeverloseStyle() {
  auto& s = ImGui::GetStyle();
  s.WindowRounding = 7.f; s.ChildRounding = 5.f; s.FrameRounding = 4.f;
  s.PopupRounding = 5.f; s.ScrollbarRounding = 6.f; s.GrabRounding = 4.f;
  s.WindowPadding = {14, 14}; s.FramePadding = {10, 7}; s.ItemSpacing = {9, 8};
  auto* c = s.Colors;
  c[ImGuiCol_WindowBg] = {0.045f,0.050f,0.070f,0.98f};
  c[ImGuiCol_ChildBg] = {0.065f,0.072f,0.095f,1.f};
  c[ImGuiCol_Border] = {0.16f,0.19f,0.25f,1.f};
  c[ImGuiCol_FrameBg] = {0.085f,0.095f,0.125f,1.f};
  c[ImGuiCol_Button] = {0.35f,0.12f,0.88f,1.f};
  c[ImGuiCol_ButtonHovered] = {0.48f,0.20f,1.f,1.f};
  c[ImGuiCol_ButtonActive] = {0.28f,0.08f,0.72f,1.f};
  c[ImGuiCol_Header] = c[ImGuiCol_Button]; c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
  c[ImGuiCol_CheckMark] = {0.58f,0.28f,1.f,1.f};
  c[ImGuiCol_TitleBg] = c[ImGuiCol_WindowBg]; c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}

bool GearButton(const char* id) {
  const ImVec2 size{22.f,22.f}; const ImVec2 pos=ImGui::GetCursorScreenPos();
  const bool pressed=ImGui::InvisibleButton(id,size); const bool hovered=ImGui::IsItemHovered();
  auto* draw=ImGui::GetWindowDrawList(); const ImVec2 center{pos.x+11.f,pos.y+11.f};
  const ImU32 color=ImGui::GetColorU32(hovered?ImVec4(0.72f,0.48f,1.f,1.f):ImVec4(0.62f,0.65f,0.75f,1.f));
  draw->AddCircle(center,6.f,color,16,2.f); draw->AddCircleFilled(center,2.f,color,12);
  for(int i=0;i<8;++i){const float a=i*3.14159265f/4.f; const ImVec2 p1{center.x+7.f*cosf(a),center.y+7.f*sinf(a)}; const ImVec2 p2{center.x+9.f*cosf(a),center.y+9.f*sinf(a)};draw->AddLine(p1,p2,color,2.5f);}
  return pressed;
}

LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (g_menu && g_menu->visible() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l)) return TRUE;
  return CallWindowProcW(g_original_wndproc, hwnd, msg, w, l);
}

bool InitRenderer(IDXGISwapChain* chain) {
  if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) { LogHook("Present reached, but swapchain is not D3D11 (possibly Vulkan/D3D12)"); return false; }
  g_device->GetImmediateContext(&g_context);
  DXGI_SWAP_CHAIN_DESC desc{}; chain->GetDesc(&desc); g_hwnd = desc.OutputWindow;
  ID3D11Texture2D* back{};
  if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
  g_device->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release();
  IMGUI_CHECKVERSION(); ImGui::CreateContext(); ApplyNeverloseStyle();
  ImGui_ImplWin32_Init(g_hwnd); ImGui_ImplDX11_Init(g_device, g_context);
  g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
  LogHook("D3D11 ImGui renderer initialized");
  return g_imgui_ready = true;
}

void RenderMenu() {
  static std::optional<std::filesystem::path> selected_script_ui;
  ImGui::SetNextWindowSize({620, 430}, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("CS2 Lua  |  scripts", nullptr, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
  ImGui::TextColored({0.65f,0.38f,1.f,1.f}, "LUA MANAGER"); ImGui::SameLine();
  ImGui::TextDisabled("Home - show/hide   End - unload DLL"); ImGui::Separator();
  if (ImGui::Button("Load scripts", {135, 36})) g_lua->load_all(); ImGui::SameLine();
  if (ImGui::Button("Unload scripts", {135, 36})) g_lua->unload_all(); ImGui::SameLine();
  if (ImGui::Button("Reload scripts", {135, 36})) g_lua->reload_all();
  if (selected_script_ui) {
    ImGui::Spacing();
    if (ImGui::Button("< Back to scripts")) selected_script_ui.reset();
    if (selected_script_ui) {
      ImGui::SameLine(); ImGui::TextColored({0.65f,0.38f,1.f,1.f}, "%s", selected_script_ui->filename().string().c_str());
      ImGui::Separator(); ImGui::BeginChild("script_ui", {0,0}, true);
      g_lua->render_script_ui(*selected_script_ui);
      ImGui::EndChild(); ImGui::End(); return;
    }
  }
  ImGui::Spacing(); ImGui::Text("Script list");
  ImGui::BeginChild("scripts", {0, 0}, true);
  auto available = g_lua->available_scripts();
  auto loaded = g_lua->loaded_scripts();
  for (const auto& path : available) {
    const bool active = std::find(loaded.begin(), loaded.end(), path) != loaded.end();
    ImGui::PushID(path.string().c_str());
    ImGui::TextColored(active ? ImVec4(0.35f,0.95f,0.62f,1.f) : ImVec4(0.58f,0.61f,0.70f,1.f),
                       "%s", active ? "[loaded]" : "[idle]");
    ImGui::SameLine();
    if (active && g_lua->script_has_ui(path)) {
      if (GearButton("##script_gear")) selected_script_ui=path;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open script menu");
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(path.filename().string().c_str());
    const float right = ImGui::GetContentRegionAvail().x - 255.f;
    if (right > 0) ImGui::SameLine(ImGui::GetCursorPosX() + right);
    if (!active) {
      if (ImGui::SmallButton("Load")) g_lua->load_script(path);
    } else {
      if (ImGui::SmallButton("Unload")) g_lua->unload_script(path);
      ImGui::SameLine();
      if (ImGui::SmallButton("Reload")) g_lua->reload_script(path);
    }
    ImGui::Separator();
    ImGui::PopID();
  }
  if (available.empty()) ImGui::TextDisabled("No .lua files found.");
  ImGui::EndChild(); ImGui::End();
}

HRESULT __stdcall HookPresent(IDXGISwapChain* chain, UINT sync, UINT flags) {
  if (!g_imgui_ready) InitRenderer(chain);
  if (g_imgui_ready) {
    if (g_lua) g_lua->try_fire_event("render");
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    if (g_menu && g_menu->visible()) RenderMenu();
    ImGui::Render(); g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }
  return g_original_present(chain, sync, flags);
}

HRESULT __stdcall HookPresent1(IDXGISwapChain1* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
  if (!g_imgui_ready) InitRenderer(chain);
  if (g_imgui_ready) {
    if (g_lua) g_lua->try_fire_event("render");
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    if (g_menu && g_menu->visible()) RenderMenu();
    ImGui::Render(); g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }
  return g_original_present1(chain, sync, flags, params);
}

LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h,m,w,l); }
}

bool MenuController::initialize(LuaEngine& lua) {
  g_menu = this; g_lua = &lua; visible_ = true; LogHook("plugin initialized; installing DXGI hooks");
  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DummyProc, 0, 0, GetModuleHandleW(nullptr), nullptr,nullptr,nullptr,nullptr,L"CS2LuaDummy",nullptr};
  RegisterClassExW(&wc); HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0,0,100,100,nullptr,nullptr,wc.hInstance,nullptr);
  DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=1; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=wnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  ID3D11Device* dev{}; ID3D11DeviceContext* ctx{}; IDXGISwapChain* sc{}; D3D_FEATURE_LEVEL fl{};
  const auto hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&sc,&dev,&fl,&ctx);
  if (FAILED(hr)) { LogHook("dummy D3D11 swapchain creation failed"); DestroyWindow(wnd); UnregisterClassW(wc.lpszClassName,wc.hInstance); return false; }
  g_present_target=(*reinterpret_cast<void***>(sc))[8];
  IDXGISwapChain1* sc1{};
  if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) g_present1_target=(*reinterpret_cast<void***>(sc1))[22];
  MH_Initialize();
  const bool p0=MH_CreateHook(g_present_target, HookPresent, reinterpret_cast<void**>(&g_original_present))==MH_OK && MH_EnableHook(g_present_target)==MH_OK;
  bool p1=false;
  if (g_present1_target && g_present1_target != g_present_target)
    p1=MH_CreateHook(g_present1_target, HookPresent1, reinterpret_cast<void**>(&g_original_present1))==MH_OK && MH_EnableHook(g_present1_target)==MH_OK;
  const bool ok=p0 || p1; LogHook(ok ? "DXGI Present/Present1 hook installed" : "MinHook failed to install DXGI hook");
  if (sc1) sc1->Release();
  sc->Release(); ctx->Release(); dev->Release(); DestroyWindow(wnd); UnregisterClassW(wc.lpszClassName,wc.hInstance); initialized_=ok; return ok;
}

void MenuController::shutdown() {
  if (g_present_target) { MH_DisableHook(g_present_target); MH_RemoveHook(g_present_target); g_present_target=nullptr; }
  if (g_present1_target) { MH_DisableHook(g_present1_target); MH_RemoveHook(g_present1_target); g_present1_target=nullptr; }
  MH_Uninitialize();
  if (g_hwnd && g_original_wndproc) SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(g_original_wndproc));
  if (g_imgui_ready) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
  if (g_rtv) g_rtv->Release(); if (g_context) g_context->Release(); if (g_device) g_device->Release();
  g_imgui_ready=false; g_menu=nullptr; g_lua=nullptr; initialized_=false;
}
void MenuController::toggle() { visible_ = !visible_.load(); }
bool MenuController::visible() const { return visible_.load(); }
void MenuController::print_script_list(LuaEngine& lua) const { (void)lua; }
