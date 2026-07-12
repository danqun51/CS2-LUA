print('========== network 异步 API 测试 ==========', CS2LUA_API_VERSION)

local started = os.clock()
local queued = network.get('https://httpbin.org/get', {['X-CS2Lua-Test']='get'}, function(response, status)
    if status == 200 and type(response)=='string' and #response>0 then
        print('[成功] network.get 异步回调功能正常，HTTP '..status)
    else
        print('[失败] network.get: HTTP '..tostring(status)..' '..tostring(response))
    end
end)

if queued and os.clock()-started < 0.1 then
    print('[成功] network.get 非阻塞提交功能正常')
else
    print('[失败] network.get 提交发生阻塞')
end

started = os.clock()
queued = network.post('https://httpbin.org/post', {message='hello',value=123}, {}, function(response,status)
    if status == 200 and type(response)=='string' and #response>0 then
        print('[成功] network.post JSON 异步回调功能正常，HTTP '..status)
    else
        print('[失败] network.post: HTTP '..tostring(status)..' '..tostring(response))
    end
end)

if queued and os.clock()-started < 0.1 then
    print('[成功] network.post 非阻塞提交功能正常')
else
    print('[失败] network.post 提交发生阻塞')
end

print('[等待] HTTP 请求在工作线程运行，结果将通过插件 tick 回到 Lua callback')
