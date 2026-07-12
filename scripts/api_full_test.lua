-- CS2LuaPlugin full compatibility smoke test.
-- Copy to: Counter-Strike Global Offensive/game/bin/win64/lua/

local passed, failed = 0, 0
local function test(name, fn)
    local ok, result = pcall(fn)
    if ok and result ~= false then
        passed = passed + 1
        print(('[成功] %s 功能正常'):format(name))
    else
        failed = failed + 1
        print(('[失败] %s 功能异常: %s'):format(name, tostring(result)))
    end
end

print('========== CS2LuaPlugin API 全功能测试开始 ==========')
print('API version:', tostring(CS2LUA_API_VERSION))

test('CS2 控制台 print', function() print('print 测试文本 123') end)
test('vector 构造与运算', function()
    local a, b = vector(3,4,0), vector(1,2,3)
    assert(a:length() == 5)
    assert((a+b).x == 4 and a:dot(b) == 11)
end)
test('color 构造与转换', function()
    local c = color(255,128,64,200)
    assert(c:to_hex() == 'FF8040C8')
    assert(c:alpha_modulate(100).a == 100)
end)
test('bit 位运算', function() assert(bit.band(0xF,0x3)==3 and bit.bor(1,2)==3) end)
test('globals', function() assert(type(globals.tickcount)=='number' and globals.tickinterval>0) end)
test('common 日期时间', function()
    assert(type(common.get_unixtime())=='number' and type(common.get_timestamp())=='number')
    local t=common.get_system_time(); assert(type(t.year)=='number' and type(common.get_date('%Y'))=='string')
end)
test('utils.random', function()
    utils.random_seed(12345); local n=utils.random_int(1,10); assert(n>=1 and n<=10)
    local f=utils.random_float(0,1); assert(f>=0 and f<=1)
end)
test('utils.create_interface', function() assert(utils.create_interface('engine2.dll','InputService_001') ~= nil) end)
test('utils.opcode_scan', function() assert(utils.opcode_scan('engine2.dll','4D 5A') ~= nil) end)
test('utils.console_exec', function() utils.console_exec('echo CS2LuaPlugin console_exec success') end)
test('utils.trace_line 兼容结构', function()
    local tr=utils.trace_line(vector(),vector(100,0,0)); assert(tr.fraction==1 and tr:is_visible())
end)
test('utils.net_channel 兼容结构', function() local n=utils.net_channel(); assert(type(n.latency[0])=='number') end)
test('schema/offsets', function()
    assert(type(schema.get_build_number())=='number')
    assert(type(schema.get_offset('client.dll','dwLocalPlayerPawn'))=='number')
end)
test('json encode/decode', function()
    local encoded=json.encode({name='test',value=123,active=true})
    local decoded=json.decode(encoded); assert(decoded.name=='test' and decoded.value==123 and decoded.active==true)
end)
test('json Unicode decode', function()
    local decoded=json.decode('{"text":"\\u732b\\u5a18 \\ud83d\\udc31"}')
    assert(decoded.text=='猫娘 🐱')
end)
test('db 持久化存储', function() db.write('api_test',123); assert(db.read('api_test')==123); db.delete('api_test') end)

local temp='cs2lua_api_test.tmp'
local renamed='cs2lua_api_test_renamed.tmp'
test('files 读写', function() files.write(temp,'hello'); assert(files.read(temp)=='hello'); assert(files.size(temp)==5) end)
test('files 重命名', function() assert(files.rename(temp,renamed)); assert(files.exists(renamed)) end)
test('files 删除', function() assert(files.remove(renamed)) end)
test('files 目录操作', function()
    assert(files.create_directory('cs2lua_test_dir'))
    local list=files.list('.'); assert(type(list)=='table')
    files.remove('cs2lua_test_dir')
end)

test('events.tick set/call/unset', function()
    local called=false; events.tick:set(function(v) called=(v==123) end); events.tick:call(123); events.tick:unset(); assert(called)
end)
test('events.player_hurt 参数传递', function()
    local damage=0; events.player_hurt:set(function(e) damage=e.dmg_health end)
    events.player_hurt:call({dmg_health=88}); events.player_hurt:unset(); assert(damage==88)
end)
test('events.player_chat 参数传递', function()
    local text,name
    events.player_chat:set(function(e) text=e.text; name=e.get_username() end)
    events.player_chat:call({text='/roll',get_username=function() return 'tester' end})
    events.player_chat:unset(); assert(text=='/roll' and name=='tester')
end)
test('utils.execute_after', function()
    utils.execute_after(1.0,function() print('[成功] utils.execute_after 延迟执行功能正常') end)
end)

print(('========== 同步测试完成：成功 %d，失败 %d =========='):format(passed,failed))
print('execute_after 将在 1 秒后另行输出结果；player_hurt 游戏原生触发需进入本地对局测试。')
