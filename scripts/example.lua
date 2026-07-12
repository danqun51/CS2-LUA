local offsets = require("offsets")

print("[example.lua] loaded")
print(string.format("[example.lua] offsets build=%s timestamp=%s", tostring(offsets.info.build_number), tostring(offsets.info.timestamp)))
print(string.format("[example.lua] client.dwLocalPlayerPawn=0x%X", offsets.offsets["client.dll"].dwLocalPlayerPawn))

function on_load()
  print("[example.lua] on_load")
end

function on_unload()
  print("[example.lua] on_unload")
end

function on_tick(dt)
  -- 本地学习示例：每帧/每轮询周期会被调用。
end
