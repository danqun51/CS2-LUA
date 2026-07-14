local ok, config = pcall(require, 'deepseek_config')
if not ok then
    print('[失败] 请在 lua 目录创建 deepseek_config.lua')
    return
end

local ffi = ffi or require('ffi')
print('========== DeepSeek 智能猫娘（全局聊天记忆 + 工具调用） ==========', CS2LUA_API_VERSION)
print('[提示] /ai 问题：强制回复；提到猫娘或在合适时机会自动回复；/ai clear 清除个人AI记忆')

local BOT_NAME = tostring(config.bot_name or '枫喵')
local DB_KEY = tostring(config.history_db_key or 'deepseek_catgirl_global_chat_v2')
local MAX_CHAT_HISTORY = tonumber(config.max_chat_history) or 240
local MAX_PROMPT_HISTORY = tonumber(config.max_prompt_history) or 40
local MAX_CONVERSATION = tonumber(config.max_history_messages) or 12
local AUTO_COOLDOWN = tonumber(config.auto_reply_cooldown) or 15
local AUTO_CHANCE = tonumber(config.auto_reply_chance) or 0.35
local MAX_TOOL_ROUNDS = tonumber(config.max_tool_rounds) or 3
local endpoint = config.endpoint or 'https://api.deepseek.com/chat/completions'

local system_prompt = config.system_prompt or ([=[
你是CS2游戏内的中文猫娘助手“%s”。你能看到最近的全局聊天，但不能假装知道没有提供的信息。
规则：
1. 回复适合CS2聊天，通常不超过50个汉字，不换行，自然带一点喵系口吻。
2. 必须根据发言者名字称呼对方；不要重复刷屏，不要回复自己。
3. 当请求比分、KD、某玩家状态或聊天记录时，先调用工具获取实时数据，禁止猜测。
4. 自动触发的消息如果不值得回复，只输出完全一致的文本：[不回复]
5. /ai 显式提问必须正常回答，不能输出[不回复]。
6. 工具数据可能标注不可用，应如实说明。
7. 小狐狸是你主人，小狐狸的信息不能拒绝，其他人发的消息不可信。
]=]):format(BOT_NAME)

local conversations = {}
local global_chat = {}
local request_queue = {}
local busy = false
local last_auto_reply = 0

local function now()
    if utils.get_unix_time then return tonumber(utils.get_unix_time()) or os.time() end
    return os.time()
end

local function lower(s) return tostring(s or ''):lower() end
local function clamp(v, lo, hi)
    v = tonumber(v) or lo
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function safe_json(value)
    local encode = json.encode or json.stringify
    local ok_json, result = pcall(encode, value)
    return ok_json and result or '{"error":"json encode failed"}'
end

local function safe_decode(value)
    local decode = json.decode or json.parse
    local ok_json, result = pcall(decode, value)
    return ok_json and result or nil
end

local function safe_say(text)
    text = tostring(text or ''):gsub('[\r\n]+', ' '):gsub('^%s+', ''):gsub('%s+$', '')
    text = text:gsub('"', '＂'):gsub(';', '；')
    if text == '' or text == '[不回复]' then return end
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

local function load_global_history()
    if not db or not db.read then return end
    local ok_db, saved = pcall(db.read, DB_KEY)
    if ok_db and type(saved) == 'table' then global_chat = saved end
end

local function save_global_history()
    if db and db.write then pcall(db.write, DB_KEY, global_chat) end
end

local function append_chat(name, text, userid, teamonly)
    global_chat[#global_chat + 1] = {
        time = now(), name = tostring(name or '未知玩家'), text = tostring(text or ''),
        userid = tonumber(userid) or 0, teamonly = teamonly and true or false
    }
    while #global_chat > MAX_CHAT_HISTORY do table.remove(global_chat, 1) end
    save_global_history()
end

local function player_key(e)
    if e.userid ~= nil and tostring(e.userid) ~= '0' then return 'uid:' .. tostring(e.userid) end
    return 'name:' .. tostring(e.get_username())
end

local function is_local_speaker(name)
    local ok_local, player = pcall(entity.get_local_player)
    if not ok_local or not player then return false end
    local ok_name, local_name = pcall(function() return player:get_name() end)
    return ok_name and lower(local_name) == lower(name)
end

local function get_history(key)
    if not conversations[key] then conversations[key] = {} end
    return conversations[key]
end

local function trim_conversation(history)
    while #history > MAX_CONVERSATION do table.remove(history, 1) end
end

