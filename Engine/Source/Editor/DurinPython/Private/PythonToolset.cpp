#include "PythonToolset.h"
#include "PythonRuntime.h"
#include "Editor/EditorEngine.h"
#include "Tools/EditorToolService.h"
#include "Tools/EditorToolSubsystem.h"

namespace Durin
{
    namespace
    {
        using FObject = FToolValue::FObject;
        auto Failure(EToolError Code, std::string Message) -> FToolResult
        { return {EToolStatus::Failed, {}, {{Code, std::move(Message), {}}}}; }
        struct FClient
        {
            uint64 Session = 0;
            FToolCancellationToken Cancellation;
            std::map<std::string, uint64, std::less<>> Jobs;
        };
        struct FState
        {
            std::shared_ptr<FPythonRuntime> Runtime;
            std::map<std::string, FClient> Clients;
            ~FState() { for (const auto& [Key, Client] : Clients) { (void)Key; Runtime->CloseSession(Client.Session); } }
            auto Call(const FToolRequest& Request, const FToolContext& Context) -> FToolResult
            {
                auto* Editor = GetToolEditor(Context);
                auto* Tools = Editor ? Editor->GetEditorSubsystem<DEditorToolSubsystem>() : nullptr;
                if (!Tools || !Runtime) return Failure(EToolError::ToolUnavailable, "Python runtime or admitting editor is unavailable.");
                std::erase_if(Clients, [&](const auto& Entry) {
                    if (!Entry.second.Cancellation.IsCancellationRequested()) return false;
                    Runtime->CloseSession(Entry.second.Session); return true;
                });
                const auto Key = std::format("{}:{}:{}", Context.SessionId, static_cast<uint8>(Context.Source), Context.Client.Id);
                const auto& Arguments = std::get<FObject>(Request.Arguments.Data);
                if (Request.Address.Tool == "submit")
                {
                    const bool Code = Arguments.contains("code"), File = Arguments.contains("file");
                    if (Code == File) return Failure(EToolError::InvalidArguments, "Specify exactly one of code or file.");
                    auto It = Clients.find(Key);
                    if (It == Clients.end())
                    {
                        auto Session = Runtime->CreateSession(Tools->GetService(), Context.Source, Context.Client);
                        if (!Session) return Failure(EToolError::ToolUnavailable, Session.error());
                        It = Clients.emplace(Key, FClient{*Session, Context.Client.Cancellation, {}}).first;
                    }
                    auto& Client = It->second;
                    std::erase_if(Client.Jobs, [&](const auto& Entry) { return !Runtime->GetJobResult(Entry.second); });
                    auto Submitted = File ? Runtime->RunFile(Client.Session, std::filesystem::u8path(std::get<std::string>(Arguments.at("file").Data)))
                        : Runtime->RunString(Client.Session, std::get<std::string>(Arguments.at("code").Data));
                    if (!Submitted) return Failure(File ? EToolError::InvalidArguments : EToolError::Busy, Submitted.error());
                    // Runtime bounds retained jobs globally; discard expired client handles on admission.
                    std::erase_if(Client.Jobs, [&](const auto& Entry) { return !Runtime->GetJobResult(Entry.second); });
                    std::random_device Random;
                    std::string Id;
                    do { Id = std::format("{:08x}{:08x}{:08x}{:08x}", Random(), Random(), Random(), Random()); }
                    while (Client.Jobs.contains(Id));
                    Client.Jobs.emplace(Id, *Submitted);
                    return {EToolStatus::Succeeded, FObject{{"job_id", std::move(Id)}}};
                }
                const auto Id = std::get<std::string>(Arguments.at("job_id").Data);
                const auto Client = Clients.find(Key);
                if (Client == Clients.end() || !Client->second.Jobs.contains(Id))
                    return Failure(EToolError::TargetNotFound, "Unknown or expired Python job for this client session.");
                const auto Job = Client->second.Jobs.at(Id);
                auto Snapshot = Runtime->GetJobResult(Job);
                if (!Snapshot) return Failure(EToolError::InvocationExpired, Snapshot.error());
                FObject Data{{"job_id", Id}};
                if (Request.Address.Tool == "cancel") Data.emplace("cancellation_requested", Runtime->CancelJob(Job));
                else
                {
                    constexpr const char* Names[] = {"queued", "running", "succeeded", "failed", "cancelled"};
                    Data.emplace("state", Names[static_cast<size_t>(Snapshot->State)]);
                    Data.emplace("output", std::move(Snapshot->Output)); Data.emplace("error", std::move(Snapshot->Error));
                }
                return {EToolStatus::Succeeded, std::move(Data)};
            }
        };
    }
    auto CreatePythonToolset(std::shared_ptr<FPythonRuntime> Runtime) -> std::unique_ptr<FNativeToolset>
    {
        auto State = std::make_shared<FState>(); State->Runtime = std::move(Runtime);
        auto JobId = FToolSchema(EToolValueType::String); JobId.MinSize = 1; JobId.MaxSize = 128;
        std::vector<FToolBinding> Bindings;
        for (const auto Name : {"submit", "status", "cancel"})
        {
            FToolDescriptor Tool; Tool.Name = Name; Tool.bExposeExternally = true;
            Tool.RetryPolicy = EToolRetryPolicy::Idempotent;
            Tool.Output.Properties.emplace("job_id", JobId); Tool.Output.Required = {"job_id"};
            if (Tool.Name == "submit")
            {
                Tool.Description = "Submit trusted Python (not sandboxed): specify exactly one of code or file. Relative files use the project directory and are read before job admission. Poll status for completion; no automatic retry.";
                Tool.Effects = EToolEffect::MutatesMemory | EToolEffect::WritesStorage | EToolEffect::External;
                Tool.RetryPolicy = EToolRetryPolicy::Unsafe;
                auto Code = FToolSchema(EToolValueType::String); Code.MaxSize = 64 * 1024;
                auto Path = FToolSchema(EToolValueType::String); Path.MinSize = 1; Path.MaxSize = 4096;
                Tool.Input.Properties.emplace("code", std::move(Code)); Tool.Input.Properties.emplace("file", std::move(Path));
            }
            else
            {
                Tool.Input.Properties.emplace("job_id", JobId); Tool.Input.Required = {"job_id"};
                if (Tool.Name == "status")
                {
                    Tool.Description = "Query this client's Python job state, captured output and traceback.";
                    auto Status = FToolSchema(EToolValueType::String); Status.Enum = {"queued", "running", "succeeded", "failed", "cancelled"};
                    auto Text = FToolSchema(EToolValueType::String); Text.MaxSize = 64 * 1024;
                    Tool.Output.Properties.emplace("state", std::move(Status));
                    Tool.Output.Properties.emplace("output", Text); Tool.Output.Properties.emplace("error", Text);
                    Tool.Output.Required = {"job_id", "state", "output", "error"};
                }
                else
                {
                    Tool.Description = "Request cooperative cancellation. Does not imply termination or rollback; query status afterward.";
                    Tool.Effects = EToolEffect::MutatesMemory;
                    Tool.Output.Properties.emplace("cancellation_requested", FToolSchema(EToolValueType::Boolean));
                    Tool.Output.Required = {"job_id", "cancellation_requested"};
                }
            }
            Bindings.push_back({std::move(Tool), [State](const auto& Request, const auto& Context) { return State->Call(Request, Context); }});
        }
        return std::make_unique<FNativeToolset>("editor.python", "Python script jobs for the admitting editor and client.", std::move(Bindings));
    }
}
