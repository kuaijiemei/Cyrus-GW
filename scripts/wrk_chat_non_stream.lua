-- ---------------------------------------------------------------------------
-- 模块职责：为 wrk 提供固定的 /chat 非流式 POST 请求模板。
-- 对外暴露：统一 method/header/body，避免每次压测手写命令导致输入不一致。
-- 易踩坑：body 使用固定 JSON 字符串，减少 shell 转义差异造成的请求体污染。
-- ---------------------------------------------------------------------------

wrk.method = "POST"
wrk.path = "/chat"
wrk.headers["Content-Type"] = "application/json"

local body = [[{"message":"bench non-stream","stream":false}]]

request = function()
    return wrk.format(nil, nil, nil, body)
end
