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
#include <atomic>
#include <chrono>
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
IDXGISwapChain* g_active_chain{};
bool g_imgui_ready{};
std::chrono::steady_clock::time_point g_intro_start{};
bool g_intro_finished{};
std::atomic<float> g_mouse_scale_x{1.0f};
std::atomic<float> g_mouse_scale_y{1.0f};
UINT g_backbuffer_width{};
UINT g_backbuffer_height{};
LONG g_client_width{};
LONG g_client_height{};

std::string PathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void ConfigureChineseFont() {
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig config{};
  config.OversampleH = 2;
  config.OversampleV = 2;
  config.PixelSnapH = true;
  const char* fonts[] = {
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\simhei.ttf",
      "C:\\Windows\\Fonts\\simsun.ttc",
  };
  for (const char* font : fonts) {
    if (GetFileAttributesA(font) == INVALID_FILE_ATTRIBUTES) continue;
    if (io.Fonts->AddFontFromFileTTF(
            font, 17.0f, &config, io.Fonts->GetGlyphRangesChineseFull())) {
      return;
    }
  }
  io.Fonts->AddFontDefault();
}

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

float IntroSmoothStep(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value * value * (3.0f - 2.0f * value);
}

void DrawIntroText(ImDrawList* draw, const char* text, float size,
                   float center_x, float y, ImU32 color) {
  ImFont* font = ImGui::GetFont();
  const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
  draw->AddText(font, size, {center_x - extent.x * 0.5f, y}, color, text);
}

