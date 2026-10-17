#ifdef AGENT_BRIDGE

#include "AgentInternal.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <optional>

#include <GLFW/glfw3.h>

namespace Agent
{
    namespace Input
    {
        namespace
        {
            bool gActive = false;
            bool gSavedDrawCursor = false;
            ImVec2 gPos;
            bool gButtons[ImGuiMouseButton_COUNT] = {};
            std::vector<ImGuiKey> gKeysDown;

            ImGuiContext& Ctx() { return *ImGui::GetCurrentContext(); }

            // Events we queue are tagged so DropBackendEvents() can tell them from the OS ones.
            template <class Emit>
            void Inject(Emit emit)
            {
                ImVector<ImGuiInputEvent>& queue = Ctx().InputEventsQueue;
                const int firstNew = queue.Size;
                emit(ImGui::GetIO());
                for (int i = firstNew; i < queue.Size; i++)
                    queue[i].AddedByTestEngine = true;
            }
        } // namespace

        bool IsActive() { return gActive; }
        ImVec2 MousePos() { return gPos; }

        void Begin()
        {
            if (gActive)
                return;
            ImGuiIO& io = ImGui::GetIO();
            gActive = true;
            gSavedDrawCursor = io.MouseDrawCursor;
            io.MouseDrawCursor = true; // Show the agent's cursor so the user can follow it.
            gPos = io.MousePos.x > -FLT_MAX ? io.MousePos : ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            io.ClearInputKeys();
            DropBackendEvents();
            for (int b = 0; b < ImGuiMouseButton_COUNT; b++)
                if (io.MouseDown[b])
                    Button(b, false); // The user's own release would be dropped and the button would stay stuck.
        }

        void DropBackendEvents()
        {
            if (!gActive)
                return;
            ImVector<ImGuiInputEvent>& queue = Ctx().InputEventsQueue;
            for (int i = 0; i < queue.Size;)
            {
                if (queue[i].AddedByTestEngine)
                    i++;
                else
                    queue.erase(&queue[i]);
            }
        }

        void ReleaseAll()
        {
            for (int b = 0; b < ImGuiMouseButton_COUNT; b++)
                if (gButtons[b])
                    Button(b, false);
            while (!gKeysDown.empty())
                Key(gKeysDown.back(), false);
        }

        void End()
        {
            if (!gActive)
                return;
            ReleaseAll();
            gActive = false;

            ImGuiIO& io = ImGui::GetIO();
            io.MouseDrawCursor = gSavedDrawCursor;

            // The OS backend only reports cursor changes, so resync to where the real cursor is.
            if (GLFWwindow* window = glfwGetCurrentContext())
            {
                if (glfwGetWindowAttrib(window, GLFW_HOVERED))
                {
                    double x, y;
                    glfwGetCursorPos(window, &x, &y);
                    io.AddMousePosEvent((float)x, (float)y);
                }
                else
                {
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                }
            }
        }

        void Move(const ImVec2& pos)
        {
            gPos = pos;
            Inject([&](ImGuiIO& io) { io.AddMousePosEvent(pos.x, pos.y); });
        }

        void Button(int button, bool down)
        {
            if (button < 0 || button >= ImGuiMouseButton_COUNT)
                return;
            gButtons[button] = down;
            Inject([&](ImGuiIO& io) { io.AddMouseButtonEvent(button, down); });
        }

        void Key(ImGuiKey key, bool down)
        {
            auto it = std::find(gKeysDown.begin(), gKeysDown.end(), key);
            if (down && it == gKeysDown.end())
                gKeysDown.push_back(key);
            else if (!down && it != gKeysDown.end())
                gKeysDown.erase(it);
            Inject([&](ImGuiIO& io) { io.AddKeyEvent(key, down); });
        }

        void Text(const std::string& utf8)
        {
            Inject([&](ImGuiIO& io) { io.AddInputCharactersUTF8(utf8.c_str()); });
        }

        void Wheel(float wheelX, float wheelY)
        {
            Inject([&](ImGuiIO& io) { io.AddMouseWheelEvent(wheelX, wheelY); });
        }
    } // namespace Input