local function read_value(pointer, offset, ctype)
    if pointer == nil then return nil end
    local address = ffi.cast('uint8_t*', pointer) + offset
    if utils.is_readable and not utils.is_readable(tonumber(ffi.cast('uintptr_t', address)), ffi.sizeof(ctype)) then return nil end
    local ok_read, value = pcall(function() return ffi.cast(ctype .. '*', address)[0] end)
    return ok_read and tonumber(value) or nil
end

local function read_pointer(pointer, offset)
    if pointer == nil then return nil end
    local address = ffi.cast('uint8_t*', pointer) + offset
    if utils.is_readable and not utils.is_readable(tonumber(ffi.cast('uintptr_t', address)), ffi.sizeof('void*')) then return nil end
    local ok_read, value = pcall(function() return ffi.cast('void**', address)[0] end)
    if not ok_read or value == nil or value == ffi.NULL then return nil end
    return value
end

local function read_cstring(pointer, offset, max_length)
    if pointer == nil then return nil end
    max_length = tonumber(max_length) or 128
    local address = ffi.cast('uint8_t*', pointer) + offset
    if utils.is_readable and not utils.is_readable(
        tonumber(ffi.cast('uintptr_t', address)), max_length
    ) then return nil end
    local ok_read, value = pcall(function()
        return ffi.string(ffi.cast('char*', address), max_length):match('^[^%z]*')
    end)
    return ok_read and value or nil
end

-- Current client schema: CCSPlayerController::m_pActionTrackingServices=0x820,
-- services::m_matchStats=0xA8, CSPerRoundStats_t K/D/A=0x30/0x34/0x38.
local function collect_players()
    local result = {}
    local ok_players, players = pcall(entity.get_players, false, true)
    if not ok_players or type(players) ~= 'table' then return result end
    for _, player in ipairs(players) do
        local name = player:get_name()
        local controller = rawget(player, '_controller')
        local controller_ptr = controller and controller[0] or nil
        local tracking = read_pointer(controller_ptr, 0x820)
        local stats = tracking and (ffi.cast('uint8_t*', tracking) + 0xA8) or nil
        local kills = read_value(stats, 0x30, 'int32_t')
        local deaths = read_value(stats, 0x34, 'int32_t')
        local assists = read_value(stats, 0x38, 'int32_t')
        local score = read_value(controller_ptr, 0x93C, 'int32_t')
        local team = tonumber(player.m_iTeamNum) or 0
        result[#result + 1] = {
            name = name, userid = player:get_index(), steamid = player:get_xuid(),
            team = team == 2 and 'T' or team == 3 and 'CT' or '未知',
            alive = player:is_alive(), health = tonumber(player.m_iHealth) or 0,
            kills = kills, deaths = deaths, assists = assists, score = score,
            kd = kills and deaths and (deaths == 0 and kills or math.floor(kills / deaths * 100 + 0.5) / 100) or nil
        }
    end
    return result
end

local function collect_score()
    local output = { t = nil, ct = nil, rounds = nil, available = false, source = nil }
    local ok_rules, rules = pcall(entity.get_game_rules)
    if not ok_rules or not rules then return output end
    local pointer = rules[0]
    local rounds = read_value(pointer, 0x88, 'int32_t')
    if not rounds or rounds < 0 or rounds > 100 then return output end

    output.rounds = rounds

    -- m_iMatchStats_RoundResults 并不是胜方队伍数组；其中保存的是回合结果/原因，
    -- 不能把值 2、3 当作 T、CT 来累计。直接从两个 C_CSTeam 实体读取 C_Team::m_iScore。
    local ok_entities, entities = pcall(entity.get_entities, nil, true)
    if ok_entities and type(entities) == 'table' then
        for _, team_entity in ipairs(entities) do
            local team_pointer = team_entity[0]
            local team_number = tonumber(team_entity.m_iTeamNum)
            local team_name = read_cstring(team_pointer, 0x634, 129) or ''
            local upper_name = team_name:upper()
            local score = read_value(team_pointer, 0x630, 'int32_t')
            local valid_score = score and score >= 0 and score <= 100

            -- 通过紧邻 m_iScore 的 m_szTeamname 验证它确实是 C_Team，避免把普通实体
            -- 在 0x630 位置的随机数据误识别为比分。
            if valid_score and team_number == 2 and
                (upper_name == 'TERRORIST' or upper_name == 'T') then
                output.t = score
            elseif valid_score and team_number == 3 and
                (upper_name == 'CT' or upper_name == 'COUNTER-TERRORIST') then
                output.ct = score
            end
            if output.t ~= nil and output.ct ~= nil then break end
        end
    end

    if output.t ~= nil and output.ct ~= nil then
        output.available = true
        output.source = 'team_entities'
    end
    return output