bool RenderIntroAnimation() {
  if (g_intro_finished || g_intro_start.time_since_epoch().count() == 0) return false;
  const float elapsed = std::chrono::duration<float>(
      std::chrono::steady_clock::now() - g_intro_start).count();
  constexpr float duration = 2.80f;
  if (elapsed >= duration) { g_intro_finished = true; return false; }

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float fade_in = IntroSmoothStep(elapsed / 0.28f);
  const float fade_out = 1.0f - IntroSmoothStep((elapsed - 2.24f) / 0.56f);
  const float opacity = std::clamp(fade_in * fade_out, 0.0f, 1.0f);
  const int alpha = static_cast<int>(255.f * opacity);
  ImGui::SetNextWindowPos({0,0}, ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
  ImGui::Begin("###CS2LuaInjectionIntro", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilledMultiColor({0,0}, display,
      IM_COL32(7,7,13,static_cast<int>(225*opacity)),
      IM_COL32(19,10,32,static_cast<int>(232*opacity)),
      IM_COL32(8,8,15,static_cast<int>(235*opacity)),
      IM_COL32(5,6,11,static_cast<int>(225*opacity)));

  for (int i=0;i<46;++i) {
    const float speed=12.f+static_cast<float>(i%7)*3.2f;
    const float x=std::fmod(i*137.f+elapsed*speed,display.x+80.f)-40.f;
    const float y=std::fmod(i*79.f+std::sin(elapsed*1.7f+i)*22.f,display.y+40.f)-20.f;
    const float glow=0.45f+0.35f*std::sin(elapsed*3.f+i*0.7f);
    draw->AddCircleFilled({x,y},1.0f+glow,
        IM_COL32(157,83,255,static_cast<int>(alpha*0.32f)));
  }

  const ImVec2 center{display.x*0.5f,display.y*0.5f-12.f};
  const ImVec2 card_min{center.x-225.f,center.y-137.f};
  const ImVec2 card_max{center.x+225.f,center.y+150.f};
  draw->AddRectFilled(card_min,card_max,IM_COL32(18,16,29,static_cast<int>(242*opacity)),18.f);
  draw->AddRect(card_min,card_max,IM_COL32(134,72,220,static_cast<int>(145*opacity)),18.f,0,1.25f);
  draw->AddRectFilledMultiColor({card_min.x,card_min.y},{card_max.x,card_min.y+3.f},
      IM_COL32(102,44,230,alpha),IM_COL32(193,103,255,alpha),
      IM_COL32(193,103,255,alpha),IM_COL32(102,44,230,alpha));

  const ImVec2 logo{center.x,center.y-54.f};
  const float rotation=elapsed*3.1f;
  const float pulse=1.f+std::sin(elapsed*5.f)*0.035f;
  draw->AddCircleFilled(logo,48.f*pulse,IM_COL32(116,48,238,static_cast<int>(36*opacity)));
  draw->AddCircleFilled(logo,31.f*pulse,IM_COL32(103,43,226,alpha));
  draw->AddCircleFilled(logo,21.f*pulse,IM_COL32(31,25,48,alpha));
  draw->PathArcTo(logo,40.f,rotation,rotation+4.65f,64);
  draw->PathStroke(IM_COL32(195,119,255,alpha),0,3.f);
  draw->PathArcTo(logo,52.f,-rotation*0.58f,-rotation*0.58f+3.7f,64);
  draw->PathStroke(IM_COL32(113,65,224,static_cast<int>(150*opacity)),0,1.5f);
  DrawIntroText(draw,"NL",20.f,logo.x,logo.y-11.f,IM_COL32(240,232,255,alpha));

  DrawIntroText(draw,"CS2 LUA 已成功注入",25.f,center.x,center.y+20.f,
                IM_COL32(241,238,249,alpha));
  DrawIntroText(draw,"正在连接 Panorama V8 并初始化脚本环境",13.f,center.x,center.y+57.f,
                IM_COL32(139,134,157,alpha));
  const float progress=IntroSmoothStep(elapsed/(duration-0.35f));
  const float bar_width=276.f;
  const ImVec2 bar{center.x-bar_width*0.5f,center.y+101.f};
  draw->AddRectFilled(bar,{bar.x+bar_width,bar.y+4.f},IM_COL32(50,45,65,alpha),3.f);
  draw->AddRectFilled(bar,{bar.x+bar_width*progress,bar.y+4.f},IM_COL32(159,82,255,alpha),3.f);
  const float comet_x=bar.x+bar_width*progress;
  draw->AddCircleFilled({comet_x,bar.y+2.f},5.f,IM_COL32(207,151,255,static_cast<int>(170*opacity)));
  DrawIntroText(draw,elapsed<1.6f?"加载兼容层与事件系统...":"准备打开 LUA 控制中心...",
                11.f,center.x,center.y+119.f,IM_COL32(105,101,122,alpha));
  ImGui::End();
  ImGui::PopStyleVar();
  return true;
}

LPARAM ScaleClientMousePosition(LPARAM position) {
  const auto client_x=static_cast<short>(LOWORD(position));
  const auto client_y=static_cast<short>(HIWORD(position));
  const auto render_x=static_cast<short>(std::lround(
      static_cast<float>(client_x)*g_mouse_scale_x.load(std::memory_order_relaxed)));
  const auto render_y=static_cast<short>(std::lround(
      static_cast<float>(client_y)*g_mouse_scale_y.load(std::memory_order_relaxed)));
  return MAKELPARAM(render_x,render_y);
}

LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  // ImGui is rendered in swap-chain pixels. In exclusive/fullscreen modes the
  // HWND client area can remain at the desktop resolution while CS2 changes
  // only the back buffer. Convert client mouse coordinates into that same
  // coordinate space before they enter ImGui's event queue.
  const LPARAM imgui_l=(msg==WM_MOUSEMOVE) ? ScaleClientMousePosition(l) : l;
  if (g_menu && g_menu->visible() &&
      ImGui_ImplWin32_WndProcHandler(hwnd,msg,w,imgui_l)) return TRUE;
  return g_original_wndproc
      ? CallWindowProcW(g_original_wndproc, hwnd, msg, w, l)
      : DefWindowProcW(hwnd,msg,w,l);
}

void RestoreWindowHook() {
  if (!g_hwnd || !g_original_wndproc) return;
  const auto current=reinterpret_cast<WNDPROC>(
      GetWindowLongPtrW(g_hwnd,GWLP_WNDPROC));
  if (current==HookWndProc)
    SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_original_wndproc));
}