    namespace Actions
    {
        namespace
        {
            std::deque<Action> gQueue;
            std::optional<Action> gCurrent;
            int gStepFrame = 0;
            int gEscFrames = 0;
            int gHold = 0; // run_steps batches in flight; input stays taken over between their actions.
            int gCaptureHold = 0; // Screenshots waiting to be captured; the pointer stays put and no new action starts.

            constexpr int kAbortFrames = 30; // Holding Esc for about half a second aborts the agent.

            // Read from GLFW directly because the app's own input events are dropped while the agent is in control.
            bool UserAbortRequested()
            {
                GLFWwindow* window = glfwGetCurrentContext();
                gEscFrames = window && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS ? gEscFrames + 1 : 0;
                return gEscFrames >= kAbortFrames;
            }

            void Finish(Action& a)
            {
                a.result["ok"] = true;
                a.result["frame"] = ImGui::GetFrameCount();
                if (a.before)
                {
                    Json changes = DiffSummary(*a.before, *CaptureSummary(LastSnapshot()));
                    if (changes.is_null())
                        a.result["noVisibleChange"] = true;
                    else
                        a.result["changes"] = std::move(changes);
                }
                if (!a.warnings.empty())
                    a.result["warnings"] = a.warnings;
                if (a.screenshot)
                    QueueScreenshot(a.call, ShotOptions{}, a.result);
                else
                    a.call->Ok(a.result);
            }

            void AbortByUser()
            {
                const std::string reason = "Aborted by the user (Esc was held)";
                std::fprintf(stderr, "[agent] Aborted by the user (Esc held); input is yours again\n");
                FailAll(reason);
                FailBatches(reason);
                gEscFrames = 0;
            }
        } // namespace

        bool IsActive() { return gCurrent.has_value() || !gQueue.empty() || gHold > 0 || gCaptureHold > 0; }
        void Enqueue(Action action) { gQueue.push_back(std::move(action)); }

        void Hold(int delta) { gHold = std::max(0, gHold + delta); }

        void HoldForCapture(int delta) { gCaptureHold = std::max(0, gCaptureHold + delta); }

        void Advance()
        {
            while (!gCurrent && !gQueue.empty() && gCaptureHold == 0)
            {
                Action next = std::move(gQueue.front());
                gQueue.pop_front();
                if (next.call->Cancelled())
                    continue;
                gCurrent = std::move(next);
                gStepFrame = 0;
                if (gCurrent->trackChanges)
                    gCurrent->before = CaptureSummary(LastSnapshot());
                Input::Begin();
            }
            if (!gCurrent)
            {
                if (gHold > 0 || gCaptureHold > 0) // Between the actions of a batch, or until a screenshot is taken: keep control, but still honor the abort key.
                {
                    Input::DropBackendEvents();
                    if (UserAbortRequested())
                        AbortByUser();
                    return;
                }
                gEscFrames = 0;
                Input::End();
                return;
            }

            if (UserAbortRequested())
            {
                AbortByUser();
                return;
            }
            if (gCurrent->call->Cancelled())
            {
                Input::ReleaseAll();
                gCurrent.reset();
                return;
            }
            Input::DropBackendEvents();

            Action& a = *gCurrent;
            ActionCtx ctx{LastSnapshot(), a, gStepFrame};
            const bool done = a.steps.front()(ctx);
            if (ctx.failed)
            {
                Input::ReleaseAll();
                a.call->Fail(ctx.error);
                gCurrent.reset();
                return;
            }
            if (!done)
            {
                gStepFrame++;
                return;
            }

            a.steps.pop_front();
            gStepFrame = 0;
            if (a.steps.empty())
            {
                Finish(a);
                gCurrent.reset();
            }
        }

        void FailAll(const std::string& reason)
        {
            if (gCurrent)
                gCurrent->call->Fail(reason);
            gCurrent.reset();
            for (Action& a : gQueue)
                a.call->Fail(reason);
            gQueue.clear();
            Input::End();
        }
    } // namespace Actions
} // namespace Agent

#endif // AGENT_BRIDGE
