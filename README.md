# CS2-LUA

- 版本：`1.0.2`
- 更新时间：`2026-07-18`
- 运行时：`LuaJIT 2.1 / Lua 5.1 语义`
- 脚本目录：`Counter-Strike Global Offensive\game\bin\win64\lua`
- 菜单热键：`Home`
- 卸载热键：`End`

## 1.0.2 更新内容

- 注入器改为无边框 Neverlose 风格中文界面，增加启动及注入完成动画。
- `CS2LuaInjector.exe` 已内置插件 DLL 与驱动载荷，正常使用只需启动一个 EXE。
- ImGui 菜单和 Lua 路径完整支持中文。
- Lua 脚本改为单独加载、卸载和重载，不再连带重启其他脚本。
- 每个 Lua 可单独勾选“自动加载”，只有选中的脚本会在下次启动时运行。
- 加强 Panorama V8、回调、定时器和持久句柄清理，降低卸载脚本后的残留与崩溃。
- 修复切换画质、全屏模式和分辨率后画面冻结、菜单偏移、点击错位及拖动异常。
- 新增 Neverlose 风格 `group:hotkey(name, default_key)`：左键绑定，右键选择“长按触发”或“按下按键切换”，触发时 UI 文字变色。

## 使用方法

1. 启动 CS2。
2. 以管理员身份运行发布页中的 `CS2LuaInjector.exe`。
3. 在注入器中选择 `cs2.exe` 并点击注入。
4. 注入完成后按 `Home` 打开 Lua 控制中心。

如果无法注入，可在 CS2 启动项中加入：

```text
-allow_third_party_software
```
<img width="2056" height="1213" alt="326770c780654d9b2a3f4f0c55ae60db" src="https://github.com/user-attachments/assets/555c974c-d441-4603-a6bf-8486536cbf91" />


## Lua 热键示例

```lua
local group = ui.create("快捷键")
local hotkey = group:hotkey("开启功能", 0x56) -- V 键

events.render:set(function()
    if hotkey:get() then
        -- 热键触发中
    end
end)
```

## 下载与构建

- 编译好的单文件注入器请前往 [Releases](https://github.com/danqun51/CS2-LUA/releases)。
- 源码构建说明请查看 [help.md](help.md)。
- Neverlose 兼容 API 进度请查看 [进度.md](进度.md)。

## 联系与反馈

- CS2 LUA 交流群 / BUG 提交群：`1063275679`
- [点击链接加入 QQ 群](https://qm.qq.com/q/QuzGioOKMS)
- 项目作者 QQ：`2713639183`

## 支持与赞助

你的赞助是对项目最大的支持，赞助后可联系作者定制 Lua 或新功能。

支付宝：

<img width="539" height="576" alt="支付宝赞助码" src="https://github.com/user-attachments/assets/bfda7794-d1db-4ef4-8b71-3a6085f0bb6f" />

微信：

<img width="600" height="600" alt="微信赞助码" src="https://github.com/user-attachments/assets/2b4753c0-95f7-4a39-99ce-dc20ed76c823" />