void EnsureWindowHook() {
  if (!g_hwnd) return;
  const auto current=reinterpret_cast<WNDPROC>(
      GetWindowLongPtrW(g_hwnd,GWLP_WNDPROC));
  if (current==HookWndProc) return;
  // Source 2 may recreate/subclass the game window when changing fullscreen or
  // display modes. Chain our handler onto the new procedure so ImGui receives
  // mouse coordinates from the current HWND instead of the stale one.
  if (current) g_original_wndproc=current;
  SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(HookWndProc));
}

bool SynchronizeCoordinateSpace(IDXGISwapChain* chain) {
  if (!chain || !g_hwnd) return false;
  ID3D11Texture2D* backbuffer{};
  if (FAILED(chain->GetBuffer(0,__uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&backbuffer)))) return false;
  D3D11_TEXTURE2D_DESC texture{};
  backbuffer->GetDesc(&texture);
  backbuffer->Release();
  if (!texture.Width || !texture.Height) return false;

  RECT client{};
  if (!GetClientRect(g_hwnd,&client)) return false;
  const LONG client_width=client.right-client.left;
  const LONG client_height=client.bottom-client.top;
  if (client_width<=0 || client_height<=0) return false;

  const float scale_x=static_cast<float>(texture.Width)/
                      static_cast<float>(client_width);
  const float scale_y=static_cast<float>(texture.Height)/
                      static_cast<float>(client_height);
  g_mouse_scale_x.store(scale_x,std::memory_order_relaxed);
  g_mouse_scale_y.store(scale_y,std::memory_order_relaxed);

  ImGuiIO& io=ImGui::GetIO();
  // imgui_impl_dx11 uses DisplaySize as the D3D viewport size (not just as a
  // logical layout size), therefore it must be the real back-buffer size.
  io.DisplaySize={static_cast<float>(texture.Width),
                  static_cast<float>(texture.Height)};
  io.DisplayFramebufferScale={1.0f,1.0f};

  // imgui_impl_win32 may poll a raw client-space cursor position in NewFrame.
  // Queue the normalized value last so hovering/dragging stays exact even
  // when no WM_MOUSEMOVE is generated during a display-mode transition.
  if (GetForegroundWindow()==g_hwnd || GetCapture()==g_hwnd) {
    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(g_hwnd,&cursor))
      io.AddMousePosEvent(static_cast<float>(cursor.x)*scale_x,
                          static_cast<float>(cursor.y)*scale_y);
  }

  if (g_backbuffer_width!=texture.Width ||
      g_backbuffer_height!=texture.Height ||
      g_client_width!=client_width || g_client_height!=client_height) {
    g_backbuffer_width=texture.Width;
    g_backbuffer_height=texture.Height;
    g_client_width=client_width;
    g_client_height=client_height;
    std::ofstream out(std::filesystem::path([] {
      wchar_t path[MAX_PATH]{};
      GetModuleFileNameW(g_module,path,MAX_PATH);
      return std::filesystem::path(path).parent_path();
    }()) / L"CS2LuaPlugin-hook.log",std::ios::app);
    out << "coordinate space: backbuffer=" << texture.Width << 'x'
        << texture.Height << ", client=" << client_width << 'x'
        << client_height << ", mouse-scale=" << scale_x << 'x'
        << scale_y << '\n';
  }
  return true;
}

