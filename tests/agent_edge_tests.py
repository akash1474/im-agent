#!/usr/bin/env python3
"""Broad end-to-end tests for the agent bridge, driven through MCP like an agent would.

Start the Debug app first (it shows the Agent Playground, Edge Cases and Offscreen Window).
Env: AGENT_PORT (default 7878), AGENT_TOKEN. Usage: agent_edge_tests.py [substring of test names to run]
The tests change app state (themes, counters, windows); restart the app afterwards if you care.
"""
import base64
import json
import os
import struct
import sys
import threading
import time
import traceback
import urllib.error
import urllib.request
import zlib

PORT = int(os.environ.get("AGENT_PORT", "7878"))
TOKEN = os.environ.get("AGENT_TOKEN", "")
URL = f"http://127.0.0.1:{PORT}/mcp"
EDGE = "Edge Cases"
THEMES = [f"{base}-{mode}" for base in ("neutral", "zinc", "stone", "slate") for mode in ("light", "dark")]


def http(data, headers=None, method="POST", timeout=60):
    h = {"Content-Type": "application/json"}
    if TOKEN:
        h["Authorization"] = f"Bearer {TOKEN}"
    h.update(headers or {})
    req = urllib.request.Request(URL, data, h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def rpc(method, params=None, rid=1, timeout=60):
    status, body = http(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}).encode(), timeout=timeout)
    return json.loads(body)


def tool(_name, /, timeout=60, **args):
    """Returns (is_error, payload); payload is parsed JSON when possible, else text."""
    res = rpc("tools/call", {"name": _name, "arguments": args}, timeout=timeout)["result"]
    items = res["content"]
    if any(c["type"] == "image" for c in items):
        return res["isError"], res
    text = next(c["text"] for c in items if c["type"] == "text")
    try:
        return res["isError"], json.loads(text)
    except ValueError:
        return res["isError"], text


def ok(_name, /, **args):
    err, data = tool(_name, **args)
    if err:
        raise AssertionError(f"{_name}({args}) failed: {str(data)[:300]}")
    return data


def fails(_name, /, contains=None, **args):
    err, data = tool(_name, **args)
    if not err:
        raise AssertionError(f"{_name}({args}) should have failed but returned {str(data)[:200]}")
    if contains and contains.lower() not in str(data).lower():
        raise AssertionError(f"{_name}({args}) failed with {str(data)[:200]!r}, expected {contains!r}")
    return data


def state(name):
    return ok("get_state", name=name)


def edge():
    return state("edge")


def items(query="", window=EDGE, **kw):
    return ok("find_items", query=query, window=window, visible_only=kw.pop("visible_only", False), limit=kw.pop("limit", 100), **kw)["items"]


def one(query, window=EDGE):
    found = [i for i in items(query, window) if query.lower() in i.get("label", "").lower()]
    assert found, f"no item labeled {query!r}"
    return found[0]


def texts(query="", window=""):
    return ok("get_text", query=query, window=window, visible_only=False, limit=200)["texts"]


def have_text(query, window=""):
    return any(query in t["text"] for t in texts(query, window))


def section(label, state_open=True):
    ok("click", label=label, window=EDGE, ensure="open" if state_open else "closed", speed="instant")


def step_frames(n=3):
    ok("step_frames", frames=n)


def png_size(result):
    data = base64.b64decode(next(c["data"] for c in result["content"] if c["type"] == "image"))
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    return struct.unpack(">II", data[16:24])


def close_size(actual, expected, tolerance=2):
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def png_pixels(result):
    """Decodes the 8-bit RGB PNG of a screenshot result into (width, height, bytes)."""
    data = base64.b64decode(next(c["data"] for c in result["content"] if c["type"] == "image"))
    width, height = struct.unpack(">II", data[16:24])
    assert data[24] == 8 and data[25] == 2, "expected 8-bit RGB"
    raw, pos = b"", 8
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IDAT":
            raw += data[pos + 8:pos + 8 + length]
        pos += 12 + length
    raw, stride, rows = zlib.decompress(raw), width * 3, []
    prev = bytearray(stride)
    for y in range(height):
        ft, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b, c = prev[i], (prev[i - 3] if i >= 3 else 0)
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return width, height, b"".join(rows)


def differing_pixels(a, b, region=None):
    """Pixels that differ between two decoded screenshots; region = (x, y, w, h) in pixels restricts the comparison."""
    width, height = a[0], a[1]
    x0, y0, w, h = region or (0, 0, width, height)
    x1, y1 = min(width, int(x0 + w)), min(height, int(y0 + h))
    count = 0
    for y in range(max(0, int(y0)), y1):
        row = y * width * 3
        for x in range(max(0, int(x0)), x1):
            if a[2][row + x * 3:row + x * 3 + 3] != b[2][row + x * 3:row + x * 3 + 3]:
                count += 1
    return count


results = []
FILTER = sys.argv[1] if len(sys.argv) > 1 else ""


def cleanup():
    """Closes every header and popup so one failed test cannot poison the next."""
    try:
        steps = [{"tool": "key", "args": {"key": "Escape", "speed": "instant"}}]
        steps += [{"tool": "click", "args": {"label": h, "window": EDGE, "ensure": "closed", "speed": "instant"}} for h in HEADERS]
        tool("run_steps", steps=steps, stop_on_error=False)
    except Exception:  # noqa: BLE001
        pass


HEADERS = ["Scrolling child", "Table", "Tabs and nesting", "Inputs", "Awkward buttons", "Popups and drag and drop", "Timing", "Stress: 3000 widgets"]


