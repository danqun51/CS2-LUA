#include <windows.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <tchar.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "hexsync_client.hpp"
#include "driver_service.hpp"
#include "injector_resources.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

// 注入流程按 D:\Documents\CS2 LUA\CS2-P2C-TEMPLATES-main\source\tools\CS2UnifiedInjector
// 的 loadlib 分支重写：FindProcessByName -> OpenTargetProcess -> VirtualAllocEx
// -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryW)。这里只保留本地学习用的
// LoadLibraryW 方法，没有接入 template 中的 kernel/manualmap/APC 等高级分支。

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_mainWindow = nullptr;
static ImFont* g_uiFont = nullptr;
static const auto g_startTime = std::chrono::steady_clock::now();
static float g_copyNoticeUntil = 0.f;

static std::vector<std::string> g_logs;
constexpr wchar_t kTargetProcess[] = L"cs2.exe";
constexpr wchar_t kDriverService[] = L"HexSyncService";

void ApplyNeverloseStyle() {
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 0.f; s.ChildRounding = 10.f; s.FrameRounding = 7.f;
    s.PopupRounding = 8.f; s.ScrollbarRounding = 8.f; s.GrabRounding = 6.f;
    s.WindowPadding = ImVec2(0, 0); s.FramePadding = ImVec2(12, 8); s.ItemSpacing = ImVec2(10, 9);
    s.WindowBorderSize = 0.f; s.ChildBorderSize = 0.f; s.FrameBorderSize = 0.f;
    s.ScrollbarSize = 8.f;
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.027f,0.029f,0.043f,1.f);
    c[ImGuiCol_ChildBg] = ImVec4(0.048f,0.051f,0.073f,1.f);
    c[ImGuiCol_Border] = ImVec4(0.20f,0.17f,0.30f,0.85f);
    c[ImGuiCol_FrameBg] = ImVec4(0.080f,0.082f,0.115f,1.f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.105f,0.105f,0.150f,1.f);
    c[ImGuiCol_Button] = ImVec4(0.39f,0.16f,0.91f,1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.52f,0.24f,1.f,1.f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.31f,0.10f,0.78f,1.f);
    c[ImGuiCol_Header] = c[ImGuiCol_Button]; c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_CheckMark] = ImVec4(0.67f,0.39f,1.f,1.f);
    c[ImGuiCol_Text] = ImVec4(0.91f,0.91f,0.96f,1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.49f,0.50f,0.61f,1.f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.27f,0.22f,0.40f,1.f);
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
        g_uiFont = io.Fonts->AddFontFromFileTTF(
                font, 17.0f, &config, io.Fonts->GetGlyphRangesChineseFull());
        if (g_uiFont) {
            io.FontDefault = g_uiFont;
            return;
        }
    }
    g_uiFont = io.Fonts->AddFontDefault();
}

void Log(const char* fmt, ...) {
    char buf[2048]{};
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    g_logs.emplace_back(buf);
}

bool ExtractResource(WORD id, const std::filesystem::path& output) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) { Log("[-] 查找内嵌资源 %u 失败：%lu", id, GetLastError()); return false; }
    HGLOBAL loaded = LoadResource(nullptr, resource);
    const DWORD size = SizeofResource(nullptr, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size) { Log("[-] 内嵌资源 %u 为空", id); return false; }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log("[-] 创建内嵌载荷文件失败：%lu", GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes, size, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != size) {
        Log("[-] 写入内嵌载荷文件失败：%lu", GetLastError());
        DeleteFileW(output.c_str());
        return false;
    }
    return true;
}

struct PayloadPaths {
    std::filesystem::path directory;
    std::filesystem::path dll;
    std::filesystem::path driver;
};

bool PreparePayloads(bool with_driver, PayloadPaths& paths) {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) { Log("[-] 获取临时目录失败：%lu", GetLastError()); return false; }
    paths.directory = std::filesystem::path(temp) /
        (L"CS2LuaInjector-" + std::to_wstring(GetCurrentProcessId()));
    paths.dll = paths.directory / L"CS2LuaPlugin.dll";
    paths.driver = paths.directory / L"CS2HexSyncCompatDriver.sys";
    if (!ExtractResource(IDR_CS2LUA_PLUGIN, paths.dll)) return false;
    if (with_driver && !ExtractResource(IDR_CS2LUA_DRIVER, paths.driver)) return false;
    return true;
}

