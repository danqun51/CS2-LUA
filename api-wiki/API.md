# CS2LuaPlugin API Wiki

版本：`0.7.0`  
脚本目录：`Counter-Strike Global Offensive\game\bin\win64\lua`  
菜单热键：`Home`

## 脚本生命周期

菜单支持单脚本加载、卸载和重载。每个脚本具有独立 Lua 状态；卸载或重载会清除该脚本的事件回调、定时器、等待派发的网络回调以及 Lua 内存数据。加载和回调错误会在 CS2 控制台输出文件、行号、原因和 traceback。

## events

```lua
events.name:set(callback)
events.name:unset()
events.name:call(...)
```

已接入：`tick`、`render`、`load`、`shutdown`、`player_hurt`、`player_chat`。

`player_hurt` 字段：`userid`、`attacker`、`health`、`armor`、`dmg_health`、`dmg_armor`、`hitgroup`、`weapon`。

`player_chat` 字段：`userid`、`text`、`name`、`teamonly`、`get_username()`。

```lua
events.player_chat:set(function(e)
    print(('[聊天] %s: %s'):format(e.get_username(), e.text))
end)
```

## network

请求在后台线程执行，Lua callback 在游戏 tick 中派发，不阻塞游戏。

```lua
network.get(url, headers, function(response, status)
    print(status, response)
end)

network.post(url, { hello = 'world' }, {
    ['Content-Type'] = 'application/json'
}, function(response, status)
    print(status, response)
end)
```

函数立即返回布尔值。POST data 为 table 时自动 JSON 编码。卸载脚本后不会继续调用该脚本的 callback。

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
utils.opcode_scan(module, signature, offset)
utils.get_netvar_offset(table, prop)
utils.get_vfunc(index, ffi_function_type)
utils.get_vfunc(module, interface, index, ffi_function_type)
utils.trace_line(...)
utils.trace_hull(...)
utils.trace_bullet(...)
utils.net_channel()
```

`trace_*` 和 `net_channel` 当前主要提供兼容结构，尚未完整连接 Source 2。

## Current features

| Library | Description | 状态 |
|---|---|---|
| LuaJIT 2.1 | Lua JIT 编译运行时 | 已嵌入 Windows x64 静态版本，`jit` 全局可用 |
| FFI | 从 Lua 声明 C 类型、调用外部 C 函数和操作 C 数据 | `ffi` 全局及 `require('ffi')` 可用 |
| BIT | LuaJIT BitOp 数值位运算库 | `bit` 全局及 `require('bit')` 可用 |

```lua
print(jit.version)

ffi.cdef[[
typedef struct { int x; float y; } example_t;
]]
local value = ffi.new('example_t')
value.x = 123

print(bit.band(7, 3)) -- 3
print(bit.tohex(255)) -- 000000ff
```

LuaJIT 使用 Lua 5.1 语义。兼容层额外提供 `table.unpack`、`utf8.char` 和 `utf8.codes`，供现有 Lua 5.4 风格脚本继续运行。

## vector / color / bit

```lua
local v = vector(1, 2, 3)
v:length(); v:length2d(); v:normalized(); v:dot(other)
v:cross(other); v:dist(other); v:lerp(other, t)

local c = color(255, 128, 0, 255)
c:unpack(); c:to_hex(); c:to_int32(); c:lerp(other, t)
c:grayscale(); c:alpha_modulate(128)

bit.band(...); bit.bor(...); bit.bxor(a, b)
bit.bnot(a); bit.lshift(a, b); bit.rshift(a, b)
```

## files / db / json

```lua
files.read(path); files.write(path, data); files.append(path, data)
files.exists(path); files.size(path); files.rename(old, new)
files.remove(path); files.create_directory(path); files.list(path)

db.read(key); db.write(key, value); db.delete(key)

local text = json.stringify({ name = 'cat', value = 123 })
local value = json.parse(text)
-- aliases: json.encode / json.decode
```

`db` 为 JSON 磁盘持久化 KV。JSON 支持 Unicode、`\uXXXX` 和 surrogate pair。

## schema / offsets

```lua
schema.get_offset('client.dll', 'dwLocalPlayerPawn')
schema.get_button('attack')
schema.get_build_number()
offsets.info; offsets.offsets; offsets.buttons
```

偏移数据来源：`D:\Documents\CS2 LUA\output`。

## DeepSeek AI 示例

`deepseek_ai.lua` 监听 `/ai 问题`，按玩家保存上下文。

```text
/ai 记住数字527
/ai 我刚才让你记住什么
/ai clear
```

默认最多保留 12 条历史消息。`/ai clear` 清除个人记忆；卸载或重载脚本清除全部内存记忆。

## entity

```lua
entity.get(index [, by_userid])
entity.get_local_player()
entity.get_players([enemies_only, include_dormant, callback])
entity.get_entities([class, include_dormant, callback])
entity.get_game_rules()
entity.get_player_resource()
entity.get_threat([hittable])
```

实体对象当前支持：

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

已原生读取常用字段：`m_iHealth`、`m_iMaxHealth`、`m_iTeamNum`、`m_lifeState`、`m_vecVelocity`、`m_vecAbsVelocity`。支持通过 `ent.prop_name` 读取或写入这些字段。

当前骨骼、hitbox、class info、materials、动画状态、player resource、threat 和 movement simulation 尚未完整接入。

## 当前兼容状态

| 模块 | 状态 |
|---|---|
| Lua 生命周期、错误机制、print | 可用 |
| player_hurt / player_chat | 已验证 |
| network GET/POST | 异步可用 |
| vector/color/bit/files/db/json | 基础实现 |
| utils/globals/common/schema | 部分原生、部分兼容 |
| entity/render/ui/cvar/materials/panorama/esp/rage | 待继续实现 |