def test(fn):
    if FILTER and FILTER not in fn.__name__:
        return fn
    started = time.time()
    try:
        fn()
        results.append((fn.__name__, None))
        print(f"PASS {fn.__name__} ({time.time() - started:.1f}s)")
    except Exception as e:  # noqa: BLE001 - report everything, keep going
        detail = "".join(traceback.format_exception_only(type(e), e)).strip()
        results.append((fn.__name__, detail))
        print(f"FAIL {fn.__name__}: {detail[:400]}")
    if not fn.__name__.startswith(("proto_", "setup_")):
        cleanup()
    return fn


def reset():
    ok("run_command", name="reset_layout")
    ok("run_command", name="reset_playground")
    ok("run_command", name="reset_edge_cases")
    step_frames(3)


# ---------------------------------------------------------------- protocol


@test
def proto_garbage_and_recovery():
    statuses = [http(b"{not json")[0], http(b"")[0], http(b"[]")[0], http(b'{"jsonrpc":"2.0","id":1}')[0]]
    assert all(s in (200, 202, 400) for s in statuses), statuses
    assert rpc("ping")["result"] == {}


@test
def proto_unknown_method_and_notification():
    assert rpc("no/such/method")["error"]["code"] == -32601
    status, body = http(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}).encode())
    assert status == 202 and not body.strip(), (status, body)
    assert http(b"", method="GET")[0] == 405
    assert rpc("ping")["result"] == {}