void CleanupPayloads(const PayloadPaths& paths, bool with_driver) {
    if (with_driver) DeleteFileW(paths.driver.c_str());
    // A loaded DLL remains mapped by cs2.exe. Ask Windows to remove the
    // temporary extraction on reboot if it cannot be deleted immediately.
    if (!DeleteFileW(paths.dll.c_str())) MoveFileExW(paths.dll.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    std::error_code ec;
    std::filesystem::remove(paths.directory, ec);
}

DWORD FindProcessByName(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool IsModuleLoaded(DWORD pid, const wchar_t* module_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W module{sizeof(module)};
    bool found = false;
    if (Module32FirstW(snap, &module)) {
        do {
            if (_wcsicmp(module.szModule, module_name) == 0) { found = true; break; }
        } while (Module32NextW(snap, &module));
    }
    CloseHandle(snap);
    return found;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) { CloseHandle(token); return false; }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const bool ok = GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

HANDLE OpenTargetProcess(DWORD pid) {
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                            PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hp) Log("[-] 打开进程（PID=%lu）失败：%lu", pid, GetLastError());
    return hp;
}

bool WriteRemoteBytes(HANDLE hp, void* addr, const void* data, size_t size) {
    SIZE_T written = 0;
    if (!WriteProcessMemory(hp, addr, data, size, &written) || written != size) {
        Log("[-] 写入目标进程内存（%p，%zu 字节）失败：%lu", addr, size, GetLastError());
        return false;
    }
    return true;
}

bool InjectLoadLibrary(HANDLE hp, const std::wstring& dllPath) {
    Log("[*] 注入方式：CreateRemoteThread + LoadLibraryW");
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    auto pLoadLibW = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hK32, "LoadLibraryW"));
    if (!pLoadLibW) { Log("[-] 获取 LoadLibraryW 地址失败"); return false; }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hp, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { Log("[-] 分配目标进程内存失败：%lu", GetLastError()); return false; }
    if (!WriteRemoteBytes(hp, remote, dllPath.c_str(), bytes)) {
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(hp, nullptr, 0, pLoadLibW, remote, 0, nullptr);
    if (!thread) {
        Log("[-] 创建远程线程失败：%lu", GetLastError());
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        return false;
    }

    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);

    if (wait != WAIT_OBJECT_0) { Log("[-] 等待远程线程超时"); return false; }
    if (!exitCode) { Log("[-] LoadLibraryW 返回空地址"); return false; }
    Log("[+] 模块已加载，地址：0x%llX", static_cast<unsigned long long>(exitCode));
    return true;
}

