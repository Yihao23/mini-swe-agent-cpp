"""一个 30 行的 MCP server，用来验证 mcp.py 的握手是真的能跑通。

协议：JSON-RPC 2.0，一行一条消息，走 stdin/stdout。
"""

import json
import sys

TOOLS = [{
    "name": "echo",
    "description": "原样返回传入的文本",
    "inputSchema": {
        "type": "object",
        "properties": {"text": {"type": "string"}},
        "required": ["text"],
    },
}]


def reply(id_, result):
    sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": id_, "result": result}) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        method, id_ = msg.get("method"), msg.get("id")

        if method == "initialize":
            # 先发一条通知，专门测试客户端会不会被它带偏
            sys.stdout.write(json.dumps(
                {"jsonrpc": "2.0", "method": "notifications/message",
                 "params": {"level": "info", "data": "booting"}}) + "\n")
            sys.stdout.flush()
            reply(id_, {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
                        "serverInfo": {"name": "mock", "version": "0.1"}})
        elif method == "tools/list":
            reply(id_, {"tools": TOOLS})
        elif method == "tools/call":
            args = msg["params"].get("arguments", {})
            reply(id_, {"content": [{"type": "text", "text": f"echo: {args.get('text', '')}"}],
                        "isError": False})
        elif id_ is not None:
            reply(id_, {})


if __name__ == "__main__":
    main()
