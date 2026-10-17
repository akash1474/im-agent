#ifdef AGENT_BRIDGE

#include "AgentInternal.h"
#include "AgentUtil.h"

namespace Agent
{
    namespace
    {
        constexpr size_t kMaxSteps = 50;

        struct SubResult
        {
            bool done = false, error = false;
            Json data;
        };

        struct Batch
        {
            CallPtr call;
            std::vector<Json> steps; // {"tool", "args"}, already validated.
            size_t next = 0;
            bool stopOnError = true;
            bool screenshot = false;
            Json results = Json::array();
            CallPtr subCall; // The step currently running.
            std::shared_ptr<SubResult> sub;
            std::string subTool;
            int notBeforeFrame = 0; // After a state-changing command the snapshot needs a couple of frames to show the result.
        };

        std::vector<Batch> gBatches;

        // Keeps the batch reply small: action replies drop the target echoes the agent already knows.
        Json Trim(Json data)
        {
            if (data.is_object() && data.contains("action"))
                for (const char* key : {"target", "from", "to", "frame", "ok"})
                    data.erase(key);
            return data;
        }

        void Finish(Batch& b, bool ok)
        {
            Actions::Hold(-1);
            Json out = Json::object();
            int completed = 0;
            for (const Json& entry : b.results)
                completed += entry["ok"].get<bool>() ? 1 : 0;
            out["ok"] = ok;
            out["completed"] = completed;
            out["total"] = (int)b.steps.size();
            out["results"] = std::move(b.results);
            if (ok && b.screenshot)
                QueueScreenshot(b.call, ShotOptions{}, out);
            else if (ok)
                b.call->Ok(out);
            else
                b.call->Fail(Dump(out)); // isError, with the partial results as the message.
        }

        void StartStep(Batch& b)
        {
            const Json& step = b.steps[b.next++];
            b.subTool = step["tool"].get<std::string>();

            auto call = std::make_shared<Call>();
            call->tool = FindTool(b.subTool); // Validated when the batch was accepted.
            if (step.contains("args"))
                call->args = step["args"];
            call->parent = b.call;

            b.sub = std::make_shared<SubResult>();
            call->sink = [sub = b.sub](bool isError, Json data) {
                sub->done = true;
                sub->error = isError;
                sub->data = std::move(data);
            };
            b.subCall = call;
            try
            {
                call->tool->run(call);
            }
            catch (const std::exception& e)
            {
                call->Fail(e.what());
            }
        }

        // Returns true once the batch has finished (successfully or not).
        bool Advance(Batch& b)
        {
            if (b.call->cancelled)
            {
                if (b.subCall)
                    b.subCall->cancelled = true;
                Actions::Hold(-1);
                return true;
            }
            for (;;)
            {
                if (b.sub)
                {
                    if (!b.sub->done)
                        return false;
                    Json entry = Json::object();
                    entry["step"] = (int)b.next;
                    entry["tool"] = b.subTool;
                    entry["ok"] = !b.sub->error;
                    if (b.sub->error)
                        entry["error"] = b.sub->data.is_string() ? b.sub->data : Json(Dump(b.sub->data));
                    else
                        entry["result"] = Trim(std::move(b.sub->data));
                    // A user abort always ends the batch, whatever stop_on_error says.
                    const bool aborted = b.sub->error && b.sub->data.is_string() && b.sub->data.get<std::string>().rfind("Aborted by the user", 0) == 0;
                    const bool stop = b.sub->error && (b.stopOnError || aborted);
                    b.results.push_back(std::move(entry));
                    if (b.subTool == "run_command")
                        b.notBeforeFrame = ImGui::GetFrameCount() + 2;
                    b.sub.reset();
                    b.subCall.reset();
                    if (stop)
                    {
                        Finish(b, false);
                        return true;
                    }
                }
                if (b.next >= b.steps.size())
                {
                    Finish(b, true);
                    return true;
                }
                if (ImGui::GetFrameCount() < b.notBeforeFrame)
                    return false;
                StartStep(b);
            }
        }

        void RunSteps(const CallPtr& c)
        {
            auto list = c->args.find("steps");
            if (list == c->args.end() || !list->is_array() || list->empty())
                return c->Fail("Specify steps: a non-empty array of {\"tool\": \"click\", \"args\": {...}}");
            if (list->size() > kMaxSteps)
                return c->Fail("At most " + std::to_string(kMaxSteps) + " steps per batch");

            Batch b;
            b.call = c;
            b.stopOnError = ArgBool(c->args, "stop_on_error", true);
            b.screenshot = ArgBool(c->args, "screenshot", false);
            for (size_t i = 0; i < list->size(); i++)
            {
                const Json& step = (*list)[i];
                const std::string name = step.is_object() ? ArgStr(step, "tool") : std::string();
                const std::string where = "step " + std::to_string(i + 1);
                if (name.empty())
                    return c->Fail(where + ": missing \"tool\"");
                if (name == "run_steps")
                    return c->Fail(where + ": batches cannot be nested");
                if (name == "screenshot" || (step.contains("args") && step["args"].is_object() && ArgBool(step["args"], "screenshot", false)))
                    return c->Fail(where + ": screenshots are not allowed inside a batch; set screenshot on run_steps to get one at the end");
                if (step.contains("args") && !step["args"].is_object())
                    return c->Fail(where + ": args must be an object");
                if (!FindTool(name))
                    return c->Fail(where + ": unknown tool '" + name + "'");
                b.steps.push_back(step);
            }

            Actions::Hold(1);
            if (!Advance(b)) // Runs the leading read-only steps right away.
                gBatches.push_back(std::move(b));
        }
    } // namespace

    void TickBatches()
    {
        for (size_t i = 0; i < gBatches.size();)
        {
            if (Advance(gBatches[i]))
                gBatches.erase(gBatches.begin() + i);
            else
                i++;
        }
    }

    bool HasPendingBatches() { return !gBatches.empty(); }

    void FailBatches(const std::string& reason)
    {
        std::vector<Batch> batches = std::move(gBatches);
        gBatches.clear();
        for (Batch& b : batches)
        {
            if (b.subCall)
                b.subCall->cancelled = true; // Drops a pending wait_for.
            if (b.sub)
            {
                Json entry = Json::object();
                entry["step"] = (int)b.next;
                entry["tool"] = b.subTool;
                entry["ok"] = false;
                entry["error"] = reason;
                b.results.push_back(std::move(entry));
            }
            Finish(b, false);
        }
    }

    void AppendBatchTools(std::vector<Tool>& tools)
    {
        tools.push_back(Tool{
            "run_steps",
            "Run several tools in order as one call and get one combined reply. Each step is {\"tool\": name, \"args\": {...}}; any tool except screenshot and run_steps works "
            "(click, type_text, key, wait_for, get_text, run_command, ...). Each step finds its target when it starts, so later steps can use widgets that earlier steps reveal. "
            "Stops at the first failure unless stop_on_error is false. Action steps report what changed on screen. Use this instead of many separate calls.",
            Json::parse(R"J({"type":"object","properties":{
                "steps":{"type":"array","maxItems":50,"items":{"type":"object","properties":{
                    "tool":{"type":"string","description":"Tool name, e.g. click or type_text."},
                    "args":{"type":"object","description":"The tool's arguments."}},"required":["tool"]}},
                "stop_on_error":{"type":"boolean","description":"Default true."},
                "screenshot":{"type":"boolean","description":"Attach a screenshot taken after the last step, with the pointer still in place."}},"required":["steps"]})J"),
            RunSteps,
            120});
    }
} // namespace Agent

#endif // AGENT_BRIDGE
