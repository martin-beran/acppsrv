-- Lua script for wrk. It generates random lookups into the database

body0 = '{"db":"key_value","query":"get","args":[{"v_int64":'
body1 = "}]}"

function request()
    body = body0 .. math.random(100000000) .. body1
    return "GET /db HTTP/1.1\r\
Content-Type: application/json\r\
Content-Length: " .. #body .. "\r\n\r\n" .. body
end