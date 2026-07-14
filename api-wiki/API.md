# CS2LuaPlugin API Wiki

版本：`1.0.1`  
更新时间：`2026-07-14`  
运行时：`LuaJIT 2.1 / Lua 5.1 语义`  
脚本目录：`Counter-Strike Global Offensive\game\bin\win64\lua`  
菜单热键：`Home`

> 本文以当前参与构建的源码为准。`进度.md` 中较早的 0.9.x/1.0.x 条目属于历史实验记录，不代表对应实现仍在当前构建中。

## 快速开始

```lua
print('CS2Lua API', CS2LUA_API_VERSION)

events.render:set(function()
    -- 每帧调用
end)
```

脚本错误会输出文件名、行号、原因和 traceback。当前 `events.name:set` 是单槽注册：同一个 Lua 状态内再次 `set` 会替换此前回调，不是多监听器列表。

## LuaJIT / FFI / bit

```lua
print(jit.version)
local ffi = require('ffi')
local bit = require('bit')

ffi.cdef[[typedef struct { int x; float y; } example_t;]]
local value = ffi.new('example_t')
value.x = 123
print(bit.band(7, 3)) -- 3
```

全局提供 `jit`、`ffi`、`bit`。兼容层额外提供 `table.unpack`、`utf8.char`、`utf8.codes`。

## events

```lua
events.name:set(callback) -- 注册或替换
events.name:unset()       -- 移除
events.name:call(...)     -- 手动调用当前回调
```

### 已实际派发

| 事件 | 参数/说明 |
|---|---|
| `load` | 无参数，脚本加载阶段 |
| `shutdown` | 无参数，卸载阶段 |
| `tick` | 无参数，插件循环 |
| `render` | 无参数，菜单 Present 渲染阶段 |
| `player_hurt` | table：`userid`、`attacker`、`health`、`armor`、`dmg_health`、`dmg_armor`、`hitgroup`、`weapon` |
| `player_chat` | table：`userid`、`text`、`name`、`teamonly`、`get_username()` |

```lua
events.player_chat:set(function(e)
    print(('[聊天] %s: %s'):format(e.get_username(), e.text))
end)
```

以下名称已创建，但当前源码没有完整原生派发：`createmove`、`createmove_run`、`level_init`、`pre_render`、`post_render`、`net_update_start/end`、`console_input`、`mouse_input`、`player_say`、`player_death`、`weapon_fire`、`bullet_impact`、`round_start/end`、`bomb_planted/defused`。

## network

请求在 WinHTTP 后台线程执行，callback 在 Lua tick 中派发。

```lua
network.get(url [, headers], function(response, status)
    print(status, response)
end)

network.post(url, data [, headers], function(response, status)
    print(status, response)
end)
```

- 调用成功入队后立即返回 `true`。
- `data` 可以是字符串或 table；table 自动通过 `json.encode` 编码。
- 网络失败时 `status` 通常为 `0`，`response` 包含错误文本。
- Lua 引擎关闭时会等待后台 worker 退出。

## utils

```lua
utils.console_exec(cmd, ...)
utils.execute_after(delay, callback, ...)
utils.random_int(min, max)
utils.random_float(min, max)
utils.random_seed(seed)
utils.clamp(value, min, max)
utils.lerp(a, b, t)
utils.get_unix_time()
utils.get_timestamp()
utils.create_interface(module, interface)
utils.opcode_scan(module, signature [, offset])
utils.is_readable(address [, size])
utils.get_netvar_offset(table, prop)
utils.get_vfunc(index, ffi_function_type)
utils.get_vfunc(module, interface, index, ffi_function_type)
utils.trace_line(...)
utils.trace_hull(...)
utils.trace_bullet(...)
utils.net_channel()
```

`console_exec` 会进入延迟命令队列。`trace_*` 和 `net_channel` 当前主要返回兼容结构，没有接入完整 Source 2 Trace/NetChannel。

## vector

```lua
local v = vector(1, 2, 3)
v:clone(); v:unpack(); v:length(); v:length2d(); v:length_sqr()
v:dist(other); v:dist_sqr(other); v:dot(other); v:cross(other)
v:normalized(); v:normalize(); v:lerp(other, t); v:angles()
```

支持 `+ - * / == tostring`。

## color

```lua
local c = color(255, 128, 0, 255)
local hex = color('#FF8000FF')
c:clone(); c:init(r,g,b,a); c:unpack(); c:to_fraction()
c:to_hex(); c:to_int32(); c:lerp(other,t)
c:grayscale([weight]); c:alpha_modulate(alpha)
```

## files

```lua
files.read(path)
files.write(path, data)
files.append(path, data)
files.exists(path)
files.size(path)
files.rename(old_path, new_path)
files.remove(path)
files.create_directory(path)
files.list(path)
```

## db

```lua
db.read(key)
db.write(key, value)
db.delete(key)
```

当前 `db` 是 Lua 状态内存 KV，不写入磁盘；卸载/重载清空。

## json

```lua
local text = json.stringify({ name = 'cat', value = 123 })
local value = json.parse(text)
-- aliases
json.encode(value)
json.decode(text)
```

支持 Unicode `\uXXXX` 和 surrogate pair。注意：当前编码器无法区分空 Lua 数组和空对象，空 table 默认会编码成 `[]`。构造 JSON Schema 时，`properties = {}` 可能需要加入一个可选字段，避免服务端要求 object 却收到 array。

## schema / offsets

```lua
schema.get_offset('client.dll', 'dwLocalPlayerPawn')
schema.get_button('attack')
schema.get_build_number()
offsets.info
offsets.offsets
offsets.buttons
```