@test
def proto_oversized_body():
    status, _ = http(b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":"' + b"a" * (3 * 1024 * 1024) + b'"}}')
    assert status in (200, 400, 413), status
    assert rpc("ping")["result"] == {}


@test
def proto_bad_tool_calls():
    def rejected(reply):
        return "error" in reply or reply["result"]["isError"]

    assert rejected(rpc("tools/call", {"name": "no_such_tool", "arguments": {}}))
    assert rejected(rpc("tools/call", {}))
    assert rejected(rpc("tools/call", {"name": "click", "arguments": "not an object"}))
    assert rejected(rpc("tools/call", {"name": "click", "arguments": {"label": 12345}}))
    assert rpc("ping")["result"] == {}


@test
def proto_many_sequential_requests():
    started = time.time()
    for i in range(300):
        rpc("ping", rid=i)
    assert time.time() - started < 20, "300 pings took too long"


@test
def proto_concurrent_blocked_calls_do_not_hang():
    outcomes = []

    def worker():
        try:
            err, data = tool("wait_for", text="zzz-never-appears", timeout_frames=30, timeout=60)
            outcomes.append(("err" if err else "ok", str(data)[:80]))
        except Exception as e:  # noqa: BLE001
            outcomes.append(("exc", str(e)[:80]))

    threads = [threading.Thread(target=worker) for _ in range(8)]
    [t.start() for t in threads]
    [t.join(70) for t in threads]
    assert len(outcomes) == 8, f"only {len(outcomes)} of 8 returned"
    assert all(kind == "err" for kind, _ in outcomes), outcomes
    busy = [msg for _, msg in outcomes if "too many" in msg.lower()]
    timed_out = [msg for _, msg in outcomes if "timed out" in msg.lower()]
    assert busy, "the 4-call cap never kicked in"
    assert len(busy) + len(timed_out) == 8, outcomes
    assert rpc("ping")["result"] == {}


@test
def proto_abandoned_calls_free_their_slots():
    for _ in range(6):
        try:
            tool("wait_for", text="zzz-never-appears", timeout_frames=3000, timeout=1)
        except Exception:  # noqa: BLE001 - client-side timeout is the point
            pass
    err, data = tool("get_app_info", timeout=15)
    assert not err, f"bridge still blocked after abandoned calls: {data}"


# ---------------------------------------------------------------- setup


@test
def setup_state():
    reset()
    ok("click", label="Show ImGui demo window", window="Agent Playground", ensure="unchecked", speed="instant")
    names = {w["name"] for w in ok("get_windows")["windows"]}
    assert {"Agent Playground", EDGE, "Offscreen Window"} <= names, names
    info = ok("get_app_info")
    assert info["items"] > 0 and info["fps"] > 0


# ---------------------------------------------------------------- read tools


@test
def read_tree_flat_and_nested_agree():
    section("Table")
    flat = ok("get_ui_tree", window=EDGE, limit=500, visible_only=False)
    nested = ok("get_ui_tree", window=EDGE, limit=500, visible_only=False, nested=True)

    def count(node):
        return 1 + sum(count(c) for c in node.get("children", []))

    flat_n = sum(len(w["items"]) for w in flat["windows"])
    nested_n = sum(sum(count(n) for n in w["items"]) for w in nested["windows"])
    assert flat_n > 20 and flat_n == nested_n, (flat_n, nested_n)
    section("Table", False)


@test
def read_find_items_filters():
    assert any(i["label"] == "File" for i in items("File"))
    assert not items("zzz no such label zzz")
    assert len(items("", limit=3)) <= 3
    assert len(items("", limit=1)) == 1
    by_id = items("", id=one("File")["id"])
    assert len(by_id) == 1 and by_id[0]["label"] == "File"
    assert all(i["kind"] == "openable" for i in items("", kind="openable"))
    assert len(ok("find_items", limit=-5)["items"]) == 1  # clamped to 1
    ok("find_items", limit=10 ** 9)
    ok("find_items", id="not-hex")  # an unparsable id is ignored, not an error


@test
def read_get_text_has_style():
    t = texts("Last key", EDGE)
    assert t and t[0]["size"] > 0 and t[0]["color"].startswith("#") and t[0]["font"]


@test
def read_hover_and_state():
    ok("hover", label="+1", window="Agent Playground", speed="instant")
    h = ok("get_hovered")
    assert h["hovered"] and h["hovered"]["label"] == "+1", h
    assert "counter" in state("playground")
    fails("get_state", name="nope")
    assert "lines" in ok("get_log", lines=5)


@test
def read_step_frames_and_commands():
    f0 = ok("get_app_info")["frame"]
    step_frames(10)
    assert ok("get_app_info")["frame"] >= f0 + 10
    ok("step_frames", frames=0)
    ok("step_frames", frames=10 ** 9)  # clamped, must return
    cmds = {c["name"] for c in ok("list_commands")["commands"]}
    assert {"set_theme", "reset_playground", "add_todos", "reset_edge_cases"} <= cmds


@test
def read_screenshots():
    full = tool("screenshot", scale=1.0)[1]
    w, h = png_size(full)
    assert (w, h) == tuple(ok("get_app_info")["displaySize"]), (w, h)
    w2, h2 = png_size(tool("screenshot", scale=0.5)[1])
    assert abs(w2 - 640) <= 1
    region = tool("screenshot", scale=1.0, region={"x": 20, "y": 20, "w": 300, "h": 200})[1]
    assert png_size(region) == (300, 200)
    assert not tool("screenshot", annotate=True)[0]
    assert not tool("screenshot", scale=0.0001)[0]  # clamped
    assert tool("screenshot", region={"x": 0, "y": 0, "w": -50, "h": 10})[0]
    assert tool("screenshot", region={"x": 5000, "y": 5000, "w": 10, "h": 10})[0]
    png_size(tool("screenshot", scale=100)[1])  # clamped to the framebuffer scale


@test
def screenshot_with_action_keeps_hover_state():
    section("Popups and drag and drop")
    ok("hover", label="Open outer", window=EDGE, speed="instant")
    step_frames(5)
    baseline = png_pixels(tool("screenshot", scale=1.0)[1])
    x, y, w, h = one("Hover for tooltip")["rect"]
    around = (x, y, w + 320, h + 60)  # the button and the tooltip imgui draws beside the cursor
    hovered = png_pixels(tool("hover", label="Hover for tooltip", window=EDGE, speed="instant", screenshot=True)[1])
    assert differing_pixels(baseline, hovered, around) > 100, "no tooltip or hover highlight in the attached screenshot"
    batched = png_pixels(tool("run_steps", steps=[{"tool": "hover", "args": {"label": "Hover for tooltip", "speed": "instant"}}], screenshot=True)[1])
    assert differing_pixels(baseline, batched, around) > 100, "no tooltip or hover highlight in the run_steps screenshot"
    step_frames(5)
    after = png_pixels(tool("screenshot", scale=1.0)[1])
    assert differing_pixels(hovered, after, around) > 100, "the pointer was not released after the screenshot"


# ---------------------------------------------------------------- basic actions


@test
def action_click_by_label_id_and_coordinates():
    before = state("playground")["counter"]
    ok("click", label="+1", window="Agent Playground", speed="instant")
    plus = one("+1", "Agent Playground")
    ok("click", id=plus["id"], speed="instant")
    x, y, w, h = plus["rect"]
    ok("click", x=x + w / 2, y=y + h / 2, speed="instant")
    assert state("playground")["counter"] == before + 3


@test
def action_click_default_visible_speed():
    before = state("playground")["counter"]
    started = time.time()
    res = ok("click", label="-1", window="Agent Playground")
    assert state("playground")["counter"] == before - 1 and res["hit"] is True
    assert time.time() - started < 10


@test
def action_click_reports_changes_and_nochange():
    res = ok("click", label="+1", window="Agent Playground", speed="instant")
    assert "changes" in res
    res = ok("hover", label="+1", window="Agent Playground", speed="instant")
    assert res.get("noVisibleChange") or "changes" in res


@test
def action_bad_targets():
    fails("click", id="zzzz")
    fails("click", id="100000001")
    fails("click", label="no such thing")
    fails("click")
    fails("click", label="")
    fails("hover", x=100)
    fails("scroll", label="+1", window="Agent Playground")
    fails("type_text", label="Name", window="Agent Playground")
    fails("wait_for")
    fails("drag", label="+1", window="Agent Playground")
    fails("key", key="Bogus")
    fails("key", key="")
    fails("key", key="a", mods=["bogus"])
    fails("describe_item", label="no such thing")
    fails("focus_window", window="no such window")
    assert rpc("ping")["result"] == {}


@test
def action_arg_extremes():
    before = state("playground")["counter"]
    for extra in (dict(settle_frames=-5), dict(settle_frames=10 ** 9)):  # clamped, the click still happens
        ok("click", label="+1", window="Agent Playground", speed="instant", **extra)
    assert state("playground")["counter"] == before + 2
    fails("click", label="+1", window="Agent Playground", speed="warp", contains="speed")
    fails("click", label="+1", window="Agent Playground", speed="instant", button="banana", contains="button")
    fails("click", label="+1", window="Agent Playground", speed="instant", kind="bogus")
    assert state("playground")["counter"] == before + 2
    assert rpc("ping")["result"] == {}


@test
def action_offscreen_and_far_coordinates():
    for x, y in ((-10, -10), (5000, 5000), (1279.5, 719.5), (0, 0)):
        tool("click", x=x, y=y, speed="instant")
        tool("hover", x=x, y=y, speed="instant")
    assert rpc("ping")["result"] == {}
    assert ok("get_app_info")["frame"] > 0


@test
def action_modal_and_popups_playground():
    ok("run_steps", steps=[
        {"tool": "click", "args": {"label": "Reset counter", "speed": "instant"}},
        {"tool": "wait_for", "args": {"text": "Reset the counter to 0?"}},
        {"tool": "click", "args": {"label": "Cancel", "speed": "instant"}},
        {"tool": "wait_for", "args": {"text": "Reset the counter to 0?", "gone": True}},
    ])
    kinds = {w["kind"] for w in ok("get_windows")["windows"]}
    assert "modal" not in kinds
    assert ok("get_windows")["openPopups"] == []


# ---------------------------------------------------------------- scrolling child


@test
def scroll_child_and_reveal():
    section("Scrolling child")
    ok("click", label="Row 3", window=EDGE, speed="instant")
    assert edge()["rowClicked"] == 3
    row0 = one("Row 0")
    res = ok("scroll", id=row0["id"], dy=100, speed="instant")
    step_frames(3)
    bottom = edge()["childScrollY"]
    assert bottom > 100, bottom
    ok("scroll", id=one("Row 59")["id"], dy=-100, speed="instant")
    step_frames(3)
    assert edge()["childScrollY"] < bottom
    # Row 59 was seen at the bottom, so its label is remembered and clicking it scrolls it back into view.
    ok("click", label="Row 59", window=EDGE, speed="instant")
    assert edge()["rowClicked"] == 59, edge()["rowClicked"]


@test
def scroll_never_seen_item_gives_hint():
    section("Scrolling child")
    ok("scroll", id=one("Row 0")["id"], dy=-100, speed="instant")
    step_frames(3)
    ok("find_items", query="Row 5", visible_only=True)
    msg = fails("click", label="Row 58x")
    assert "scroll" in str(msg).lower()


@test
def scroll_window_and_point():
    ok("scroll", window=EDGE, dy=2, speed="instant")
    ok("scroll", x=700, y=300, dy=-2, speed="instant")
    ok("scroll", window="Agent Playground", dx=1, speed="instant")
    fails("scroll", window=EDGE, dy=10 ** 6)
    section("Scrolling child", False)


# ---------------------------------------------------------------- table, tabs, tree


@test
def table_select_and_ambiguous_labels():
    section("Table")
    ok("click", label="Item 7", window=EDGE, speed="instant")
    assert edge()["tableSelected"] == 7
    ok("click", label="Item 1", window=EDGE, speed="instant")
    assert edge()["tableSelected"] == 1, "exact match should win over 'Item 10'"
    msg = fails("click", label="Edit", window=EDGE)
    assert "matches" in str(msg)
    edits = [i for i in items("Edit") if i["label"] == "Edit"]
    assert len(edits) >= 2
    ok("click", id=edits[1]["id"], speed="instant")
    assert edge()["tableEdit"] >= 0
    section("Table", False)


@test
def tabs_switch():
    section("Tabs and nesting")
    ok("click", label="Beta", window=EDGE, speed="instant")
    ok("wait_for", text="Beta tab content")
    assert not have_text("Alpha tab content", EDGE)
    ok("click", label="Gamma", window=EDGE, speed="instant")
    ok("wait_for", text="Gamma tab content")
    ok("click", label="Alpha", window=EDGE, speed="instant")


@test
def tree_nested_batch_and_idempotent_open():
    section("Tabs and nesting")
    steps = [{"tool": "click", "args": {"label": f"Level {n}", "ensure": "open", "speed": "instant"}} for n in range(1, 6)]
    steps.append({"tool": "click", "args": {"label": "Deep leaf", "speed": "instant"}})
    res = ok("run_steps", steps=steps)
    assert res["completed"] == 6 and edge()["deepLeaf"] == 1
    again = ok("click", label="Level 3", ensure="open", speed="instant")
    assert again.get("skipped")
    nested = ok("get_ui_tree", window=EDGE, nested=True, limit=500)
    text = json.dumps(nested)
    assert "Deep leaf" in text
    ok("click", label="Level 1", window=EDGE, ensure="closed", speed="instant")
    section("Tabs and nesting", False)


# ---------------------------------------------------------------- inputs


@test
def inputs_multiline_and_unicode():
    section("Inputs")
    ok("type_text", label="Multiline", window=EDGE, text="a\nb", replace=True, speed="instant")
    assert edge()["multiline"] == "a\nb", repr(edge()["multiline"])
    # Only BMP characters: imgui is built with 16-bit ImWchar, so anything above U+FFFF becomes U+FFFD.
    sample = "h\u00e9llo w\u00f6rld \u65e5\u672c"
    ok("type_text", label="Unicode input", window=EDGE, text=sample, replace=True, speed="instant")
    assert edge()["unicode"] == sample, repr(edge()["unicode"])
    ok("type_text", text="!", speed="instant")
    assert edge()["unicode"] == sample + "!"


@test
def inputs_password_readonly_and_numbers():
    section("Inputs")
    ok("type_text", label="Password", window=EDGE, text="secret", replace=True, speed="instant")
    assert edge()["password"] == "secret"
    assert not have_text("secret", EDGE), "password text leaked through get_text"
    pw = one("Password")
    assert "secret" not in json.dumps(pw)
    ok("type_text", label="Read only", window=EDGE, text="xyz", speed="instant")
    assert not have_text("xyz", EDGE)
    ok("type_text", label="Int value", window=EDGE, text="42", replace=True, submit=True, speed="instant")
    assert edge()["intVal"] == 42, edge()["intVal"]


@test
def inputs_drag_slider_radio_list():
    section("Inputs")
    before = edge()["dragVal"]
    ok("drag", label="Drag value", window=EDGE, dx=60, dy=0, speed="instant")
    assert edge()["dragVal"] != before
    ok("drag", label="Slider int", window=EDGE, dx=40, dy=0, speed="instant")
    assert edge()["sliderVal"] != 10
    ok("click", label="Radio B", window=EDGE, speed="instant")
    assert edge()["radio"] == 1
    ok("click", label="Radio C", window=EDGE, speed="instant")
    assert edge()["radio"] == 2
    fails("click", label="Radio C", window=EDGE, ensure="checked", contains="checkbox")
    ok("click", label="Cherry", window=EDGE, speed="instant")
    assert edge()["listSel"] == 2


@test
def inputs_ensure_checked_everywhere():
    section("Inputs")
    ok("click", label="Choice 1", window=EDGE, speed="instant")
    assert edge()["selectables"][1] is True
    ok("click", label="Choice 1", window=EDGE, speed="instant")
    assert edge()["selectables"][1] is False
    fails("click", label="Choice 1", window=EDGE, ensure="checked", contains="checkbox")
    ok("click", label="Plain check", window=EDGE, ensure="checked", speed="instant")
    assert edge()["check"] is True
    fails("click", label="Plain check", window=EDGE, ensure="open")
    fails("click", label="Plain check", window=EDGE, ensure="sideways")
    fails("click", x=100, y=100, ensure="checked")
    section("Inputs", False)


@test
def inputs_color_popup_opens_and_closes():
    section("Inputs")
    swatches = [i for i in items("", EDGE, visible_only=True) if "color" in i.get("label", "").lower() or "color" in json.dumps(i).lower()]
    assert swatches, "no color widget found"
    ok("click", id=swatches[0]["id"], speed="instant")
    step_frames(3)
    ok("key", key="Escape", speed="instant")
    section("Inputs", False)


# ---------------------------------------------------------------- awkward buttons


@test
def awkward_disabled_and_duplicate_labels():
    section("Awkward buttons")
    tool("click", label="Disabled button", window=EDGE, speed="instant")
    assert edge()["disabledClicks"] == 0
    msg = fails("click", label="Delete", window=EDGE)
    assert "3 items" in str(msg) or "matches" in str(msg)
    ok("click", id=one("Delete##c")["id"], speed="instant")
    assert edge()["deleteClicked"] == "c"


@test
def awkward_unusual_label_text():
    section("Awkward buttons")
    ok("click", label="Caf\u00e9", window=EDGE, speed="instant")
    assert edge()["special"] == "unicode"
    ok("click", label="\u65e5\u672c\u8a9e", window=EDGE, speed="instant")
    ok("click", label='Quote "x"', window=EDGE, speed="instant")
    assert edge()["special"] == "quote"
    ok("click", label="100% done", window=EDGE, speed="instant")
    assert edge()["special"] == "percent"
    ok("click", label="LLLLLLLLLL", window=EDGE, speed="instant")
    assert edge()["special"] == "long"
    longest = max(len(i.get("label", "")) for i in items("", EDGE))
    assert longest <= 600, longest


@test
def awkward_mouse_variants():
    section("Awkward buttons")
    ok("click", label="Click variants", window=EDGE, double=True, speed="instant")
    assert edge()["doubleClicks"] == 1
    ok("click", label="Click variants", window=EDGE, button="right", speed="instant")
    assert edge()["rightClicks"] == 1
    ok("click", label="Click variants", window=EDGE, button="middle", speed="instant")
    assert edge()["middleClicks"] == 1


@test
def awkward_context_menu_batch():
    section("Awkward buttons")
    res = ok("run_steps", steps=[
        {"tool": "click", "args": {"label": "Right-click me", "button": "right", "speed": "instant"}},
        {"tool": "wait_for", "args": {"text": "Copy"}},
        {"tool": "click", "args": {"label": "Copy", "speed": "instant"}},
    ])
    assert edge()["menuAction"] == "ctx>Copy", (edge()["menuAction"], res)
    assert ok("get_windows")["openPopups"] == []


@test
def awkward_allow_duplicate_id_not_flagged():
    section("Awkward buttons")
    lint = ok("ui_lint", window=EDGE, rules=["duplicate-id"])
    flagged = [i for i in lint["issues"] if "Allowed dup" in json.dumps(i)]
    assert not flagged, flagged
    ok("click", label="Allowed dup", window=EDGE, speed="instant")
    section("Awkward buttons", False)


# ---------------------------------------------------------------- menus, popups, drag and drop, tooltips


@test
def menus_nested_and_checkable():
    ok("run_steps", steps=[
        {"tool": "click", "args": {"label": "File", "speed": "instant"}},
        {"tool": "click", "args": {"label": "Recent", "speed": "instant"}},
        {"tool": "click", "args": {"label": "a.txt", "speed": "instant"}},
    ])
    assert edge()["menuAction"] == "File>Recent>a.txt", edge()["menuAction"]
    ok("click", label="Edit", window=EDGE, speed="instant")
    ok("click", label="Toggle flag", ensure="checked", speed="instant")
    assert edge()["menuFlag"] is True
    step_frames(3)
    ok("key", key="Escape", speed="instant")


@test
def popups_nested_batch():
    section("Popups and drag and drop")
    ok("run_steps", steps=[
        {"tool": "click", "args": {"label": "Open outer", "speed": "instant"}},
        {"tool": "click", "args": {"label": "Open inner", "speed": "instant"}},
        {"tool": "click", "args": {"label": "Inner OK", "speed": "instant"}},
    ])
    assert edge()["menuAction"] == "inner ok"
    assert len(ok("get_windows")["openPopups"]) == 1, "only the inner popup should have closed"
    ok("key", key="Escape", speed="instant")
    step_frames(2)
    assert ok("get_windows")["openPopups"] == []


@test
def drag_and_drop_between_widgets():
    section("Popups and drag and drop")
    ok("drag", label="Drag me A", window=EDGE, to_label="Drop zone", to_window=EDGE, speed="instant")
    assert edge()["dropped"] == "A", edge()["dropped"]
    ok("drag", label="Drag me B", window=EDGE, to_label="Drop zone", to_window=EDGE)
    assert edge()["dropped"] == "B", edge()["dropped"]
    fails("drag", label="Drag me A", window=EDGE, to_label="no such target")


@test
def tooltip_appears_and_disappears():
    section("Popups and drag and drop")
    # The agent's pointer is released when an action ends, so the tooltip is only observable in the reply or inside a batch.
    res = ok("hover", label="Hover for tooltip", window=EDGE, speed="instant")
    assert "only visible while hovering" in json.dumps(res.get("changes", {})), res
    step_frames(3)
    assert not have_text("only visible while hovering")
    batch = ok("run_steps", steps=[
        {"tool": "hover", "args": {"label": "Hover for tooltip", "speed": "instant"}},
        {"tool": "get_text", "args": {"query": "only visible while hovering"}},
        {"tool": "get_windows"},
    ])
    assert batch["results"][1]["result"]["texts"], batch["results"][1]
    assert "tooltip" in {w["kind"] for w in batch["results"][2]["result"]["windows"]}
    section("Popups and drag and drop", False)


# ---------------------------------------------------------------- keyboard


@test
def keys_report_chords():
    # imgui swaps Ctrl and Cmd on macOS, so a physical Ctrl press reads as Super there (and shortcut = Cmd reads as Ctrl).
    mac = sys.platform == "darwin"
    ctrl = "Super" if mac else "Ctrl"
    ok("key", key="Escape", speed="instant")
    step_frames(3)
    ok("key", key="ctrl+s", speed="instant")
    assert edge()["lastKey"] == f"{ctrl}+S", edge()["lastKey"]
    ok("key", key="F5", speed="instant")
    assert edge()["lastKey"] == "F5"
    ok("key", key="ctrl+shift+z", speed="instant")
    assert edge()["lastKey"] == ("Shift+Super+Z" if mac else "Ctrl+Shift+Z"), edge()["lastKey"]
    ok("key", key="Tab", mods=["shift"], speed="instant")
    assert edge()["lastKey"] == "Shift+Tab", edge()["lastKey"]
    ok("key", key="shortcut+z", speed="instant")
    expected = ("Ctrl+Z",) if mac else ("Ctrl+Z", "Super+Z")
    assert edge()["lastKey"] in expected, edge()["lastKey"]
    count = edge()["keyCount"]
    ok("key", key="F6", repeat=3, speed="instant")
    assert edge()["keyCount"] == count + 3, (count, edge()["keyCount"])


# ---------------------------------------------------------------- timing


@test
def timing_wait_for_appear_vanish_timeout():
    section("Timing")
    ok("click", label="Start delayed", window=EDGE, speed="instant")
    assert not have_text("Delayed text appeared")
    ok("wait_for", text="Delayed text appeared", timeout_frames=200)
    msg = fails("wait_for", text="never appears", timeout_frames=10)
    assert "timed out" in str(msg).lower()
    ok("click", label="Start vanishing", window=EDGE, speed="instant")
    ok("wait_for", label="Vanishing item", gone=True, timeout_frames=200)
    ok("click", label="Hide delayed", window=EDGE, speed="instant")
    ok("wait_for", text="Delayed text appeared", gone=True, timeout_frames=60)
    section("Timing", False)


@test
def timing_wait_for_kind_filter_and_window():
    section("Timing")
    ok("wait_for", label="Start delayed", kind="item", window=EDGE, timeout_frames=30)
    fails("wait_for", label="Start delayed", window="Agent Playground", timeout_frames=5)
    ok("wait_for", id=one("Start delayed")["id"], timeout_frames=30)
    section("Timing", False)


# ---------------------------------------------------------------- stress


@test
def stress_three_thousand_widgets():
    section("Stress: 3000 widgets")
    step_frames(5)
    fps = ok("get_app_info")["fps"]
    assert fps >= 20, f"fps dropped to {fps}"
    started = time.time()
    ok("get_ui_tree", window=EDGE, limit=500, visible_only=False)
    assert time.time() - started < 5
    assert ok("get_app_info")["items"] > 3000
    started = time.time()
    ok("ui_lint", window=EDGE)
    assert time.time() - started < 10
    ok("click", label="S5", window=EDGE, speed="instant")
    assert edge()["stressClicked"] == 5


@test
def stress_scroll_then_click_far_row():
    section("Stress: 3000 widgets")
    ok("scroll", id=one("S5")["id"], dy=200, speed="instant")
    step_frames(3)
    visible = [i for i in items("S", EDGE, visible_only=True, limit=200) if i["label"].startswith("S") and i["label"][1:].isdigit()]
    assert visible and int(visible[-1]["label"][1:]) > 100, [v["label"] for v in visible[-3:]]
    target = visible[-1]["label"]
    ok("click", label=target, window=EDGE, speed="instant")
    assert edge()["stressClicked"] == int(target[1:])
    # Labels of items clipped out of view are known, so the click scrolls them into view first.
    ok("click", label="S2999", window=EDGE, speed="instant")
    assert edge()["stressClicked"] == 2999, edge()["stressClicked"]
    section("Stress: 3000 widgets", False)


@test
def suggestions_for_typos():
    section("Inputs")
    msg = fails("click", label="Raido B", window=EDGE, contains="did you mean")
    assert '"Radio B"' in str(msg), msg
    msg = fails("hover", window="Egde Cases", contains="did you mean")
    assert '"Edge Cases"' in str(msg), msg
    msg = fails("click", label="Greeet", speed="instant")  # no window given: looks everywhere
    assert '"Greet"' in str(msg), msg
    assert "did you mean" not in str(fails("click", label="qqqqqqqqqq")).lower()


@test
def set_value_numbers_text_and_booleans():
    section("Inputs")
    previous = edge()["sliderVal"]
    res = ok("set_value", label="Slider int", window=EDGE, value=70, speed="instant")
    assert edge()["sliderVal"] == 70 and res["before"] == str(previous) and res["value"] == "70" and "warnings" not in res, res
    ok("set_value", label="Drag value", window=EDGE, value=3.5, speed="instant")
    assert edge()["dragVal"] == 3.5
    ok("set_value", label="Int value", window=EDGE, value=-7, speed="instant")
    assert edge()["intVal"] == -7
    sample = "h\u00e9llo w\u00f6rld"
    ok("set_value", label="Unicode input", window=EDGE, value=sample, speed="instant")
    assert edge()["unicode"] == sample
    ok("set_value", label="Unicode input", window=EDGE, value=123, speed="instant")  # a number typed into a text field
    assert edge()["unicode"] == "123"
    ok("set_value", label="Unicode input", window=EDGE, value="", speed="instant")  # an empty string clears the field
    assert edge()["unicode"] == ""
    accented = "\u00e9" * 200  # 400 bytes but only 200 characters: within the limit; the 128-byte app buffer truncates it
    res = ok("set_value", label="Unicode input", window=EDGE, value=accented, speed="instant")
    assert accented.startswith(edge()["unicode"]) and 0 < len(edge()["unicode"]) < 200
    assert any("shows" in w for w in res["warnings"]), res
    assert ok("set_value", label="Plain check", window=EDGE, value=True, speed="instant")["ok"]
    assert edge()["check"] is True
    assert ok("set_value", label="Plain check", window=EDGE, value=True, speed="instant").get("skipped") == "already checked"
    ok("set_value", label="Plain check", window=EDGE, value=False, speed="instant")
    assert edge()["check"] is False


@test
def set_value_warns_when_the_widget_changes_it():
    section("Inputs")
    res = ok("set_value", label="Clamped slider", window=EDGE, value=99, speed="instant")
    assert edge()["clamped"] == 10, edge()["clamped"]
    assert any("shows" in w for w in res["warnings"]), res


@test
def set_value_rejects_unsupported_input():
    section("Inputs")
    fails("set_value", label="Cherry", window=EDGE, value=1, contains="use click")
    fails("set_value", label="Slider int", window=EDGE, value=True, contains="checkbox")
    fails("set_value", label="Slider int", window=EDGE, contains="value")
    fails("set_value", label="Slider int", window=EDGE, value=[1, 2])
    fails("set_value", label="Unicode input", window=EDGE, value="x" * 300, contains="256")
    fails("set_value", value=1, contains="Specify")


@test
def set_value_works_in_a_batch():
    section("Inputs")
    res = ok("run_steps", steps=[
        {"tool": "set_value", "args": {"label": "Slider int", "window": EDGE, "value": 33, "speed": "instant"}},
        {"tool": "set_value", "args": {"label": "Int value", "window": EDGE, "value": 8, "speed": "instant"}},
    ])
    assert res["completed"] == 2 and edge()["sliderVal"] == 33 and edge()["intVal"] == 8, res


@test
def screenshot_of_a_widget_or_window():
    section("Inputs")
    slider = one("Slider int")
    x, y, w, h = slider["rect"]
    res = tool("screenshot", label="Slider int", window=EDGE, scale=1.0, padding=0)[1]
    assert close_size(png_size(res), (w, h)), (png_size(res), (w, h))
    texts_ = [c["text"] for c in res["content"] if c["type"] == "text"]
    assert any(slider["id"] in t for t in texts_), "reply should describe the target"
    padded = tool("screenshot", id=slider["id"], scale=1.0)[1]
    assert close_size(png_size(padded), (w + 16, h + 16)), png_size(padded)
    win = next(i for i in ok("get_windows")["windows"] if i["name"] == EDGE)
    assert close_size(png_size(tool("screenshot", window=EDGE, scale=1.0, padding=0)[1]), (win["rect"][2], win["rect"][3]))
    fails("screenshot", label="Radio", window=EDGE, contains="matches")
    fails("screenshot", window="no such window")
    fails("screenshot", label="Raido B", window=EDGE, contains="did you mean")


@test
def screenshot_of_a_scrolled_out_widget_fails():
    section("Scrolling child")
    ok("scroll", id=one("Row 0")["id"], dy=100, speed="instant")
    step_frames(3)
    ok("scroll", id=one("Row 59")["id"], dy=-100, speed="instant")
    step_frames(3)
    fails("screenshot", label="Row 59", window=EDGE, contains="not visible")


@test
def metrics_report_timing_draw_stats_and_reset():
    step_frames(30)
    m = ok("get_metrics")
    assert m["frames"] > 20 and m["fps"] > 10, m
    for key in ("frameMs", "uiMs", "renderMs", "agentMs"):
        assert m[key]["max"] >= m[key]["p99"] >= m[key]["p95"] >= m[key]["p50"] >= 0, (key, m[key])
    assert m["uiMs"]["avg"] > 0 and m["draw"]["vertices"] > 0 and m["draw"]["drawCommands"] > 0 and m["windows"] >= 3
    small = ok("get_metrics", frames=5)
    assert small["frames"] == 5
    started_at = ok("get_app_info")["frame"]
    ok("get_metrics", reset=True)
    assert ok("get_metrics")["frames"] <= ok("get_app_info")["frame"] - started_at + 2
    step_frames(10)
    recorded = ok("get_metrics")["frames"]
    assert 10 <= recorded <= ok("get_app_info")["frame"] - started_at + 2, recorded
    base_items = m["items"]
    section("Stress: 3000 widgets")
    step_frames(5)
    assert ok("get_metrics")["items"] > base_items + 2000


# ---------------------------------------------------------------- off-screen window


@test
def offscreen_items_and_lint():
    tree = ok("get_ui_tree", window="Offscreen Window", visible_only=False, labeled_only=False)
    count = sum(len(w["items"]) for w in tree["windows"])
    assert count >= 2, f"item below the display edge was not recorded ({count})"
    ok("click", label="Edge button", window="Offscreen Window", speed="instant")
    assert edge()["offscreenClicks"] >= 1
    lint = ok("ui_lint", window="Offscreen Window")
    assert any(i["rule"] == "off-display" for i in lint["issues"]), lint


@test
def focus_window_orders_z():
    ok("focus_window", window="Agent Playground", speed="instant")
    z = {w["name"]: w["z"] for w in ok("get_windows")["windows"]}
    assert z["Agent Playground"] > z[EDGE]
    ok("focus_window", window=EDGE, speed="instant")
    z = {w["name"]: w["z"] for w in ok("get_windows")["windows"]}
    assert z[EDGE] > z["Agent Playground"]
    ok("focus_window", id=one("File")["id"], speed="instant")


@test
def covered_target_is_refused():
    ok("click", label="Show ImGui demo window", window="Agent Playground", ensure="checked", speed="instant")
    try:
        step_frames(3)
        demo = next(w for w in ok("get_windows")["windows"] if w["name"] == "Dear ImGui Demo")
        x, y, w, _ = demo["rect"]
        ok("drag", x=x + w / 2, y=y + 10, dx=-(x + w / 2) + 150, dy=60, speed="instant")
        ok("focus_window", window="Dear ImGui Demo", speed="instant")
        msg = fails("click", label="Greet", window="Agent Playground", speed="instant")
        assert "on top" in str(msg).lower() or "another" in str(msg).lower()
        ok("focus_window", window="Agent Playground", speed="instant")
        ok("click", label="Greet", window="Agent Playground", speed="instant")
    finally:
        ok("click", label="Show ImGui demo window", window="Agent Playground", ensure="unchecked", speed="instant")
        ok("run_command", name="reset_layout")


# ---------------------------------------------------------------- batching and commands


@test
def batch_validation():
    fails("run_steps", steps=[])
    fails("run_steps", steps="nope")
    fails("run_steps", steps=[{"tool": "click"}] * 51)
    fails("run_steps", steps=[{"tool": "run_steps", "args": {"steps": [{"tool": "get_app_info"}]}}], contains="nested")
    fails("run_steps", steps=[{"tool": "screenshot"}], contains="screenshot")
    fails("run_steps", steps=[{"tool": "no_such_tool"}], contains="unknown")
    fails("run_steps", steps=[{"args": {}}], contains="tool")
    fails("run_steps", steps=[{"tool": "click", "args": "nope"}])
    fails("run_steps", steps=["click"])
    fails("run_steps", steps=[{"tool": "click", "args": {"label": "+1", "screenshot": True}}])


@test
def batch_continue_after_error_and_screenshot():
    res = ok("run_steps", stop_on_error=False, steps=[
        {"tool": "click", "args": {"label": "no such thing", "speed": "instant"}},
        {"tool": "get_app_info"},
    ])
    assert [r["ok"] for r in res["results"]] == [False, True] and res["completed"] == 1, res
    png_size(tool("run_steps", steps=[{"tool": "get_app_info"}], screenshot=True)[1])


@test
def batch_with_commands_and_changes():
    res = ok("run_steps", steps=[
        {"tool": "run_command", "args": {"name": "add_todos", "args": {"count": 3}}},
        {"tool": "click", "args": {"label": "+1", "window": "Agent Playground", "speed": "instant"}},
        {"tool": "get_text", "args": {"query": "Sample todo"}},
    ])
    assert res["completed"] == 3
    assert len(state("playground")["todos"]) >= 5
    changes = res["results"][1]["result"].get("changes", {})
    assert "Sample todo" not in json.dumps(changes), "command effects leaked into the click's changes"
    ok("run_command", name="reset_playground")


@test
def commands_edge_cases():
    fails("run_command", name="no_such_command")
    fails("run_command", name="set_theme", args={"name": "bogus"})
    fails("run_command", name="set_theme", args={})
    fails("run_command", name="set_theme", args="not json")
    fails("run_command", name="add_todos", args={"count": "many"})
    fails("run_command")
    ok("run_command", name="add_todos", args={"count": 10 ** 6})
    assert len(state("playground")["todos"]) <= 60
    ok("run_command", name="reset_playground")


@test
def themes_and_lint_across_all():
    original = state("theme")["name"]
    try:
        for theme in THEMES:
            ok("run_command", name="set_theme", args={"name": theme})
            assert state("theme")["name"] == theme
            step_frames(2)
            assert "issues" in ok("ui_lint", window="Agent Playground")
            assert not tool("screenshot", scale=0.25)[0]
    finally:
        ok("run_command", name="set_theme", args={"name": original})


@test
def lint_samples_in_playground():
    ok("click", label="Lint samples (intentional problems)", window="Agent Playground", ensure="open", speed="instant")
    lint = ok("ui_lint", window="Agent Playground", max_per_rule=1)
    rules = {i["rule"] for i in lint["issues"]}
    assert {"duplicate-id", "tiny-target", "overlap", "low-contrast"} <= rules, rules
    capped = ok("ui_lint", window="Agent Playground", rules=["duplicate-id"], max_per_rule=1)
    assert all(i["rule"] == "duplicate-id" for i in capped["issues"])
    fails("ui_lint", rules=["no-such-rule"], contains="unknown rule")
    ok("ui_lint", window="no such window")
    ok("click", label="Lint samples (intentional problems)", window="Agent Playground", ensure="closed", speed="instant")


@test
def final_health():
    reset()
    assert rpc("ping")["result"] == {}
    info = ok("get_app_info")
    assert info["fps"] > 10
    log = ok("get_log", lines=20)
    assert "lines" in log


print()
failed = [(name, detail) for name, detail in results if detail]
print(f"{len(results) - len(failed)}/{len(results)} passed")
for name, detail in failed:
    print(f"  FAIL {name}: {detail[:300]}")
sys.exit(1 if failed else 0)