bool RenderDrawDataToSwapChain(IDXGISwapChain* chain) {
  if (!chain || !g_device || !g_context) return false;
  ID3D11Texture2D* back{};
  if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&back)))) return false;
  ID3D11RenderTargetView* target{};
  const HRESULT result=g_device->CreateRenderTargetView(back,nullptr,&target);
  back->Release();
  if (FAILED(result) || !target) return false;

  // Never retain or leave the game's back buffer bound. Video-quality changes
  // rebuild several render targets without always calling DXGI ResizeBuffers;
  // a persistent plugin RTV can keep the old buffer alive and freeze the last
  // presented frame. Acquire it only for this draw and restore the exact OM
  // bindings CS2 had on entry.
  ID3D11RenderTargetView* previous[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
  ID3D11DepthStencilView* previous_depth{};
  g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                previous,&previous_depth);
  g_context->OMSetRenderTargets(1,&target,nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                previous,previous_depth);
  for (auto*& view : previous) if (view) { view->Release(); view=nullptr; }
  if (previous_depth) previous_depth->Release();
  target->Release();
  return true;
}

bool RebindSwapChain(IDXGISwapChain* chain) {
  if (!chain) return false;
  ID3D11Device* next_device{};
  if (FAILED(chain->GetDevice(__uuidof(ID3D11Device),
                              reinterpret_cast<void**>(&next_device)))) return false;
  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(chain->GetDesc(&desc))) { next_device->Release(); return false; }
  if (!desc.OutputWindow) { next_device->Release(); return false; }
  const bool device_changed=next_device!=g_device;
  const bool window_changed=desc.OutputWindow!=g_hwnd;
  if (device_changed) {
    ImGui_ImplDX11_Shutdown();
    if (g_context) { g_context->Release(); g_context=nullptr; }
    if (g_device) { g_device->Release(); g_device=nullptr; }
    g_device=next_device;
    g_device->GetImmediateContext(&g_context);
    ImGui_ImplDX11_Init(g_device,g_context);
  } else {
    next_device->Release();
  }
  if (window_changed) {
    RestoreWindowHook();
    ImGui_ImplWin32_Shutdown();
    g_hwnd=desc.OutputWindow;
    ImGui_ImplWin32_Init(g_hwnd);
    g_original_wndproc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(HookWndProc)));
  }
  g_active_chain=chain;
  EnsureWindowHook();
  LogHook("swapchain rebound after video-mode change");
  return true;
}

bool EnsureSwapChainBinding(IDXGISwapChain* chain) {
  if (!chain) return false;
  if (chain!=g_active_chain) return RebindSwapChain(chain);
  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(chain->GetDesc(&desc))) return false;
  ID3D11Device* current_device{};
  const bool has_device=SUCCEEDED(chain->GetDevice(
      __uuidof(ID3D11Device),reinterpret_cast<void**>(&current_device)));
  const bool device_changed=has_device && current_device!=g_device;
  if (current_device) current_device->Release();
  if (desc.OutputWindow!=g_hwnd || device_changed) return RebindSwapChain(chain);
  EnsureWindowHook();
  return true;
}

bool InitRenderer(IDXGISwapChain* chain) {
  if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) { LogHook("Present reached, but swapchain is not D3D11 (possibly Vulkan/D3D12)"); return false; }
  g_device->GetImmediateContext(&g_context);
  DXGI_SWAP_CHAIN_DESC desc{}; chain->GetDesc(&desc); g_hwnd = desc.OutputWindow;
  if (!g_hwnd) { g_context->Release(); g_context=nullptr; g_device->Release(); g_device=nullptr; return false; }
  g_active_chain=chain;
  IMGUI_CHECKVERSION(); ImGui::CreateContext(); ConfigureChineseFont(); ApplyNeverloseStyle();
  g_intro_start=std::chrono::steady_clock::now(); g_intro_finished=false;
  ImGui_ImplWin32_Init(g_hwnd); ImGui_ImplDX11_Init(g_device, g_context);
  g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
  LogHook("D3D11 ImGui renderer initialized");
  return g_imgui_ready = true;
}

