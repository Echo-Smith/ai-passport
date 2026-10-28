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

## 二期：与审批桥联动（未做）

控制类工具（turn_on/execute_scene 等）目前直连米家云、不经过 AI Passport 审批。
二期思路：在 bridge.py 加 mihome 适配器，代理 MCP 控制调用 → 设备弹窗审批 → 放行/拒绝，
与 codex/zcode 适配器同构；或利用 Magpie 的 MCP 审批链路。物理设备操作建议默认要求审批。