bool NormalInject() {
    PayloadPaths payloads;
    if (!PreparePayloads(false, payloads)) return false;
    EnableDebugPrivilege();
    DWORD pid = FindProcessByName(kTargetProcess);
    if (!pid) { Log("[-] 未找到 cs2.exe 进程"); CleanupPayloads(payloads, false); return false; }
    Log("[*] 目标进程 cs2.exe，PID=%lu", pid);

    HANDLE hp = OpenTargetProcess(pid);
    if (!hp) { CleanupPayloads(payloads, false); return false; }
    bool ok = InjectLoadLibrary(hp, payloads.dll.wstring());
    CloseHandle(hp);
    CleanupPayloads(payloads, false);
    Log(ok ? "[+] 注入完成" : "[-] 注入失败");
    return ok;
}
bool DriverInject() {
    PayloadPaths payloads;
    if (!PreparePayloads(true, payloads)) return false;
    DWORD pid = FindProcessByName(kTargetProcess);
    if (!pid) { Log("[-] 未找到 cs2.exe 进程"); CleanupPayloads(payloads, true); return false; }

    DriverServiceManager sm;
    // Remove any stale registration from an interrupted previous run before
    // installing the embedded driver image.
    sm.stop_and_delete(kDriverService);
    Log("[*] 正在安装并启动内嵌驱动");
    if (!sm.install_and_start(kDriverService, payloads.driver.c_str())) {
        Log("[-] 驱动启动失败：%s", sm.last_error().c_str());
        sm.stop_and_delete(kDriverService);
        CleanupPayloads(payloads, true);
        return false;
    }

    // Give the symbolic link a short window to appear.
    bool online = false;
    HexSyncClient hx;
    for (int i = 0; i < 20; ++i) {
        if (hx.self_test()) { online = true; break; }
        Sleep(100);
    }

    bool injected = false;
    if (!online) {
        Log("[-] 驱动启动后 HexSync 自检失败：%s", hx.last_error().c_str());
    } else {
        Log("[+] HexSync 已连接，magic=0x%llX", hx.magic());
        Log("[*] 驱动正在注入 PID=%lu", pid);
        injected = hx.inject_loadlibrary(pid, payloads.dll.wstring());
        if (injected) {
            // The driver creates the remote thread asynchronously. Keep the
            // extracted DLL alive until LoadLibrary has actually mapped it.
            bool loaded = false;
            for (int i = 0; i < 50; ++i) {
                if (IsModuleLoaded(pid, L"CS2LuaPlugin.dll")) { loaded = true; break; }
                Sleep(100);
            }
            if (!loaded) {
                injected = false;
                Log("[-] 远程线程已返回，但 CS2LuaPlugin.dll 未成功加载");
            }
        }
        Log(injected ? "[+] 驱动注入完成" : "[-] 驱动注入失败：%s", hx.last_error().c_str());
    }

    Log("[*] 正在停止并删除驱动服务");
    if (!sm.stop_and_delete(kDriverService)) {
        Log("[-] 驱动清理失败：%s", sm.last_error().c_str());
    } else {
        Log("[+] 驱动服务已清理");
    }
    CleanupPayloads(payloads, true);
    return injected;
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

float AnimationSeconds() {
    return std::chrono::duration<float>(
        std::chrono::steady_clock::now() - g_startTime).count();
}

float SmoothStep(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

void DrawCenteredText(ImDrawList* draw, const char* text, float size,
                      float center_x, float y, ImU32 color) {
    ImFont* font = g_uiFont ? g_uiFont : ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    draw->AddText(font, size, {center_x - extent.x * 0.5f, y}, color, text);
}

void DrawWindowControls(ImDrawList* draw, const ImVec2& origin, float width) {
    const ImVec2 minimize_pos{origin.x + width - 82.f, origin.y + 9.f};
    ImGui::SetCursorScreenPos(minimize_pos);
    ImGui::InvisibleButton("##minimize", {32.f, 26.f});
    const bool minimize_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) ShowWindow(g_mainWindow, SW_MINIMIZE);
    if (minimize_hovered)
        draw->AddRectFilled(minimize_pos, {minimize_pos.x+32, minimize_pos.y+26},
                            IM_COL32(255,255,255,13), 6.f);
    draw->AddLine({minimize_pos.x+11, minimize_pos.y+14},
                  {minimize_pos.x+21, minimize_pos.y+14},
                  IM_COL32(174,174,194,255), 1.5f);

    const ImVec2 close_pos{origin.x + width - 43.f, origin.y + 9.f};
    ImGui::SetCursorScreenPos(close_pos);
    ImGui::InvisibleButton("##close", {32.f, 26.f});
    const bool close_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    if (close_hovered)
        draw->AddRectFilled(close_pos, {close_pos.x+32, close_pos.y+26},
                            IM_COL32(224,72,103,72), 6.f);
    const ImU32 close_color = close_hovered ? IM_COL32(255,160,177,255)
                                             : IM_COL32(174,174,194,255);
    draw->AddLine({close_pos.x+11, close_pos.y+9}, {close_pos.x+21, close_pos.y+19}, close_color, 1.5f);
    draw->AddLine({close_pos.x+21, close_pos.y+9}, {close_pos.x+11, close_pos.y+19}, close_color, 1.5f);
}

void DrawSplash(ImDrawList* draw, const ImVec2& origin, const ImVec2& size, float elapsed) {
    const float fade = 1.0f - SmoothStep((elapsed - 1.30f) / 0.40f);
    const int alpha = static_cast<int>(255.f * fade);
    for (int i = 0; i < 24; ++i) {
        const float x = std::fmod(i * 97.0f + elapsed * (9.0f + i % 5), size.x + 50.f) - 25.f;
        const float y = 56.f + std::fmod(i * 53.0f, std::max(1.0f, size.y - 100.f));
        const float pulse = 0.45f + 0.35f * std::sin(elapsed * 2.2f + i);
        draw->AddCircleFilled({origin.x+x, origin.y+y}, 1.2f + pulse,
                              IM_COL32(139,80,255,static_cast<int>(alpha * 0.22f)));
    }

    const ImVec2 center{origin.x + size.x * 0.5f, origin.y + size.y * 0.45f};
    const float pulse = 1.f + std::sin(elapsed * 4.f) * 0.025f;
    draw->AddCircleFilled(center, 45.f * pulse, IM_COL32(105,43,238,alpha / 7));
    draw->AddCircleFilled(center, 31.f * pulse, IM_COL32(102,45,224,alpha));
    draw->AddCircleFilled(center, 22.f * pulse, IM_COL32(36,29,61,alpha));
    const float angle = elapsed * 2.3f;
    draw->PathArcTo(center, 39.f, angle, angle + 4.45f, 52);
    draw->PathStroke(IM_COL32(178,111,255,alpha), 0, 2.5f);
    draw->PathArcTo(center, 49.f, -angle * 0.65f, -angle * 0.65f + 3.5f, 52);
    draw->PathStroke(IM_COL32(99,61,215,alpha / 2), 0, 1.5f);
    DrawCenteredText(draw, "NL", 20.f, center.x, center.y - 11.f,
                     IM_COL32(238,231,255,alpha));
    DrawCenteredText(draw, "CS2 LUA", 25.f, center.x, center.y + 67.f,
                     IM_COL32(240,238,248,alpha));
    DrawCenteredText(draw, "正在初始化注入环境", 14.f, center.x, center.y + 101.f,
                     IM_COL32(132,132,153,alpha));

    const float progress = SmoothStep(elapsed / 1.55f);
    const float bar_width = 210.f;
    const ImVec2 bar{center.x - bar_width * 0.5f, center.y + 137.f};
    draw->AddRectFilled(bar, {bar.x+bar_width, bar.y+3}, IM_COL32(55,51,72,alpha), 2.f);
    draw->AddRectFilled(bar, {bar.x+bar_width*progress, bar.y+3},
                        IM_COL32(151,79,255,alpha), 2.f);
}

bool TargetRunningCached() {
    static bool running = false;
    static auto checked = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (checked.time_since_epoch().count() == 0 || now - checked > std::chrono::seconds(1)) {
        running = FindProcessByName(kTargetProcess) != 0;
        checked = now;
    }
    return running;
}

void RenderUi() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    ImGui::Begin("###CS2LuaInjector", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const ImVec2 origin = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(origin, {origin.x+display.x, origin.y+display.y},
        IM_COL32(9,9,15,255), IM_COL32(15,12,25,255),
        IM_COL32(10,10,17,255), IM_COL32(7,8,13,255));
    draw->AddRectFilledMultiColor(origin, {origin.x+display.x, origin.y+42},
        IM_COL32(22,20,31,255), IM_COL32(29,22,43,255),
        IM_COL32(17,17,25,255), IM_COL32(17,17,25,255));
    draw->AddLine({origin.x,origin.y+42}, {origin.x+display.x,origin.y+42},
                  IM_COL32(105,65,163,70));
    draw->AddCircleFilled({origin.x+19,origin.y+21}, 7.f, IM_COL32(133,67,246,255));
    draw->AddCircleFilled({origin.x+19,origin.y+21}, 3.f, IM_COL32(229,207,255,255));
    draw->AddText(g_uiFont, 15.f, {origin.x+34,origin.y+12},
                  IM_COL32(225,222,235,255), "CS2 LUA");
    draw->AddText(g_uiFont, 12.f, {origin.x+99,origin.y+14},
                  IM_COL32(105,102,122,255), "注入器");
    DrawWindowControls(draw, origin, display.x);

    const float elapsed = AnimationSeconds();
    if (elapsed < 1.70f) {
        DrawSplash(draw, origin, display, elapsed);
        ImGui::End();
        return;
    }

    const float enter = SmoothStep((elapsed - 1.70f) / 0.38f);
    const float slide = (1.f - enter) * 14.f;
    const float sidebar_width = 205.f;
    draw->AddRectFilled({origin.x,origin.y+43},
                        {origin.x+sidebar_width,origin.y+display.y},
                        IM_COL32(14,14,22,245));
    draw->AddLine({origin.x+sidebar_width,origin.y+43},
                  {origin.x+sidebar_width,origin.y+display.y},
                  IM_COL32(94,70,130,58));

    draw->AddText(g_uiFont, 11.f, {origin.x+20,origin.y+70+slide},
                  IM_COL32(98,97,116,255), "控制中心");
    const ImVec2 nav_min{origin.x+12,origin.y+94+slide};
    draw->AddRectFilled(nav_min, {origin.x+193,nav_min.y+42},
                        IM_COL32(99,43,210,92), 8.f);
    draw->AddRectFilled(nav_min, {nav_min.x+3,nav_min.y+42},
                        IM_COL32(166,91,255,255), 2.f);
    draw->AddCircleFilled({nav_min.x+22,nav_min.y+21}, 5.f, IM_COL32(183,120,255,255));
    draw->AddText(g_uiFont, 15.f, {nav_min.x+38,nav_min.y+11},
                  IM_COL32(235,229,247,255), "注入中心");

    draw->AddText(g_uiFont, 11.f, {origin.x+20,origin.y+155+slide},
                  IM_COL32(98,97,116,255), "项目与社区");
    const ImVec2 github_min{origin.x+15,origin.y+176+slide};
    ImGui::SetCursorScreenPos(github_min);
    ImGui::InvisibleButton("##github_project", {175.f, 42.f});
    const bool github_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        ShellExecuteW(nullptr, L"open", L"https://github.com/danqun51/CS2-LUA",
                      nullptr, nullptr, SW_SHOWNORMAL);
    draw->AddRectFilled(github_min, {github_min.x+175,github_min.y+42},
                        github_hovered ? IM_COL32(45,35,70,255) : IM_COL32(23,22,34,255), 8.f);
    draw->AddRect(github_min, {github_min.x+175,github_min.y+42},
                  github_hovered ? IM_COL32(146,87,235,160) : IM_COL32(57,53,76,170), 8.f);
    draw->AddCircleFilled({github_min.x+17,github_min.y+21}, 8.f, IM_COL32(112,60,213,255));
    draw->AddText(g_uiFont, 11.f, {github_min.x+13.5f,github_min.y+12},
                  IM_COL32(238,228,255,255), "G");
    draw->AddText(g_uiFont, 13.f, {github_min.x+31,github_min.y+7},
                  IM_COL32(214,210,229,255), "danqun51 / CS2-LUA");
    draw->AddText(g_uiFont, 9.5f, {github_min.x+31,github_min.y+25},
                  IM_COL32(113,102,145,255), "github.com/danqun51/CS2-LUA");

    const ImVec2 qq_min{origin.x+15,origin.y+227+slide};
    draw->AddRectFilled(qq_min, {qq_min.x+175,qq_min.y+92}, IM_COL32(23,22,34,255), 8.f);
    draw->AddRect(qq_min, {qq_min.x+175,qq_min.y+92}, IM_COL32(57,53,76,170), 8.f);
    draw->AddText(g_uiFont, 11.f, {qq_min.x+12,qq_min.y+9},
                  IM_COL32(210,206,224,255), "CS2 LUA 交流群 / BUG提交群");
    draw->AddText(g_uiFont, 13.f, {qq_min.x+12,qq_min.y+31},
                  IM_COL32(170,108,255,255), "QQ群：1063275679");
    const ImVec2 copy_min{qq_min.x+10,qq_min.y+58};
    ImGui::SetCursorScreenPos(copy_min);
    ImGui::InvisibleButton("##copy_qq", {72.f, 25.f});
    const bool copy_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText("1063275679");
        g_copyNoticeUntil = AnimationSeconds() + 1.6f;
    }
    draw->AddRectFilled(copy_min, {copy_min.x+72,copy_min.y+25},
                        copy_hovered ? IM_COL32(79,49,127,255) : IM_COL32(48,40,68,255), 6.f);
    draw->AddText(g_uiFont, 11.f, {copy_min.x+13,copy_min.y+5},
                  IM_COL32(220,211,236,255),
                  AnimationSeconds() < g_copyNoticeUntil ? "已复制" : "复制群号");
    const ImVec2 join_min{qq_min.x+91,qq_min.y+58};
    ImGui::SetCursorScreenPos(join_min);
    ImGui::InvisibleButton("##join_qq", {74.f, 25.f});
    const bool join_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        ShellExecuteW(nullptr, L"open", L"https://qm.qq.com/q/QuzGioOKMS",
                      nullptr, nullptr, SW_SHOWNORMAL);
    draw->AddRectFilled(join_min, {join_min.x+74,join_min.y+25},
                        join_hovered ? IM_COL32(119,62,233,255) : IM_COL32(91,45,191,255), 6.f);
    draw->AddText(g_uiFont, 11.f, {join_min.x+15,join_min.y+5},
                  IM_COL32(239,232,250,255), "加入群聊");

    const bool running = TargetRunningCached();
    const ImVec2 status_min{origin.x+15,origin.y+display.y-105};
    draw->AddRectFilled(status_min, {origin.x+190,status_min.y+67},
                        IM_COL32(24,23,35,255), 9.f);
    draw->AddText(g_uiFont, 12.f, {status_min.x+13,status_min.y+11},
                  IM_COL32(108,106,127,255), "目标状态");
    draw->AddCircleFilled({status_min.x+17,status_min.y+43}, 4.f,
                          running ? IM_COL32(71,220,145,255) : IM_COL32(237,91,112,255));
    draw->AddText(g_uiFont, 14.f, {status_min.x+29,status_min.y+33},
                  IM_COL32(213,211,225,255), running ? "CS2 正在运行" : "等待 CS2 启动");
    draw->AddText(g_uiFont, 11.f, {origin.x+20,origin.y+display.y-24},
                  IM_COL32(72,72,88,255), "CS2LuaPlugin  ·  正式版");

    const float content_x = origin.x + 229.f;
    const float content_right = origin.x + display.x - 20.f;
    draw->AddText(g_uiFont, 23.f, {content_x,origin.y+65+slide},
                  IM_COL32(239,237,247,255), "选择注入方式");
    draw->AddText(g_uiFont, 13.f, {content_x,origin.y+96+slide},
                  IM_COL32(111,109,130,255), "载荷已内嵌，选择适合当前环境的方式开始注入");

    const float gap = 14.f;
    const float card_y = origin.y + 126.f + slide;
    const float card_width = (content_right - content_x - gap) * 0.5f;
    const float card_height = 126.f;
    const ImVec2 driver_min{content_x,card_y};
    const ImVec2 normal_min{content_x+card_width+gap,card_y};
    auto draw_card = [&](const ImVec2& min, const char* title, const char* subtitle,
                         const char* icon, bool primary) {
        draw->AddRectFilled(min, {min.x+card_width,min.y+card_height},
                            IM_COL32(20,20,31,255), 11.f);
        draw->AddRect(min, {min.x+card_width,min.y+card_height},
                      primary ? IM_COL32(130,72,218,145) : IM_COL32(65,62,83,190),
                      11.f, 0, 1.f);
        draw->AddRectFilled({min.x+14,min.y+14}, {min.x+48,min.y+48},
                            primary ? IM_COL32(105,46,224,255) : IM_COL32(48,46,66,255), 9.f);
        DrawCenteredText(draw, icon, 15.f, min.x+31, min.y+21,
                         IM_COL32(236,226,255,255));
        draw->AddText(g_uiFont, 16.f, {min.x+59,min.y+14}, IM_COL32(230,227,239,255), title);
        draw->AddText(g_uiFont, 12.f, {min.x+59,min.y+37}, IM_COL32(106,104,124,255), subtitle);
    };
    draw_card(driver_min, "驱动注入", "兼容性更强 · 推荐", "驱", true);
    draw_card(normal_min, "普通注入", "标准加载 · 快速", "普", false);

    ImGui::SetCursorScreenPos({driver_min.x+14,driver_min.y+72});
    if (ImGui::Button("开始驱动注入##driver", {card_width-28,38})) DriverInject();
    ImGui::SetCursorScreenPos({normal_min.x+14,normal_min.y+72});
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(48,46,66,255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70,64,96,255));
    if (ImGui::Button("开始普通注入##normal", {card_width-28,38})) NormalInject();
    ImGui::PopStyleColor(2);

    const float log_y = origin.y + 270.f + slide;
    const ImVec2 log_min{content_x,log_y};
    const ImVec2 log_max{content_right,origin.y+display.y-18.f};
    draw->AddRectFilled(log_min, log_max, IM_COL32(17,17,27,255), 11.f);
    draw->AddRect(log_min, log_max, IM_COL32(58,55,76,180), 11.f, 0, 1.f);
    draw->AddCircleFilled({log_min.x+17,log_min.y+20}, 3.5f, IM_COL32(154,84,255,255));
    draw->AddText(g_uiFont, 14.f, {log_min.x+29,log_min.y+10},
                  IM_COL32(206,203,218,255), "运行日志");
    draw->AddText(g_uiFont, 11.f, {log_max.x-72,log_min.y+12},
                  IM_COL32(78,77,94,255), "实时输出");
    ImGui::SetCursorScreenPos({log_min.x+12,log_min.y+39});
    ImGui::BeginChild("运行日志##log", {log_max.x-log_min.x-24,log_max.y-log_min.y-49},
                      false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : g_logs) {
        if (line.rfind("[+]", 0) == 0) ImGui::TextColored({0.35f,0.87f,0.61f,1.f}, "%s", line.c_str());
        else if (line.rfind("[-]", 0) == 0) ImGui::TextColored({0.95f,0.39f,0.48f,1.f}, "%s", line.c_str());
        else ImGui::TextColored({0.62f,0.61f,0.71f,1.f}, "%s", line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hWnd, &point);
        RECT client{};
        GetClientRect(hWnd, &client);
        if (point.y >= 0 && point.y < 42 && point.x >= 0 && point.x < client.right - 92)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        if (g_pd3dDevice != nullptr) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"CS2LuaInjectorWindow", nullptr };
    RegisterClassExW(&wc);
    constexpr int window_width = 780;
    constexpr int window_height = 520;
    const int window_x = (GetSystemMetrics(SM_CXSCREEN) - window_width) / 2;
    const int window_y = (GetSystemMetrics(SM_CYSCREEN) - window_height) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
                                L"CS2 Lua 一键注入器",
                                WS_POPUP | WS_MINIMIZEBOX,
                                window_x, window_y, window_width, window_height,
                                nullptr, nullptr, wc.hInstance, nullptr);
    g_mainWindow = hwnd;
    const DWORD corner_preference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(33),
                          &corner_preference, sizeof(corner_preference));
    const BOOL dark_mode = TRUE;
    DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(20),
                          &dark_mode, sizeof(dark_mode));
    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ConfigureChineseFont();
    ImGui::StyleColorsDark();
    ApplyNeverloseStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    Log("[*] 注入器已就绪");
    Log("[*] 内嵌目标：cs2.exe");

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderUi();
        ImGui::Render();

        const float clear_color[4] = { 0.027f, 0.029f, 0.043f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    g_mainWindow = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}


