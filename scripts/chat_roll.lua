print('========== 聊天 /roll 测试 ==========', CS2LUA_API_VERSION)

local last_random_int

events.player_chat:set(function(e)
    print(('[聊天] %s: %s'):format(e.get_username(), e.text))

    if e.text == '/roll' then
        local random_int = utils.random_int(1, 6)
        local str = e.get_username() .. ' rolled a ' .. random_int

        if random_int == 1 and random_int == last_random_int then
            str = str .. '... snake eyes!'
        end

        utils.console_exec('say ', str)
        utils.execute_after(1.0, function()
            utils.console_exec('echo CS2LuaPlugin delayed console_exec success')
        end)
        print('[成功] player_chat /roll 功能正常: ' .. str)
        last_random_int = random_int
        return false
    end
end)

print('[等待] 任意玩家在聊天中发送 /roll')