void RenderMenu() {
  static std::optional<std::filesystem::path> selected_script_ui;
  static ImVec2 last_display{};
  const ImVec2 display=ImGui::GetIO().DisplaySize;
  const bool display_changed=std::fabs(display.x-last_display.x)>1.f ||
                             std::fabs(display.y-last_display.y)>1.f;
  const ImVec2 desired_size{
      std::max(360.f,std::min(620.f,display.x-20.f)),
      std::max(280.f,std::min(430.f,display.y-20.f))};
  if (display_changed) {
    ImGui::SetNextWindowSize(desired_size,ImGuiCond_Always);
    ImGui::SetNextWindowPos({display.x*0.5f,display.y*0.5f},
                            ImGuiCond_Always,{0.5f,0.5f});
    last_display=display;
  } else {
    ImGui::SetNextWindowSize({620,430},ImGuiCond_FirstUseEver);
  }
  if (!ImGui::Begin("CS2 Lua｜脚本管理器###CS2LuaScripts", nullptr, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
  if (!display_changed) {
    const ImVec2 position=ImGui::GetWindowPos();
    const ImVec2 size=ImGui::GetWindowSize();
    const ImVec2 clamped{
        std::clamp(position.x,0.f,std::max(0.f,display.x-size.x)),
        std::clamp(position.y,0.f,std::max(0.f,display.y-size.y))};
    if (std::fabs(clamped.x-position.x)>0.5f ||
        std::fabs(clamped.y-position.y)>0.5f)
      ImGui::SetWindowPos(clamped,ImGuiCond_Always);
  }
  ImGui::TextColored({0.65f,0.38f,1.f,1.f}, "LUA 脚本管理"); ImGui::SameLine();
  ImGui::TextDisabled("Home 显示/隐藏　End 卸载插件"); ImGui::Separator();
  ImGui::TextDisabled("勾选“自动加载”的脚本将在下次启动插件时自动运行。");
  if (selected_script_ui) {
    ImGui::Spacing();
    if (ImGui::Button("< 返回脚本列表")) selected_script_ui.reset();
    if (selected_script_ui) {
      const std::string filename = PathToUtf8(selected_script_ui->filename());
      ImGui::SameLine(); ImGui::TextColored({0.65f,0.38f,1.f,1.f}, "%s", filename.c_str());
      ImGui::Separator(); ImGui::BeginChild("script_ui", {0,0}, true);
      g_lua->render_script_ui(*selected_script_ui);
      ImGui::EndChild(); ImGui::End(); return;
    }
  }
  ImGui::Spacing(); ImGui::Text("脚本列表");
  ImGui::BeginChild("scripts", {0, 0}, true);
  auto available = g_lua->available_scripts();
  auto loaded = g_lua->loaded_scripts();
  for (const auto& path : available) {
    const bool active = std::find(loaded.begin(), loaded.end(), path) != loaded.end();
    const std::string path_utf8 = PathToUtf8(path);
    const std::string filename_utf8 = PathToUtf8(path.filename());
    ImGui::PushID(path_utf8.c_str());
    ImGui::TextColored(active ? ImVec4(0.35f,0.95f,0.62f,1.f) : ImVec4(0.58f,0.61f,0.70f,1.f),
                       "%s", active ? "[已加载]" : "[未加载]");
    ImGui::SameLine();
    if (active && g_lua->script_has_ui(path)) {
      if (GearButton("##script_gear")) selected_script_ui=path;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("打开脚本设置");
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(filename_utf8.c_str());
    const float right = ImGui::GetContentRegionAvail().x - 305.f;
    if (right > 0) ImGui::SameLine(ImGui::GetCursorPosX() + right);
    bool autoload = g_lua->script_autoload_enabled(path);
    if (ImGui::Checkbox("自动加载", &autoload))
      g_lua->set_script_autoload(path, autoload);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("下次启动插件时自动加载此脚本");
    ImGui::SameLine();
    if (!active) {
      if (ImGui::SmallButton("加载")) g_menu->request_script_action(ScriptActionType::LoadScript, path);
    } else {
      if (ImGui::SmallButton("卸载")) g_menu->request_script_action(ScriptActionType::UnloadScript, path);
      ImGui::SameLine();
      if (ImGui::SmallButton("重新加载")) g_menu->request_script_action(ScriptActionType::ReloadScript, path);
    }
    ImGui::Separator();
    ImGui::PopID();
  }
  if (available.empty()) ImGui::TextDisabled("没有找到 .lua 脚本文件。");
  ImGui::EndChild(); ImGui::End();
}

HRESULT __stdcall HookPresent(IDXGISwapChain* chain, UINT sync, UINT flags) {
  const bool renderer_ready=g_imgui_ready
      ? EnsureSwapChainBinding(chain) : InitRenderer(chain);
  if (g_imgui_ready && renderer_ready) {
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
    SynchronizeCoordinateSpace(chain);
    ImGui::NewFrame();
    const bool intro_active=RenderIntroAnimation();
    if (!intro_active && g_menu && g_menu->visible()) RenderMenu();
    ImGui::Render();
    RenderDrawDataToSwapChain(chain);
  }
  return g_original_present(chain, sync, flags);
}

HRESULT __stdcall HookPresent1(IDXGISwapChain1* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
  auto* base_chain=static_cast<IDXGISwapChain*>(chain);
  const bool renderer_ready=g_imgui_ready
      ? EnsureSwapChainBinding(base_chain) : InitRenderer(base_chain);
  if (g_imgui_ready && renderer_ready) {
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
    SynchronizeCoordinateSpace(base_chain);
    ImGui::NewFrame();
    const bool intro_active=RenderIntroAnimation();
    if (!intro_active && g_menu && g_menu->visible()) RenderMenu();
    ImGui::Render();
    RenderDrawDataToSwapChain(base_chain);
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
  LogHook("using transient back-buffer rendering (video-mode safe)");
  if (sc1) sc1->Release();
  sc->Release(); ctx->Release(); dev->Release(); DestroyWindow(wnd); UnregisterClassW(wc.lpszClassName,wc.hInstance); initialized_=ok; return ok;
}

void MenuController::request_script_action(ScriptActionType type,
                                           std::filesystem::path path) {
  std::lock_guard lock(action_mutex_);
  pending_actions_.push_back({type, std::move(path)});
}

std::vector<ScriptAction> MenuController::consume_script_actions() {
  std::lock_guard lock(action_mutex_);
  std::vector<ScriptAction> result;
  result.swap(pending_actions_);
  return result;
}

void MenuController::shutdown() {
  if (g_present_target) { MH_DisableHook(g_present_target); MH_RemoveHook(g_present_target); g_present_target=nullptr; }
  if (g_present1_target) { MH_DisableHook(g_present1_target); MH_RemoveHook(g_present1_target); g_present1_target=nullptr; }
  MH_Uninitialize();
  RestoreWindowHook();
  if (g_imgui_ready) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
  if (g_context) g_context->Release(); if (g_device) g_device->Release();
  g_context=nullptr; g_device=nullptr; g_active_chain=nullptr;
  g_mouse_scale_x.store(1.0f,std::memory_order_relaxed);
  g_mouse_scale_y.store(1.0f,std::memory_order_relaxed);
  g_backbuffer_width=g_backbuffer_height=0;
  g_client_width=g_client_height=0;
  g_hwnd=nullptr; g_original_wndproc=nullptr;
  g_imgui_ready=false; g_menu=nullptr; g_lua=nullptr; initialized_=false;
}
void MenuController::toggle() { visible_ = !visible_.load(); }
bool MenuController::visible() const { return visible_.load(); }
void MenuController::print_script_list(LuaEngine& lua) const { (void)lua; }
