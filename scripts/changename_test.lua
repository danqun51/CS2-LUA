local ffi = require('ffi')
local CSGOMainMenu = panorama.open('CSGOMainMenu')

ffi.cdef[[
typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef void* HANDLE;
BOOL VirtualProtect(void* address, SIZE_T size, DWORD new_protect, DWORD* old_protect);
BOOL FlushInstructionCache(HANDLE process, const void* address, SIZE_T size);
HANDLE GetCurrentProcess(void);
]]

-- 用纯 Lua 代替 CSNamePatch.dll：扫描并把 0x75 改成 0xEB。
local match = utils.opcode_scan('engine2.dll', '48 C1 E8 ? A8 01 75 15')

if match then
    local patch = ffi.cast('uint8_t*', match) + 6
    local patch_number = tonumber(ffi.cast('uintptr_t', patch))
    if not utils.is_readable(patch_number, 1) then
        error('改名补丁地址不可读')
    end

    if patch[0] == 0x75 then
        local old_protect = ffi.new('DWORD[1]')
        if ffi.C.VirtualProtect(patch, 1, 0x40, old_protect) == 0 then
            error('VirtualProtect 失败')
        end

        patch[0] = 0xEB
        ffi.C.FlushInstructionCache(ffi.C.GetCurrentProcess(), patch, 1)

        local unused = ffi.new('DWORD[1]')
        ffi.C.VirtualProtect(patch, 1, old_protect[0], unused)
    elseif patch[0] ~= 0xEB then
        error(('补丁字节不匹配：期望 75，实际 %02X'):format(tonumber(patch[0])))
    end
else
    print('未找到特征码，可能是已经运行过一次')
end

local function utf8_split(s)
    local chars = {}
    local idx = 1
    while idx <= #s do
        local b = s:byte(idx)
        local char_len = 1
        if b >= 0xC0 and b <= 0xDF then
            char_len = 2
        elseif b >= 0xE0 and b <= 0xEF then
            char_len = 3
        elseif b >= 0xF0 and b <= 0xF7 then
            char_len = 4
        end
        table.insert(chars, s:sub(idx, idx + char_len - 1))
        idx = idx + char_len
    end
    return chars
end

local LobbyAPI = CSGOMainMenu.LobbyAPI
local Settings = LobbyAPI.GetSessionSettings()
local mySteamID = CSGOMainMenu.MyPersonaAPI.GetXuid()
local mySteamName = "小狐狸"

--获取绝对名字
if Settings.members then
    for i = 0, Settings.members.numMachines - 1 do
        local Player = Settings.members["machine" .. i]
        if Player.id == mySteamID then
            mySteamName = Player.player0.name
            print("我的名字是: " .. mySteamName)
            break
        end
    end
end

local target = "AIMWARE"
local names = tostring(mySteamName)
local step_delay = 0.5
local char_table = utf8_split(target)
local char_count = #char_table

-- 构建名字
local function build_name(count)
    local name_str = ""
    for i = 1, count do
        name_str = name_str .. char_table[i]
    end
    return name_str
end



local function set_name(names,name_str)
    if not entity.get_local_player() or entity.get_local_player() =='nil' then
      utils.console_exec('setinfo name "' .. names .. '"')
    else
      utils.console_exec('setinfo name "' .. name_str .. " " .. names .. '"')
    end
end

-- 正向：逐渐增加字符
local function type_forward(callback)
    for i = 1, char_count do
        utils.execute_after((i - 1) * step_delay, function()
            local name_str = build_name(i)
            set_name(names,name_str)
            if i == char_count then
                callback()  -- 完成后执行反向
            end
        end)
    end
end

-- 反向：逐渐减少字符，最后清空
local function type_backward()
    for i = char_count - 1, 0, -1 do  -- 从 char_count-1 到 0
        local delay = (char_count - 1 - i) * step_delay
        utils.execute_after(delay, function()
            if i == 0 then
                -- 完全清空，只保留自己的名字
                utils.console_exec('setinfo name "' .. names .. '"')
            else
                local name_str = build_name(i)
                set_name(names,name_str)
            end
        end)
    end
    -- 循环执行
    utils.execute_after(char_count * step_delay, function()
        type_forward(function()
            utils.execute_after(step_delay, type_backward)
        end)
    end)
end

-- 开始循环
type_forward(function()
    utils.execute_after(step_delay, type_backward)
end)
