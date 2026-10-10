"""Protocol smoke test against the real official MCP Python SDK."""
import anyio
import json
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[3]


async def run():
    params = StdioServerParameters(
        command="python",
        args=[str(ROOT / "tools/MaaMcp/server.py")],
        cwd=ROOT,
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await session.list_tools()
            names = {tool.name for tool in tools.tools}
            assert {"observe_state", "observe_roguelike_state", "observe_tasks", "battle_action"} <= names
            result = await session.call_tool("observe_state", {})
            observed = result.structuredContent or json.loads(result.content[0].text)
            assert observed["status"] == "not_configured"
            denied = await session.call_tool("battle_action", {"request": {
                "kind": "pause", "expected_state_hash": "0" * 64,
                "idempotency_key": "smoke", "timeout_ms": 1000,
                "safety_gate": False, "preconditions": {"connected": True, "running": False},
                "payload": {},
            }})
            denied_value = denied.structuredContent or json.loads(denied.content[0].text)
            assert denied_value["status"] == "safety_denied"


if __name__ == "__main__":
    anyio.run(run)
