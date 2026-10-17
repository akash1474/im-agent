# ImAgent

A debug-only [MCP](https://modelcontextprotocol.io) server that runs inside a Dear ImGui app, so an AI agent (VS Code Copilot, Claude, ...) can look at the running UI and drive it like a user: read widgets and text, click, type, drag, scroll, take screenshots, lint the layout, and call app-defined commands.

Everything compiles to nothing in Release builds.

```
agent (MCP client) --HTTP JSON-RPC--> 127.0.0.1:7878/mcp --> ImAgent --> your ImGui app
```

## Requirements

- Dear ImGui **1.92.9b** with the GLFW + OpenGL3 backends (a `static_assert` fails the build on any other version).
- A one-time local patch to imgui (see [Patching imgui](#patching-imgui)).
- C++17 (enforced on the library target), CMake 3.16+, OpenGL, Threads. With `AGENT_BRIDGE=OFF` only the imgui target and C++ are needed.
- Header-only dependencies, found in `IMAGENT_THIRD_PARTY_DIR` (default `<repo>/packages`): `cpp-httplib/httplib.h` and `nlohmann/json.hpp`. The stb PNG writer is vendored in `third_party/stb`.

## Using it

### 1. CMake

```cmake
add_library(imgui STATIC ...)                 # your imgui target, defined first
add_subdirectory(src/agent)
imagent_configure(imgui IMGUI_DIR ${IMGUI_DIR})
target_link_libraries(my_app PRIVATE ImAgent::ImAgent)
```

`imagent_configure` defines `AGENT_BRIDGE=1`, `IMGUI_AGENT_HOOKS` and `IMGUI_AGENT_TEXT_HOOK` on imgui in Debug builds and fails the configure step if imgui is not patched.

| Option / variable         | Default           | Meaning                                          |
| ------------------------- | ----------------- | ------------------------------------------------ |
| `AGENT_BRIDGE`            | `ON`              | `OFF` removes the bridge even from Debug builds. |
| `IMAGENT_THIRD_PARTY_DIR` | `<repo>/packages` | Where `cpp-httplib/` and `nlohmann/` live.       |

Single-config generators with no build type default to Debug in the root `CMakeLists.txt`; otherwise the bridge would silently be absent.

### 2. Frame loop

```cpp
#include "agent/AgentBridge.h"

Agent::Start();                         // once, after the ImGui and backend init

while (!done) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    Agent::Pump();                      // after the backend's NewFrame, so injected input can replace its events
    ImGui::NewFrame();
    /* build the UI */
    Agent::EndFrame();                  // before ImGui::Render()
    ImGui::Render();
    /* render draw data */
    Agent::AfterDraw();                 // after RenderDrawData, before swapping buffers
    glfwSwapBuffers(window);
}

Agent::Stop();                          // before shutting imgui down
```

### 3. Optional: expose state and commands

```cpp
AGENT_EXPOSE("theme", [] { return R"({"name":"zinc-dark"})"; });       // read with get_state
AGENT_COMMAND("reset_data", "Reload the sample data.", [](const std::string& argsJson) {
    LoadSampleData();
    return std::string("{\"ok\":true}");                               // throw std::exception to report an error
});
```

Commands are how an agent does things that are slow or awkward through the UI (switching themes, loading fixtures). Both macros are no-ops when the bridge is compiled out.

### 4. Point the client at it

`.vscode/mcp.json`:
```json
{ "servers": { "imgui-app": { "type": "http", "url": "http://127.0.0.1:7878/mcp" } } }
```

Or for Antigravity / Gemini (`~/.gemini/config/mcp_config.json`):
```json
{
  "mcpServers": {
    "stride-agent": {
      "url": "http://127.0.0.1:7878/mcp"
    }
  }
}
```

> **Note for AI Agents**: When active (Stride running in Debug mode), agents should invoke tools directly through the harness tool (e.g. `call_mcp_tool` with `ServerName: "stride-agent"`) rather than writing manual terminal scripts or HTTP requests.

## Tools


Coordinates are ImGui points; item ids are 8-digit hex ImGuiIDs. Data describes the last completed frame.

| Group        | Tools                                                                                                                                                                                                    |
| ------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Inspect      | `get_app_info`, `get_windows`, `get_ui_tree` (flat or nested), `find_items`, `get_text`, `get_hovered`, `get_state`, `get_log`, `get_metrics`, `screenshot` (whole window, region, or one widget/window) |
| Act          | `click` (button, double, `ensure=open/closed/checked/unchecked`), `set_value`, `hover`, `drag`, `scroll`, `type_text`, `key`, `focus_window`, `step_frames`                                              |
| Wait / debug | `wait_for` (item or text appears/disappears), `describe_item` (readable ID path)                                                                                                                         |
| App          | `list_commands`, `run_command`                                                                                                                                                                           |
| Batch        | `run_steps` runs up to 50 tools in one call and returns one combined reply                                                                                                                               |
| Quality      | `ui_lint` finds duplicate ids, off-screen widgets, tiny targets, overlaps, low contrast, unlabeled widgets and duplicate labels                                                                          |

Action replies include a `changes` summary (windows opened/closed, widgets added/removed/changed, text added/removed). Targets are found by `id`, by `label` (optionally with `window`), or by `x`/`y`. An ambiguous label is an error that lists the candidates; a label or window that is not found lists the closest names ("Did you mean ...?").

- `set_value` sets a text or number field, slider or drag to an exact value (string or number) and a checkbox to true/false. It replies with the text before and after and warns when the widget shows something else (clamped, rounded, masked).
- `screenshot` with `id`, `label` (+`window`) or `window` crops to that widget or window (`padding` points around it, full display resolution by default).
- `get_metrics` summarizes the last N frames: `frameMs` (includes vsync), `uiMs` (CPU time of the app's UI code, including this library's per-widget recording in Debug builds), `renderMs`, `agentMs` (the library's Pump/EndFrame work: tool calls and snapshots), slow-frame count, draw statistics and item counts. Call it with `reset=true`, perform an action, then call it again to measure that action.

## Safety

- Binds `127.0.0.1` only and checks `Host`/`Origin` (blocks DNS rebinding from web pages).
- Set `AGENT_TOKEN` to require `Authorization: Bearer <token>`. Set `AGENT_PORT` to change the port.
- **Hold Esc for about half a second to take control back.** Agent input stops and in-flight actions fail.
- At most 4 tool calls may wait at once; each has a timeout (30 s, 120 s for `run_steps` and `wait_for`). A client that disconnects frees its slot.
- Input limits: scroll up to 200 ticks, `type_text` up to 2000 characters, 50 steps per batch.
- Password fields are reported as asterisks.
- A click is refused when another window or item covers the target.

## Patching imgui

imgui needs a few hooks the library depends on, added by a local patch. Every hook compiles to nothing unless its define is set (`IMGUI_AGENT_HOOKS`, `IMGUI_AGENT_TEXT_HOOK`):

- `imgui_draw.cpp`: a text hook in `ImFont::RenderText`, so `get_text` sees every drawn string.
- `imgui_internal.h`, `imgui.cpp`, `imgui_widgets.cpp`: item hooks next to imgui's `IMGUI_TEST_ENGINE_ITEM_ADD/INFO` macros, and the item status flags (`Checkable`, `Opened`, ...) that were only defined for the Test Engine.

```sh
patch -p1 -d packages/imgui < src/agent/patches/imgui-agent-hooks.patch
```

Re-apply it whenever you restore or upgrade imgui, and bump the version in `static_assert` (in `src/AgentInternal.h`) after re-verifying the internals the library uses.

## How it works

- `Recorder.cpp` implements the patch's item and text hooks. Each frame produces a snapshot of items, text and windows. `ItemUnclipByLog` is set while recording, so widgets scrolled out of view still report their labels; labels are remembered across frames.
- `AgentBridge.cpp` is the HTTP/MCP front end (cpp-httplib). Tool calls are queued and run on the main thread in `Pump()`.
- `Input.cpp` injects tagged input events and runs actions step by step across frames, so the real cursor, hover states and animations behave as for a user.
- `Changes.cpp`, `Lint.cpp`, `Batch.cpp`, `Metrics.cpp`, `Screenshot.cpp` and `AgentTools.cpp` / `AgentActions.cpp` hold the diffing, linting, batching, frame timing, capture and tool implementations.

## Known limits

### imgui boundaries

- Only widgets that go through `ItemAdd` with an id are items. `Text`, `Dummy`, images and custom draw-list shapes are not; text is visible only through `get_text`.
- Radio buttons and selectables do not report their checked state, so `ensure=checked` works only for checkboxes and menu items.
- `set_value` supports text/number fields, sliders, drags and checkboxes only; combos, radios and selectables need `click`. Numbers are entered with Ctrl+click (the only way imgui lets sliders and drags take typed input) and committed with Enter; strings press Enter only with `submit=true`, because Enter can run app logic. Values are limited to 256 characters; an empty string clears the field.
- `get_metrics` frame time is wall-clock between frames, so with vsync it sits at the refresh interval; use `uiMs` to see what the app's UI code costs.
- Combos and a few other widgets report no label; it is inferred from the text drawn next to them (`labelInferred`). Scrollbars, resize grips and title-bar buttons are treated as decoration.
- Items skipped by `ImGuiListClipper` or inside a collapsed section are not recorded, so they cannot be found by label; scroll the area or expand the section first. Items past the display edge exist but are unreachable (`ui_lint` flags them).
- Recording sets `ItemUnclipByLog`, so widgets clipped by their window or child still run and report labels. Huge lists without `ImGuiListClipper` cost extra CPU in Debug.
- Labels are matched exactly first, then by substring; an ambiguous label is an error unless exactly one match is visible. Use `id` or `window` to disambiguate (`##` / `###` suffixes keep ids stable).
- Menu and popup items live in their own windows (`##Popup_...`, `Edit###Menu_00`); do not filter them by the parent window's name.
- imgui built with 16-bit `ImWchar` turns characters above U+FFFF (emoji) into U+FFFD when typed.
- On macOS imgui swaps Ctrl and Cmd: the key `ctrl+s` reads as Super+S in the app; `shortcut+s` reads as Ctrl+S.
- One imgui context on the main thread, main viewport only. Multi-viewport and docking are untested.
- Works in the same build as the Dear ImGui Test Engine (the library has its own hooks, so there are no duplicate symbols). Do not drive the UI with both at once; they inject input independently. The root `CMakeLists.txt` shows the setup (`-DWORK_TRACKER_TEST_ENGINE=ON`).

### Library boundaries

- Everything runs in `Pump()`: the app's frame loop must be running. A minimized or blocked window stalls calls until they time out.
- Data describes the last completed frame (about two frames behind a fresh action).
- While an action runs, the user's real mouse and keyboard input is dropped. The agent's pointer goes back to the user when the action ends, so hover states and tooltips are not visible to a later `screenshot` call. To see them, pass `screenshot=true` on the action (or on `run_steps`): the pointer is held until that frame is captured. The tooltip text is also in the action's reply and readable with `get_text` inside a batch.
- No auth by default: any local process can drive the app. Set `AGENT_TOKEN`.
- `AGENT_EXPOSE` / `AGENT_COMMAND` arguments are not compiled in Release; keep needed side effects out of them. Commands cannot be cancelled once running, even if the client times out.
- `run_steps`: no nesting, no screenshots, each step finds its target when it starts, and a `run_command` step waits 2 frames. `scroll` replies carry no `changes`.
- Clicking a disabled widget does nothing (the reply carries a warning); typing into read-only fields is ignored.
- Lint is heuristic: contrast uses the stock style colors, and overlap and tiny-target are advisory.

## Tests

The scripts in `tests/` talk to a running Debug app over MCP (Python 3, standard library only). They expect the demo windows from the host app (`Agent Playground`, `Edge Cases`, `Offscreen Window`) and the commands `reset_playground`, `reset_edge_cases`, `reset_layout`, `set_theme`, `add_todos` registered in its `main.cpp`.

```sh
python3 src/agent/tests/agent_smoke.py          # quick check of the main paths
python3 src/agent/tests/agent_edge_tests.py     # full suite; pass a name fragment to run a subset
```

Both honor `AGENT_PORT` and `AGENT_TOKEN`. Keep the app window at its default size (1280x720): the offscreen-window tests assume it. The tests change app state (themes, counters, windows); restart the app afterwards if that matters.

## Layout

```
src/agent/
  CMakeLists.txt            library target (imagent / ImAgent::ImAgent), imagent_configure()
  include/agent/            public headers (AgentBridge.h API)
  src/                      implementation (private headers live here)
  patches/                  local imgui patch (imgui-agent-hooks.patch)
  third_party/stb/          vendored stb_image_write (MIT / public domain)
  tests/                    MCP end-to-end tests
```
