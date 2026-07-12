print('========== player_hurt 原生事件测试 ==========', CS2LUA_API_VERSION)
local hitgroup_str = {[0]='generic','head','chest','stomach','left arm','right arm','left leg','right leg','neck','generic','gear'}
local count = 0
events.player_hurt:set(function(e)
    count = count + 1
    print(('[成功] player_hurt 原生事件 #%d: attacker=%s userid=%s hitgroup=%s damage=%d health=%d weapon=%s'):format(
        count, tostring(e.attacker), tostring(e.userid), hitgroup_str[e.hitgroup] or tostring(e.hitgroup),
        e.dmg_health or 0, e.health or 0, tostring(e.weapon)))
end)
print('[等待] 请进入本地对局击中任意人物；成功时会输出 [成功] player_hurt 原生事件')
