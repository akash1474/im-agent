#pragma once

#include <functional>
#include <string>

// Debug-only MCP server that lets an AI agent inspect the running UI.
// Everything compiles to nothing unless AGENT_BRIDGE is defined (Debug builds).
#ifdef AGENT_BRIDGE
namespace Agent
{
    // port 0 = $AGENT_PORT, else 7878. Returns false if the port can't be bound.
    bool Start(int port = 0);
    void Stop();

    void Pump();      // Main thread, after the backend's NewFrame and before ImGui::NewFrame().
    void EndFrame();  // Main thread, after building the UI and before ImGui::Render().
    void AfterDraw(); // Main thread, after ImGui_ImplOpenGL3_RenderDrawData() and before swapping buffers.
    bool IsBusy();    // True if agent has active inputs or queued tool calls.

    // Main thread only. The provider returns a JSON document describing app state.
    void Expose(const char* name, std::function<std::string()> jsonProvider);

    // Main thread only. A command is an app function the agent can call by name (list_commands / run_command),
    // e.g. switching the theme or loading test data. It gets the call's args as JSON text and returns JSON text;
    // throw std::exception to report an error.
    using CommandFn = std::function<std::string(const std::string& argsJson)>;
    void RegisterCommand(const char* name, const char* description, CommandFn fn);
} // namespace Agent

#define AGENT_EXPOSE(name, ...) Agent::Expose(name, __VA_ARGS__)
#define AGENT_COMMAND(name, description, ...) Agent::RegisterCommand(name, description, __VA_ARGS__)
#else
namespace Agent
{
    inline bool Start(int = 0) { return false; }
    inline void Stop() {}
    inline void Pump() {}
    inline void EndFrame() {}
    inline void AfterDraw() {}
    inline bool IsBusy() { return false; }
} // namespace Agent

#define AGENT_EXPOSE(...) ((void)0)
#define AGENT_COMMAND(...) ((void)0)
#endif
