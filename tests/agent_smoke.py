#!/usr/bin/env python3
"""Smoke test for the agent bridge. Start the app first (Debug build), then run this.

Env: AGENT_PORT (default 7878), AGENT_TOKEN (if the app was started with one).
Needs the Agent Playground window and the reset_playground / set_theme commands, as main.cpp provides.
Restores the theme and resets the Playground when it finishes, even if a check fails.
"""
import base64
import json
import os
import sys
import urllib.error
import urllib.request

PORT = int(os.environ.get("AGENT_PORT", "7878"))
TOKEN = os.environ.get("AGENT_TOKEN", "")
URL = f"http://127.0.0.1:{PORT}/mcp"
PG = "Agent Playground"
failures = []


def post(body, headers=None, use_token=True):
    h = {"Content-Type": "application/json"}
    if TOKEN and use_token:
        h["Authorization"] = f"Bearer {TOKEN}"
    h.update(headers or {})
    req = urllib.request.Request(URL, json.dumps(body).encode(), h)
    try:
        with urllib.request.urlopen(req, timeout=40) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, None


def rpc(method, params=None):
    return post({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}})[1]["result"]


def call(tool, **args):
    res = rpc("tools/call", {"name": tool, "arguments": args})
    text = next(c["text"] for c in res["content"] if c["type"] == "text")
    return res, (json.loads(text) if not res["isError"] else text)


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail and not ok else ""))
    if not ok:
        failures.append(name)


ping = {"jsonrpc": "2.0", "id": 1, "method": "ping"}
check("rejects foreign Host", post(ping, {"Host": "evil.example.com"})[0] == 403)
check("rejects other localhost origin", post(ping, {"Origin": "http://localhost:9999"})[0] == 403)
if TOKEN:
    check("rejects missing token", post(ping, use_token=False)[0] == 401)

check("initialize", rpc("initialize", {"protocolVersion": "2025-06-18"})["serverInfo"]["name"] == "imagent")
names = {t["name"] for t in rpc("tools/list")["tools"]}
check("tools listed", {"click", "type_text", "get_ui_tree", "screenshot", "describe_item"} <= names, str(names))

_, info = call("get_app_info")
check("app info", info["frame"] > 0 and info["items"] > 0)
_, text = call("get_text", query="Agent Playground")
check("get_text sees drawn text (imgui patch present)", len(text["texts"]) > 0)


def counter():
    return call("get_state", name="playground")[1]["counter"]


before = counter()
_, clicked = call("click", label="+1", window=PG, speed="instant")
check("click hits the target", clicked.get("hit") is True, json.dumps(clicked)[:200])
check("click changed app state", counter() == before + 1)
call("click", label="-1", window=PG, speed="instant")  # restore

_, typed = call("type_text", label="Name", window=PG, text="Ada", replace=True, speed="instant")
call("click", label="Greet", window=PG, speed="instant")
_, waited = call("wait_for", text="Hello, Ada!", timeout_frames=60)
check("type_text + button updated the status", waited.get("ok") is True)
check("typed text reached app state", call("get_state", name="playground")[1]["name"] == "Ada")

res, _ = call("screenshot", scale=0.5)
png = base64.b64decode(next(c["data"] for c in res["content"] if c["type"] == "image"))
check("screenshot is a PNG", png[:8] == b"\x89PNG\r\n\x1a\n")

res, err = call("scroll", dy=1e9, speed="instant")
check("huge scroll rejected", res["isError"], str(err)[:80])
res, err = call("type_text", text="x" * 5000, speed="instant")
check("huge text rejected", res["isError"], str(err)[:80])
res, err = call("screenshot", region={"x": 1e30, "y": 1e30, "w": 1e30, "h": 1e30})
check("absurd screenshot region handled", res["isError"], str(err)[:80])
res, err = call("click", id="100000001", speed="instant")
check("out-of-range id rejected", res["isError"], str(err)[:80])

# --- change summaries, idempotent clicks, batching, app commands, lint ---
original_theme = call("get_state", name="theme")[1]["name"]
try:
    call("run_command", name="reset_playground")
    _, clicked = call("click", label="+1", window=PG, speed="instant")
    added = json.dumps(clicked.get("changes", {}).get("text", {}).get("added", []))
    check("click reports what changed", "Count: 1" in added, added)

    call("click", label="Lint samples", window=PG, ensure="open", speed="instant")
    _, second = call("click", label="Lint samples", window=PG, ensure="open", speed="instant")
    check("ensure=open is idempotent", second.get("skipped") == "already open", json.dumps(second)[:120])

    res, batch = call(
        "run_steps",
        steps=[
            {"tool": "run_command", "args": {"name": "reset_playground"}},
            {"tool": "click", "args": {"label": "Lint samples", "window": PG, "ensure": "open", "speed": "instant"}},
            {"tool": "type_text", "args": {"label": "Name", "window": PG, "text": "Batch", "replace": True, "speed": "instant"}},
            {"tool": "click", "args": {"label": "Greet", "window": PG, "speed": "instant"}},
            {"tool": "wait_for", "args": {"text": "Hello, Batch!", "timeout_frames": 60}},
        ],
    )
    check("run_steps completes every step", not res["isError"] and batch["completed"] == 5, json.dumps(batch)[:200])
    res, failed = call("run_steps", steps=[{"tool": "click", "args": {"label": "no such widget"}}, {"tool": "key", "args": {"key": "Escape"}}])
    check("run_steps stops at the first failure", res["isError"] and '"completed":0' in failed, str(failed)[:160])

    _, lint = call("ui_lint", window=PG)
    rules = {i["rule"] for i in lint["issues"]}
    check("lint finds the planted problems", {"duplicate-id", "overlap", "low-contrast", "tiny-target"} <= rules, str(rules))

    _, commands = call("list_commands")
    check("commands listed", {"set_theme", "reset_playground"} <= {c["name"] for c in commands["commands"]})
    other_theme = "slate-light" if original_theme != "slate-light" else "zinc-dark"
    call("run_command", name="set_theme", args={"name": other_theme})
    check("run_command changed the app", call("get_state", name="theme")[1]["name"] == other_theme)
    res, _ = call("run_command", name="set_theme", args={"name": "nope"})
    check("bad command args reported", res["isError"])
finally:
    call("click", label="Lint samples", window=PG, ensure="closed", speed="instant")
    call("run_command", name="set_theme", args={"name": original_theme})
    call("run_command", name="reset_playground")

print("\nAll checks passed." if not failures else f"\n{len(failures)} check(s) failed: {failures}")
sys.exit(1 if failures else 0)
