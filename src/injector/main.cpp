#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <tchar.h>

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

namespace {

// 注入流程按 D:\Documents\CS2 LUA\CS2-P2C-TEMPLATES-main\source\tools\CS2UnifiedInjector
// 的 loadlib 分支重写：FindProcessByName -> OpenTargetProcess -> VirtualAllocEx
// -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryW)。这里只保留本地学习用的
// LoadLibraryW 方法，没有接入 template 中的 kernel/manualmap/APC 等高级分支。

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static std::vector<std::string> g_logs;
constexpr wchar_t kTargetProcess[] = L"cs2.exe";
constexpr wchar_t kDriverService[] = L"HexSyncService";

void ApplyNeverloseStyle() {
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 7.f; s.ChildRounding = 5.f; s.FrameRounding = 4.f;
    s.PopupRounding = 5.f; s.ScrollbarRounding = 6.f; s.GrabRounding = 4.f;
    s.WindowPadding = ImVec2(14, 14); s.FramePadding = ImVec2(10, 7); s.ItemSpacing = ImVec2(9, 8);
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.045f,0.050f,0.070f,0.98f);
    c[ImGuiCol_ChildBg] = ImVec4(0.065f,0.072f,0.095f,1.f);
    c[ImGuiCol_Border] = ImVec4(0.16f,0.19f,0.25f,1.f);
    c[ImGuiCol_FrameBg] = ImVec4(0.085f,0.095f,0.125f,1.f);
    c[ImGuiCol_Button] = ImVec4(0.35f,0.12f,0.88f,1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.48f,0.20f,1.f,1.f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.28f,0.08f,0.72f,1.f);
    c[ImGuiCol_Header] = c[ImGuiCol_Button]; c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_CheckMark] = ImVec4(0.58f,0.28f,1.f,1.f);
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
    if (!resource) { Log("[-] FindResource(%u) failed: %lu", id, GetLastError()); return false; }
    HGLOBAL loaded = LoadResource(nullptr, resource);
    const DWORD size = SizeofResource(nullptr, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size) { Log("[-] resource %u is empty", id); return false; }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log("[-] create embedded payload failed: %lu", GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes, size, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != size) {
        Log("[-] write embedded payload failed: %lu", GetLastError());
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
    if (!GetTempPathW(MAX_PATH, temp)) { Log("[-] GetTempPathW failed: %lu", GetLastError()); return false; }
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
    if (!hp) Log("[-] OpenProcess(pid=%lu) failed: %lu", pid, GetLastError());
    return hp;
}

bool WriteRemoteBytes(HANDLE hp, void* addr, const void* data, size_t size) {
    SIZE_T written = 0;
    if (!WriteProcessMemory(hp, addr, data, size, &written) || written != size) {
        Log("[-] WriteProcessMemory(%p, %zu B) failed: %lu", addr, size, GetLastError());
        return false;
    }
    return true;
}

bool InjectLoadLibrary(HANDLE hp, const std::wstring& dllPath) {
    Log("[*] method: loadlib / CreateRemoteThread + LoadLibraryW");
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    auto pLoadLibW = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hK32, "LoadLibraryW"));
    if (!pLoadLibW) { Log("[-] GetProcAddress(LoadLibraryW) failed"); return false; }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hp, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { Log("[-] VirtualAllocEx failed: %lu", GetLastError()); return false; }
    if (!WriteRemoteBytes(hp, remote, dllPath.c_str(), bytes)) {
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(hp, nullptr, 0, pLoadLibW, remote, 0, nullptr);
    if (!thread) {
        Log("[-] CreateRemoteThread failed: %lu", GetLastError());
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        return false;
    }

    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);

    if (wait != WAIT_OBJECT_0) { Log("[-] remote thread timeout"); return false; }
    if (!exitCode) { Log("[-] LoadLibraryW returned NULL"); return false; }
    Log("[+] module loaded @ 0x%llX", static_cast<unsigned long long>(exitCode));
    return true;
}

bool NormalInject() {
    PayloadPaths payloads;
    if (!PreparePayloads(false, payloads)) return false;
    EnableDebugPrivilege();
    DWORD pid = FindProcessByName(kTargetProcess);
    if (!pid) { Log("[-] cs2.exe not found"); CleanupPayloads(payloads, false); return false; }
    Log("[*] target cs2.exe PID = %lu", pid);

    HANDLE hp = OpenTargetProcess(pid);
    if (!hp) { CleanupPayloads(payloads, false); return false; }
    bool ok = InjectLoadLibrary(hp, payloads.dll.wstring());
    CloseHandle(hp);
    CleanupPayloads(payloads, false);
    Log(ok ? "[+] injection finished" : "[-] injection failed");
    return ok;
}
bool DriverInject() {
    PayloadPaths payloads;
    if (!PreparePayloads(true, payloads)) return false;
    DWORD pid = FindProcessByName(kTargetProcess);
    if (!pid) { Log("[-] cs2.exe not found"); CleanupPayloads(payloads, true); return false; }

    DriverServiceManager sm;
    // Remove any stale registration from an interrupted previous run before
    // installing the embedded driver image.
    sm.stop_and_delete(kDriverService);
    Log("[*] installing and starting embedded driver");
    if (!sm.install_and_start(kDriverService, payloads.driver.c_str())) {
        Log("[-] driver start failed: %s", sm.last_error().c_str());
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
        Log("[-] HexSync SELF_TEST failed after start: %s", hx.last_error().c_str());
    } else {
        Log("[+] HexSync online, magic=0x%llX", hx.magic());
        Log("[*] driver inject PID=%lu", pid);
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
                Log("[-] remote thread returned but CS2LuaPlugin.dll was not loaded");
            }
        }
        Log(injected ? "[+] kernel injection finished" : "[-] kernel injection failed: %s", hx.last_error().c_str());
    }

    Log("[*] stopping and deleting driver service");
    if (!sm.stop_and_delete(kDriverService)) {
        Log("[-] driver cleanup failed: %s", sm.last_error().c_str());
    } else {
        Log("[+] driver service cleaned up");
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

void RenderUi() {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::Begin("CS2 Lua Injector", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::TextUnformatted("CS2 Lua One-Click Injector");
    ImGui::Separator();
    ImGui::TextUnformatted("Target: cs2.exe");
    ImGui::TextUnformatted("CS2LuaPlugin.dll and HexSync driver are embedded.");
    ImGui::Spacing();

    const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Driver Inject", ImVec2(width, 52))) DriverInject();
    ImGui::SameLine();
    if (ImGui::Button("Normal Inject", ImVec2(width, 52))) NormalInject();

    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : g_logs) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        if (g_pd3dDevice != nullptr) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
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
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"CS2 Lua Injector", WS_OVERLAPPEDWINDOW,
                              100, 100, 660, 420, nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ApplyNeverloseStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    Log("[*] ready");
    Log("[*] embedded target: cs2.exe");

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

        const float clear_color[4] = { 0.06f, 0.06f, 0.07f, 1.00f };
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
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}