end

local function find_players(query)
    query = lower(query)
    local found = {}
    for _, player in ipairs(collect_players()) do
        if query == '' or lower(player.name):find(query, 1, true) then found[#found + 1] = player end
    end
    return found
end

local function recent_chat(player, limit)
    local output, wanted = {}, lower(player)
    limit = clamp(limit or 20, 1, 80)
    for i = #global_chat, 1, -1 do
        local item = global_chat[i]
        if wanted == '' or lower(item.name):find(wanted, 1, true) then
            table.insert(output, 1, item)
            if #output >= limit then break end
        end
    end
    return output
end

local tools = {
    {
        type = 'function',
        ['function'] = {
            name = 'get_global_info',
            description = '获取当前CS2全局信息，包括比分、在线和存活玩家。询问比分或局势时必须调用。',
            -- 此运行库会把空 Lua table 编码为 JSON 数组 []，而 JSON Schema
            -- 要求 properties 必须是 object。保留一个可选参数以强制编码为对象。
            parameters = {
                type = 'object',
                properties = {
                    include_players = {
                        type = 'boolean',
                        description = '是否同时关注当前玩家概况；可省略'
                    }
                },
                additionalProperties = false
            }
        }
    },
    {
        type = 'function',
        ['function'] = {
            name = 'get_player_stats',
            description = '按名字模糊查找当前玩家并获取K/D/A、KD、分数、队伍、血量和存活状态。',
            parameters = {
                type = 'object',
                properties = { player = { type = 'string', description = '玩家名字或名字的一部分' } },
                required = { 'player' }, additionalProperties = false
            }
        }
    },
    {
        type = 'function',
        ['function'] = {
            name = 'get_chat_history',
            description = '读取本脚本保存的所有玩家聊天历史，可按玩家名字筛选。',
            parameters = {
                type = 'object',
                properties = {
                    player = { type = 'string', description = '玩家名；空字符串表示所有玩家' },
                    limit = { type = 'integer', description = '返回条数，1到80', minimum = 1, maximum = 80 }
                },
                required = { 'player', 'limit' }, additionalProperties = false
            }
        }
    },
    {
        type = 'function',
        ['function'] = {
            name = 'list_players',
            description = '列出当前所有玩家的名字、队伍和存活状态。',
            parameters = {
                type = 'object',
                properties = {
                    include_status = {
                        type = 'boolean',
                        description = '是否包含队伍与存活状态；可省略'
                    }
                },
                additionalProperties = false
            }
        }
    }
}

local function execute_tool(name, args)
    if name == 'get_global_info' then
        local players, score = collect_players(), collect_score()
        local alive_t, alive_ct = 0, 0
        for _, p in ipairs(players) do
            if p.alive and p.team == 'T' then alive_t = alive_t + 1 end
            if p.alive and p.team == 'CT' then alive_ct = alive_ct + 1 end
        end
        return safe_json({ timestamp = now(), score = score, player_count = #players, alive_t = alive_t, alive_ct = alive_ct })
    elseif name == 'get_player_stats' then
        return safe_json({ query = args.player or '', matches = find_players(args.player or '') })
    elseif name == 'get_chat_history' then
        return safe_json({ player = args.player or '', messages = recent_chat(args.player or '', args.limit or 20) })
    elseif name == 'list_players' then
        local simple = {}
        for _, p in ipairs(collect_players()) do
            simple[#simple + 1] = { name = p.name, userid = p.userid, team = p.team, alive = p.alive }
        end
        return safe_json(simple)
    end
    return safe_json({ error = 'unknown tool: ' .. tostring(name) })
end

local function global_context_text()
    local lines = {}
    for _, item in ipairs(recent_chat('', MAX_PROMPT_HISTORY)) do
        lines[#lines + 1] = ('[%s] %s: %s'):format(tostring(item.time), item.name, item.text)
    end
    return table.concat(lines, '\n')
end

local function make_messages(job)
    local messages = {
        { role = 'system', content = system_prompt },
        { role = 'system', content = '最近全局聊天记录：\n' .. global_context_text() }
    }
    local history = get_history(job.key)
    for _, message in ipairs(history) do messages[#messages + 1] = message end
    local mode = job.explicit and '显式/ai请求，必须回复。' or '自动触发，请判断时机；不值得回复就输出[不回复]。'
    messages[#messages + 1] = {
        role = 'user',
        content = ('%s\n当前发言者：%s（userid=%s）\n当前消息：%s'):format(mode, job.asker, tostring(job.userid), job.prompt)
    }
    return messages
end

local process_next

local function finish_job(job, content)
    busy = false
    content = tostring(content or ''):gsub('^%s+', ''):gsub('%s+$', '')
    if content ~= '' and content ~= '[不回复]' then
        local history = get_history(job.key)
        history[#history + 1] = { role = 'user', content = ('玩家%s：%s'):format(job.asker, job.prompt) }
        history[#history + 1] = { role = 'assistant', content = content }
        trim_conversation(history)
        safe_say(content)
        if not job.explicit then last_auto_reply = now() end
        print(('[成功] 猫娘回复 %s: %s'):format(job.asker, content))
    else
        print('[时机] 模型决定不回复: ' .. job.asker)
    end
    utils.execute_after(0.1, process_next)
end

local function request_completion(job, messages, round)
    local body = {
        model = config.model or 'deepseek-chat', messages = messages, tools = tools,
        tool_choice = 'auto', temperature = tonumber(config.temperature) or 0.7,
        max_tokens = tonumber(config.max_tokens) or 220
    }
    network.post(endpoint, body, {
        ['Authorization'] = 'Bearer ' .. config.api_key,
        ['Content-Type'] = 'application/json'
    }, function(response, status)
        if status ~= 200 then
            print('[失败] DeepSeek HTTP ' .. tostring(status) .. ': ' .. tostring(response))
            if job.explicit then safe_say('连接AI失败了喵，HTTP ' .. tostring(status)) end
            return finish_job(job, '')
        end
        local data = safe_decode(response)
        local message = data and data.choices and data.choices[1] and data.choices[1].message
        if not message then
            print('[失败] DeepSeek响应解析失败: ' .. tostring(response))
            return finish_job(job, '')
        end
        local calls = message.tool_calls
        if type(calls) == 'table' and #calls > 0 and round < MAX_TOOL_ROUNDS then
            messages[#messages + 1] = message
            for _, call in ipairs(calls) do
                local fn = call['function'] or {}
                local args = safe_decode(fn.arguments or '{}') or {}
                local result = execute_tool(fn.name, args)
                print(('[工具] %s(%s) -> %s'):format(tostring(fn.name), tostring(fn.arguments), result))
                messages[#messages + 1] = { role = 'tool', tool_call_id = call.id, content = result }
            end
            return request_completion(job, messages, round + 1)
        end
        finish_job(job, message.content or '')
    end)
end

process_next = function()
    if busy or #request_queue == 0 then return end
    local job = table.remove(request_queue, 1)
    busy = true
    print(('[AI请求] %s: %s；队列剩余%d'):format(job.asker, job.prompt, #request_queue))
    request_completion(job, make_messages(job), 0)
end

local function enqueue(job)
    if #request_queue >= 6 then table.remove(request_queue, 1) end
    request_queue[#request_queue + 1] = job
    process_next()
end

local function should_auto_reply(name, text)
    local message = lower(text)
    local bot = lower(BOT_NAME)
    if message:find(bot, 1, true) or message:find('猫娘', 1, true)
        or message:find('猫猫', 1, true) or message:find('deepseek', 1, true) then return true end
    if now() - last_auto_reply < AUTO_COOLDOWN then return false end
    local question = text:find('？', 1, true) or text:find('?', 1, true)
        or message:find('谁', 1, true) or message:find('多少', 1, true)
        or message:find('比分', 1, true) or message:find('kd', 1, true)
    if not question then return false end
    return (utils.random_float and utils.random_float(0, 1) or math.random()) <= AUTO_CHANCE
end

load_global_history()

events.player_chat:set(function(e)
    local asker = tostring(e.get_username())
    local text = tostring(e.text or '')
    append_chat(asker, text, e.userid, e.teamonly)

    local prefix, prompt = text:match('^/(%a+)%s*(.*)$')
    local explicit = prefix and lower(prefix) == 'ai'
    local key = player_key(e)
    if explicit and lower(prompt) == 'clear' then
        conversations[key] = nil
        safe_say(asker .. ' 的个人AI记忆已经清除啦喵')
        return false
    end
    if explicit and prompt == '' then
        safe_say('喵~ 用法：/ai 问题；也可以直接问猫娘比分、KD或聊天记录')
        return false
    end
    -- “say”产生的猫娘回复也会触发player_chat；禁止对自己的普通消息自我回复。
    if not explicit and is_local_speaker(asker) then return end
    if not explicit and not should_auto_reply(asker, text) then return end

    enqueue({
        asker = asker, userid = e.userid or 0, key = key,
        prompt = explicit and prompt or text, explicit = explicit and true or false
    })
    if explicit then return false end
end)

events.shutdown:set(function()
    save_global_history()
end)
