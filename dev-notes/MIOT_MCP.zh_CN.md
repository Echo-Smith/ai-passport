# 米家 MCP 接入笔记（2026-10-28）

选型：[javen-yan/miot-mcp](https://github.com/javen-yan/miot-mcp)（产品化米家 MCP server，基于 mijiaAPI 3.x，
面向"家/房间/设备名"自然语言路由，26 个工具）。clone 于 `/Users/marshecho/Codex/AI-Passport/miot-mcp`。

## 环境（已装好，可直接用）

- Python 3.12 venv：`/Users/marshecho/Codex/AI-Passport/miot-mcp/.venv`（uv 创建；系统 python 3.9 太老不可用）
- **坑：requirements.txt 的 `mcp>=1.13.0` 会装到 2.x，2.x 移除了 fastmcp 模块导致启动即崩。
  必须降级：`uv pip install --python .venv/bin/python "mcp<2"`（已执行）**
- 登录态：`~/.miot-mcp/auth_data.json`（已登录并验证：家庭「巷巷的栖息地」2 房间 11 设备全部在线可读）
- 二维码登录产物：`~/.miot-mcp/qr.png` / `qr.html`；日志 `~/.miot-mcp/run.log`

## 客户端配置片段（stdio MCP）

```json
{
  "mcpServers": {
    "mijia": {
      "command": "/Users/marshecho/Codex/AI-Passport/miot-mcp/.venv/bin/python",
      "args": ["/Users/marshecho/Codex/AI-Passport/miot-mcp/mcp_server/mcp_server.py"],
      "env": { "MIJIA_ENABLE_QR": "true", "MIJIA_QR_OPEN_MODE": "browser", "MIJIA_LOG_LEVEL": "INFO" }
    }
  }
}
```

登录过期时：agent 调 `prepare_login`（`force_reauth:true`）→ 浏览器自动弹二维码 → 扫码即可。

## 快速验证（无需客户端）

```bash
# 列工具/拉设备概览的独立脚本（stdio MCP 直连）
/Users/marshecho/Codex/AI-Passport/miot-mcp/.venv/bin/python /tmp/miot_call.py get_home_overview
```
（`/tmp/miot_call.py` 为临时脚本，会话清理后按需重建；正式验证用客户端配置。）

## 一期已落地：审批代理（approval-miot-mcp，2026-10-28 端到端走通）

`approval-bridge/miot-gateway/approval_miot_mcp.py`——米家控制审批代理 MCP：

- **架构**：agent 客户端 → 本代理（FastMCP 子类覆写 list_tools/call_tool）→ miot-mcp 子进程（ClientSession 长连）→ 米家云；控制类调用先经 hub UDS（`hub.sock`，`{"type":"request",...}` 阻塞等 result）
- **策略**：默认拒绝制——12 个查询工具直读，其余（控制/场景/账号操作）一律设备弹窗；`agent=mihome`，title 带中文设备名（设备字库已扩 30 字），超时 60s fail-safe=deny
- **踩坑记录**：① mcp 1.30 的 FastMCP 无装饰器注册 API，须子类覆写 `list_tools/call_tool`；② 下游工具带 outputSchema，转发必须同时返回 `tuple(content, structuredContent)`，否则客户端报 "outputSchema defined but no structured output returned"
- **端到端证据**：`turn_on_device 台灯` → 设备弹窗 → OK → hub 日志 `决定 req-…→ allow (device)` → 米家云执行成功
- **客户端接入**：把 mcpServers 配置的 args 换成本代理即可（工具语义与 miot-mcp 完全一致，26 个）：
  ```json
  {"mcpServers":{"mijia":{"command":"/Users/marshecho/Codex/AI-Passport/miot-mcp/.venv/bin/python",
    "args":["/Users/marshecho/Codex/AI-Passport/approval-bridge/miot-gateway/approval_miot_mcp.py"]}}}
  ```

## 二期（已拍板）：底座迁移 miloco-miot

用官方 `pip install miloco-miot`（xiaomi-miloco 的独立子包，异步 Python 客户端 + 局域网
mTLS 控制）替换 miot-mcp 后端，审批代理壳与工具语义保持不变；整装 miloco（摄像头感知/
家庭记忆/主动智能，需 OpenClaw 运行时 + MiMo API key）留给三期。迁移前需探明其登录流程。
