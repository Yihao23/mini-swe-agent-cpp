"""一个"较真"的 MCP server：没收到 notifications/initialized 之前不干活。

存在的理由：mock_mcp_server.py 不等这条通知就开始服务，所以客户端里
`notify("notifications/initialized", ...)` 那一句删掉了也没人发现 ——
测试全绿，而协议已经被违反了。

⚠️ 一处刻意的不忠实：真实的 server 多半是**不答**，症状是客户端卡到超时。
   这里改成回一条 JSON-RPC error，是为了让用例在毫秒级完成而不是等满 15 秒。
   代价是测的东西略有偏移：测的是"客户端确实发了那条通知"，
   而不是"不发会卡死"。
"""

import json
import sys

TOOLS = [{
    "name": "echo",
    "description": "原样返回传入的文本",
    "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}},
                    "required": ["text"]},
}]


def reply(id_, payload):
    sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": id_, **payload}) + "\n")
    sys.stdout.flush()


def main():
    initialized = False
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        method, id_ = msg.get("method"), msg.get("id")

        if method == "initialize":
            reply(id_, {"result": {"protocolVersion": "2024-11-05",
                                   "capabilities": {"tools": {}},
                                   "serverInfo": {"name": "strict", "version": "0.1"}}})
        elif method == "notifications/initialized":
            initialized = True          # 通知没有 id，不回任何东西
        elif not initialized:
            # 握手没走完就来要东西 —— 真实 server 会沉默，这里说清楚好测
            reply(id_, {"error": {"code": -32002,
                                  "message": "握手未完成：没收到 notifications/initialized"}})
        elif method == "tools/list":
            reply(id_, {"result": {"tools": TOOLS}})
        elif method == "tools/call":
            args = msg["params"].get("arguments", {})
            reply(id_, {"result": {"content": [{"type": "text",
                                                "text": f"strict: {args.get('text', '')}"}],
                                   "isError": False}})
        elif id_ is not None:
            reply(id_, {"result": {}})


if __name__ == "__main__":
    main()
