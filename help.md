# CS2 Lua Plugin Skeleton

本项目是本地学习用骨架：

- `CS2LuaInjector.exe`：ImGui + DirectX11 桌面 UI 注入器。
- 注入流程已按 `D:\Documents\CS2 LUA\CS2-P2C-TEMPLATES-main\source\tools\CS2UnifiedInjector\CS2UnifiedInjector.cpp` 的 `loadlib` 分支风格重写：
  `FindProcessByName -> OpenTargetProcess -> VirtualAllocEx -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryW)`。
- `CS2LuaPlugin.dll`：在目标进程内启动独立工作线程，初始化 Lua 运行时和菜单状态。
- `Insert`：插件侧菜单开关热键（当前为框架/日志层，DX11 ImGui hook 预留在 `src/plugin/ui/menu.*`）。
- `End`：请求卸载插件线程。
- 用户 Lua 脚本目录：默认读取目标程序同级 `lua/*.lua`。
- `nl_compat.lua`、`offsets.lua`、`entity_offsets.lua`、`entity_compat.lua`、
  `panorama_compat.lua` 已作为 RCDATA 资源嵌入 DLL，不需要随 DLL 分发库脚本。
- Panorama 已改为基于 luv8 source2 的 LuaJIT FFI 实现。接口、V8 包装、
  特征码、vtable 索引、面板解析和结构偏移全部位于
  `src/plugin/lua/libraries/panorama_compat.moon`（生成
  `panorama_compat.lua`）。宿主只提供通用的 `panorama_native.call/defer`
  owner-thread 调度点，不再由 Present/worker 线程直接进入 V8，也不使用
  `v8::Locker`。


## 构建

需要 Visual Studio 2022 C++ Build Tools + CMake，构建时会拉取 Lua 和 ImGui：

```powershell
cd "D:\Documents\CS2 LUA\CS2-Lua-Plugin-Skeleton"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```


## 本地运行

1. 启动本地目标进程。
2. 打开：

```powershell
.\build\Release\CS2LuaInjector.exe
```

3. UI 中确认：
   - `Target process`: `cs2.exe`
   - `Plugin DLL`: `CS2LuaPlugin.dll` 的绝对路径，或点 `Default` 使用注入器同目录 DLL。
4. 点击 `Inject`。

DLL 注入后会创建控制台日志。当前插件侧菜单控制已接入脚本管理：

- `Insert`：切换插件菜单状态
- `F5`：加载脚本
- `F6`：卸载脚本
- `F7`：reload scripts
- `F8`：打印脚本列表
- `End`：请求卸载工作线程

说明：注入器仍保留本地用户态 `loadlib` 流程。未接入驱动级注入/内核映射分支。`src/plugin/ui/menu.*` 已整理成 ImGui-ready 的菜单模型，后续如接入合法宿主窗口的 DX11 Present hook，可直接把 `F5/F6/F7/F8` 对应逻辑映射到 `ImGui::Button("Load scripts")` / `Unload scripts` / `Reload scripts` / `Script list`。

## 目录

```text
src/injector/          ImGui + DX11 注入器 EXE
src/plugin/            插件 DLL 主体
src/plugin/lua/        LuaEngine 与嵌入式 Lua 库（libraries/）
src/plugin/ui/         MenuController：热键/menu 状态，预留 ImGui DX11 接入点
scripts/example.lua    示例脚本
```
