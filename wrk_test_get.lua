-- Lua script for wrk. It generates random lookups into the database

body0 = '{"db":"key_value","query":"get","args":[{"v_int64":'
-- body0 = '{"db":"key_value","query":"get_val","args":[{"v_int64":'
body0 = '{"db":"key_value","query":"update","args":[{"v_int64":'
body1 = '}],"retry_if_locked":true}'
key_range = 100000000
-- key_range = 1000

function request()
    body = body0 .. math.random(key_range) .. body1
    return "GET /db HTTP/1.1\r\
Content-Type: application/json\r\
Content-Length: " .. #body .. "\r\n\r\n" .. body
end
