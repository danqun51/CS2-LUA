# CS2-LUA 构建与使用说明

## 发布包

`CS2LuaInjector.exe` 是发布给普通用户使用的单文件注入器，其中已经嵌入：

- `CS2LuaPlugin.dll`
- `CS2HexSyncCompatDriver.sys`

注入时载荷会释放到私有临时目录，完成后自动清理。正常使用不需要把 DLL 和 SYS 放在 EXE 旁边。

## 源码构建

需要：

- Windows 10/11 x64
- Visual Studio 2022，安装“使用 C++ 的桌面开发”
- CMake 3.24 或更高版本
- 首次配置时能够访问 GitHub，以获取 LuaJIT、Dear ImGui 和 MinHook

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target CS2LuaInjector
```

输出文件：

```text
build/Release/CS2LuaInjector.exe
build/Release/CS2LuaPlugin.dll
```

## 使用方法

1. 启动 `cs2.exe`。
2. 以管理员身份运行 `CS2LuaInjector.exe`。
3. 确认目标进程后点击“开始注入”。
4. 等待注入动画完成，按 `Home` 显示或隐藏 Lua 控制中心。
5. 按 `End` 安全卸载插件。

Lua 脚本目录：

```text
Counter-Strike Global Offensive/game/bin/win64/lua
```

脚本列表中的“自动加载”是逐脚本保存的。未勾选的脚本不会在插件启动时自动运行。

## Neverlose 风格热键

```lua
local group = ui.create("功能")
local bind = group:hotkey("功能热键", 0x46) -- F 键

bind:set_callback(function(ref)
    print("当前状态：", ref:get())
end)

events.render(function()
    if bind:get() then
        -- 功能逻辑
    end
end)
```

- 左键点击热键控件：监听并绑定下一次按键。
- `Esc`：取消本次监听。
- 右键点击热键控件：选择“长按触发”或“按下按键切换”。
- 触发时控件文字和左侧状态标记会变色。
- `ui.get_binds()` 可取得当前脚本创建的热键状态、模式、按键值和引用。

## 目录结构

```text
src/injector/                 中文无边框注入器与驱动通信
src/plugin/                   插件运行时
src/plugin/lua/               LuaEngine 与嵌入式兼容库
src/plugin/lua/libraries/     Neverlose/Panorama/entity 兼容层
src/plugin/ui/                DX11 ImGui 菜单及分辨率同步
src/plugin/events/            Source 2 游戏事件桥接
scripts/                      示例与测试 Lua
api-wiki/                     API 文档
```

## 常用热键

| 热键 | 功能 |
|---|---|
| `Home` | 显示/隐藏 Lua 控制中心 |
| `End` | 卸载插件 |
| `F8` | 在控制台打印脚本列表 |

## 联系方式

- QQ 群：`1063275679`
- 加群链接：https://qm.qq.com/q/QuzGioOKMS
- GitHub：https://github.com/danqun51/CS2-LUA
