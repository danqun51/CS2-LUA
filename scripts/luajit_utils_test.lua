print('========== LuaJIT / FFI / BIT / utils 测试开始 ==========', CS2LUA_API_VERSION)

local passed, failed = 0, 0
local function test(name, callback)
    local ok, result = pcall(callback)
    if ok and result ~= false then
        passed = passed + 1
        print('[成功] ' .. name .. ' 功能正常')
    else
        failed = failed + 1
        print('[失败] ' .. name .. ' 功能异常: ' .. tostring(result))
    end
end

test('LuaJIT', function()
    assert(type(jit) == 'table' and type(jit.version) == 'string')
    print('LuaJIT version: ' .. jit.version)
end)

test('FFI cdef/sizeof/new/cast', function()
    local native_ffi = ffi or require('ffi')
    native_ffi.cdef[[typedef struct { int x; float y; } cs2lua_test_t;]]
    local value = native_ffi.new('cs2lua_test_t')
    value.x, value.y = 123, 4.5
    assert(value.x == 123 and native_ffi.sizeof(value) == 8)
    local pointer = native_ffi.cast('cs2lua_test_t*', value)
    assert(pointer[0].x == 123)
end)

test('BIT BitOp', function()
    assert(bit.band(7, 3) == 3)
    assert(bit.bor(4, 1) == 5)
    assert(bit.bxor(7, 3) == 4)
    assert(bit.lshift(1, 4) == 16)
    assert(bit.tohex(255) == '000000ff')
end)

test('utils random', function()
    utils.random_seed(527)
    local i = utils.random_int(1, 6)
    local f = utils.random_float(0, 1)
    assert(i >= 1 and i <= 6 and f >= 0 and f <= 1)
end)

test('utils console_exec', function()
    assert(utils.console_exec('echo CS2LuaPlugin utils test') ~= false)
end)

test('utils execute_after', function()
    utils.execute_after(1, function()
        print('[成功] utils.execute_after 延迟回调功能正常')
    end)
end)

test('utils create_interface', function()
    local pointer = utils.create_interface('engine2.dll', 'InputService_001')
    assert(pointer ~= nil)
end)

test('utils opcode_scan', function()
    local pointer = utils.opcode_scan('kernel32.dll', '4D 5A')
    assert(pointer ~= nil)
end)

test('utils get_vfunc 构造', function()
    local wrapper = utils.get_vfunc(0, 'void(*)(void*)')
    assert(type(wrapper) == 'function')
end)

test('utils trace 兼容结构', function()
    local trace = utils.trace_line(vector(), vector(100, 0, 0))
    assert(type(trace) == 'table' and type(trace.did_hit) == 'function')
end)

test('utils net_channel 兼容结构', function()
    local net = utils.net_channel()
    assert(type(net) == 'table' and type(net.get_server_info) == 'function')
end)

print(('========== 测试完成：成功 %d，失败 %d =========='):format(passed, failed))