偏移由 `D:\Documents\CS2 LUA\output` 生成。当前是生成表查询，不是完整 `SchemaSystem_001` 反射。

## entity

```lua
entity.get(index [, by_userid])
entity.get_local_player()
entity.get_players([enemies_only, include_dormant, callback])
entity.get_entities([class, include_dormant, callback])
entity.get_game_rules()
entity.get_player_resource() -- 当前返回 nil
entity.get_threat([hittable]) -- 当前返回 nil
```

实体对象：

```lua
ent[0] -- FFI pointer
ent:is_player(); ent:is_weapon(); ent:is_dormant(); ent:is_bot()
ent:is_alive(); ent:is_enemy(); ent:is_visible(); ent:is_occluded()
ent:get_index(); ent:get_name(); ent:get_origin(); ent:get_angles()
ent:get_eye_position(); ent:get_simulation_time()
ent:get_classname(); ent:get_classid(); ent:get_model_name()
ent:get_network_state(); ent:get_bbox(); ent:get_player_info()
ent:get_player_weapon([all_weapons]); ent:get_xuid()
```

原生读取字段：`m_iHealth`、`m_iMaxHealth`、`m_iTeamNum`、`m_lifeState`、`m_vecVelocity`、`m_vecAbsVelocity`。当前实体 entry stride 会运行时探测；已实机验证 controller/pawn、本地玩家、10 人遍历、活动武器和 GameRules。骨骼、hitbox、真实 class info、materials、动画和 player resource 尚未接入。

## ui

```lua
local group = ui.create('脚本', '设置')
local enabled = group:switch('启用', true)
local amount = group:slider('数值', 0, 100, 50)
local mode = group:combo('模式', 'A', 'B', 'C')
local button = group:button('执行', function() print('clicked') end)

print(enabled:get())
enabled:set(false)
enabled:set_callback(function() print(enabled:get()) end)
```

MenuGroup 当前提供：`switch`、`slider`、`combo`、`selectable`、`list`、`listable`、`button`、`hotkey`、`input`、`label`、`color_picker`、`texture`。

MenuItem 当前提供：`get/set/id/list/type/override/get_override/update/reset/name/tooltip/visibility/disabled/set_callback/unset_callback`。`color_picker/texture`、完整 hotkey 捕获和内置菜单路径仍是部分实现。

## panorama（实验）

当前后端是嵌入的 `panorama_compat.lua`，直接使用 LuaJIT FFI 操作 Panorama/V8。它尚未达到稳定 API 标准。

### 推荐：一次性 JS-only 安装

```lua
local ok, result = panorama.run_script([[
(() => {
    const KEY = '__my_script_v1';
    if (globalThis[KEY]) return;
    globalThis[KEY] = true;

    $.RegisterForUnhandledEvent('ShowAcceptPopup', () => {
        $.Msg('accept popup');
        // 事件处理继续留在 JS 内，不回调 Lua。
    });
})();
]], 'CSGOMainMenu')

if not ok then print(result) end
```

`panorama.run_script(js_code [, panel])` 返回 `ok, result_or_error`。

### 实验性代理 API

```lua
local fn = panorama.loadstring([[
    return { echo: (text) => $.Msg(text) };
]], 'CSGOMainMenu')
local api = fn()
api.echo('hello')

local js = panorama.open('CSGOMainMenu')
js.PartyListAPI.SessionCommand(...)
```

同时存在：

```lua
panorama.loadrawstring(js [, panel])
panorama.runScript(js [, panel [, xml_context]])
panorama.hasPanel(name)
panorama.getPanel(name [, fallback])
panorama.getRootPanel(name [, fallback])
panorama.getIsolate()
panorama.debug_status()
panorama.flush()
panorama.setSafeMode(enabled)
panorama.pairs(value)
panorama.ipairs(value)
panorama.len(value)
panorama.type(value)
```

### 已知致命限制

以下模式可能触发 V8 进程级 Fatal Assertion，Lua `pcall/xpcall` 无法捕获：

- 在 `render/tick/execute_after` 中持续调用 `loadstring/open` 返回的函数或对象。
- 跨帧保存 JS Array/Object/Function/PersistentProxy。
- Lua 定时轮询 JS 事件队列。
- Panorama JS 回调 Lua，Lua 回调中再次进入 Panorama。
- 面板 Context 被销毁后继续使用旧代理。

常见错误：

```text
EscapableHandleScope::Escape - Escape value set twice
v8::Context::Exit - Cannot exit non-entered context
```

当前没有正式的 `panorama_events` API。测试脚本采用 JS-only 事件所有权：Lua 只安装一次，事件注册、参数处理和动作均留在 JS 内。

## common

```lua
common.get_username()
common.get_product_version()
common.get_date([format [, unix_time]])
common.get_unixtime()
common.get_timestamp()
common.get_system_time()
common.get_game_directory()
common.get_map_data()
common.get_config_name()
common.get_active_scripts()
common.is_in_thirdperson()
common.add_event(text [, icon])
common.add_notify(title, text)
```

其中地图、配置、脚本列表、第三人称等部分仍是兼容占位。

## 当前状态摘要

| 模块 | 状态 |
|---|---|
| LuaJIT/FFI/bit、脚本错误输出 | 可用 |
| tick/render/load/shutdown | 可用 |
| player_hurt/player_chat | 已实机验证 |
| network GET/POST | 异步可用 |
| vector/color/json/files | 基础可用 |
| entity | 原生基础桥已验证，复杂数据待补 |
| ui | 基础控件可用 |
| utils/globals/common/schema/db | 部分原生、部分兼容或内存实现 |
| panorama | 实验；只推荐一次性 JS-only `run_script` |
| render/cvar/materials/esp/rage | 未完成 |
