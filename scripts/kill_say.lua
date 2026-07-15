-- 击杀喊话（CS2LuaPlugin API 1.0.1）
-- 当前 API 尚未原生派发 player_death，因此使用已实机验证的 player_hurt，
-- 先确认攻击者是本地玩家，再确认受害者是敌人且已经死亡。

local say_text = "请输入文本"

events.player_hurt:set(function(e)

    local local_player = entity.get_local_player()
    local attacker = entity.get(e.attacker, true)

    if not local_player or not attacker then
        return
    end

    if local_player:get_index() ~= attacker:get_index() then
        return
    end

    -- 只有伤害事件报告目标生命值归零时，才进入死亡实体复核。
    if tonumber(e.health) ~= 0 then
        return
    end

    utils.console_exec('say "' .. say_text ..'"')
end)

print('[击杀喊话] 已加载：先验证本地玩家击杀，再验证敌人死亡。')


