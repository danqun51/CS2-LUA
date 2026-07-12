local ok, config = pcall(require, 'deepseek_config')
if not ok then
    print('[失败] 请在 lua 目录创建 deepseek_config.lua')
    return
end

print('========== DeepSeek AI 猫娘聊天（上下文记忆） ==========', CS2LUA_API_VERSION)
print('[等待] 发送 /ai 问题；/ai clear 可清除自己的当前记忆')

local busy = false
local conversations = {}
local max_history_messages = tonumber(config.max_history_messages) or 12
local system_prompt = config.system_prompt or
    '你是一只可爱、友善的中文猫娘。称呼提问者，回答控制在120个汉字以内，不使用换行，适合CS2聊天，并自然带一点喵系口吻。'

local function safe_say(text)
    text = tostring(text or ''):gsub('[\r\n]+', ' '):gsub('[";]', '')
    local chunks, current, count = {}, {}, 0
    for _, cp in utf8.codes(text) do
        current[#current + 1] = utf8.char(cp)
        count = count + 1
        if count >= 90 then
            chunks[#chunks + 1] = table.concat(current)
            current, count = {}, 0
        end
    end
    if #current > 0 then chunks[#chunks + 1] = table.concat(current) end
    for i, chunk in ipairs(chunks) do
        utils.execute_after((i - 1) * 1.1, function()
            utils.console_exec('say "' .. chunk .. '"')
        end)
    end
end

local function player_key(e)
    -- userid 优先，无法取得时使用名字；记忆只存在于本脚本 Lua 状态中。
    if e.userid ~= nil and tostring(e.userid) ~= '0' then return 'uid:' .. tostring(e.userid) end
    return 'name:' .. tostring(e.get_username())
end

local function get_history(key)
    local history = conversations[key]
    if not history then
        history = {}
        conversations[key] = history
    end
    return history
end

local function trim_history(history)
    while #history > max_history_messages do table.remove(history, 1) end
end

local function make_messages(history, asker, prompt)
    local messages = {{ role = 'system', content = system_prompt }}
    for _, message in ipairs(history) do
        messages[#messages + 1] = { role = message.role, content = message.content }
    end
    messages[#messages + 1] = {
        role = 'user',
        content = ('玩家%s问：%s'):format(asker, prompt)
    }
    return messages
end

events.player_chat:set(function(e)
    local prefix, prompt = tostring(e.text or ''):match('^/(%a+)%s*(.*)$')
    if not prefix or prefix:lower() ~= 'ai' then return end

    local asker = e.get_username()
    local key = player_key(e)
    if prompt:lower() == 'clear' then
        conversations[key] = nil
        safe_say(asker .. ' 的猫娘记忆已经清除啦喵')
        print('[成功] 已清除 AI 上下文: ' .. asker)
        return false
    end
    if prompt == '' then
        safe_say('喵~ 用法是 /ai 加上问题；/ai clear 清除记忆')
        return false
    end
    if busy then
        safe_say('猫娘正在思考上一个问题，请稍等一下喵~')
        return false
    end

    busy = true
    local history = get_history(key)
    local messages = make_messages(history, asker, prompt)
    print(('[AI请求] %s: %s（历史消息 %d）'):format(asker, prompt, #history))

    network.post(config.endpoint, {
        model = config.model or 'deepseek-chat',
        messages = messages,
        temperature = tonumber(config.temperature) or 0.8,
        max_tokens = tonumber(config.max_tokens) or 180
    }, {
        ['Authorization'] = 'Bearer ' .. config.api_key,
        ['Content-Type'] = 'application/json'
    }, function(response, status)
        busy = false
        if status ~= 200 then
            print('[失败] DeepSeek HTTP ' .. tostring(status) .. ': ' .. tostring(response))
            safe_say('连接AI失败了喵，HTTP ' .. tostring(status))
            return
        end

        local parsed_ok, data = pcall(json.decode, response)
        if not parsed_ok or not data.choices or not data.choices[1] then
            print('[失败] DeepSeek响应解析: ' .. tostring(data))
            safe_say('AI回复解析失败了喵')
            return
        end
        local content = data.choices[1].message and data.choices[1].message.content
        if not content then
            safe_say('AI这次没有回复内容喵')
            return
        end

        -- 只有请求成功才写入记忆，失败请求不会污染上下文。
        history[#history + 1] = { role = 'user', content = ('玩家%s问：%s'):format(asker, prompt) }
        history[#history + 1] = { role = 'assistant', content = content }
        trim_history(history)
        print(('[成功] DeepSeek AI回复（已记忆 %d 条）: %s'):format(#history, content))
        safe_say(content)
    end)
    return false
end)

-- conversations 是脚本 Lua 状态内的普通表；卸载或重载脚本销毁状态后会自动清空。
