#include <httplib.h>
#include "MCPServer.h"
#include "MCPCodec.h"
#include "PythonRuntime.h"
#include "PythonToolset.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/NativeToolset.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace
{
    using namespace Durin;
    using namespace Durin::MCP;
    constexpr std::string_view Token = "DurinMCPPythonJobTestsToken123456789";

    auto Body(std::string Method, FObject Params = {}, bool Notification = false) -> std::string
    {
        FObject Value{{"jsonrpc", "2.0"}, {"method", std::move(Method)}, {"params", std::move(Params)}};
        if (!Notification) Value.emplace("id", 7);
        return Serialize(std::move(Value));
    }
    auto DataString(const std::string& BodyText, std::string_view Name) -> std::string
    {
        FJsonDocument Document; EXPECT_TRUE(Document.Parse(BodyText));
        return Document.GetRootView().GetView("result").GetView("structuredContent").GetView("data").GetView(Name).GetString();
    }
    auto IsError(const std::string& BodyText) -> bool
    {
        FJsonDocument Document; EXPECT_TRUE(Document.Parse(BodyText));
        return Document.GetRootView().GetView("result").GetView("isError").GetBool(true);
    }
    class FMCPPythonJobTests : public testing::Test
    {
    protected:
        auto SetUp() -> void override
        {
            Testing::InitializeDObjectSystemForTests();
            Host = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
            ASSERT_TRUE(FEditorEngineTestAccess::StartEditor(*Host));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("LevelEditor"));
            ASSERT_TRUE(Host->RefreshEditorSubsystems()); Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
            Runtime = std::make_shared<FPythonRuntime>();
            auto Started = Runtime->Start(); ASSERT_TRUE(Started) << Started.error();
            PythonTools = CreatePythonToolset(Runtime);
            PythonRegistration = FModularFeatureRegistry::Get().RegisterFeature(*PythonTools);
            auto Listening = Server.Start(*Host, {.BearerToken = std::string(Token)});
            ASSERT_TRUE(Listening) << Listening.error(); Port = *Listening; Session = Connect();
        }
        auto TearDown() -> void override
        {
            Server.Stop(); PythonRegistration.Reset(); PythonTools.reset(); Runtime->Stop();
            Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); CollectGarbage();
        }
        auto Headers(std::string_view Owner) -> httplib::Headers
        {
            httplib::Headers Result{{"Authorization", "Bearer " + std::string(Token)},
                {"Accept", "application/json, text/event-stream"}, {"MCP-Protocol-Version", "2025-06-18"}};
            if (!Owner.empty()) Result.emplace("Mcp-Session-Id", Owner);
            return Result;
        }
        auto Connect() -> std::string
        {
            httplib::Client Client("127.0.0.1", Port); Client.set_read_timeout(3, 0);
            auto Result = Client.Post("/mcp", Headers(""), Body("initialize", {{"protocolVersion", ProtocolVersion},
                {"capabilities", FObject{}}, {"clientInfo", FObject{{"name", "python-test"}, {"version", "1"}}}}), "application/json");
            EXPECT_TRUE(Result); if (!Result) return {};
            EXPECT_EQ(200, Result->status); auto Id = Result->get_header_value("Mcp-Session-Id");
            auto Notification = Client.Post("/mcp", Headers(Id), Body("notifications/initialized", {}, true), "application/json");
            EXPECT_TRUE(Notification); if (Notification) EXPECT_EQ(202, Notification->status);
            return Id;
        }
        auto Post(std::string_view Owner, std::string Method, FObject Params = {}) -> std::string
        {
            httplib::Client Client("127.0.0.1", Port); Client.set_read_timeout(3, 0);
            auto Result = Client.Post("/mcp", Headers(Owner), Body(std::move(Method), std::move(Params)), "application/json");
            EXPECT_TRUE(Result); if (!Result) return {};
            EXPECT_EQ(200, Result->status) << Result->body;
            const auto Data = Result->body.find("data: ");
            if (Data == std::string::npos) return Result->body;
            return Result->body.substr(Data + 6, Result->body.find('\n', Data) - Data - 6);
        }
        auto Call(std::string_view Owner, std::string Tool, FObject Args = {}) -> std::string
        {
            auto Future = std::async(std::launch::async, [this, Owner = std::string(Owner), Tool = std::move(Tool), Args = std::move(Args)]() mutable {
                return Post(Owner, "tools/call", {{"name", std::move(Tool)}, {"arguments", std::move(Args)}});
            });
            while (Future.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
                if (IsInGameThread()) Tools->TickTools();
            return Future.get();
        }
        auto Submit(std::string Code, std::string_view Owner = {}) -> std::string
        {
            auto ReplyText = Call(Owner.empty() ? Session : Owner, "editor.python.submit", {{"code", std::move(Code)}});
            EXPECT_FALSE(IsError(ReplyText)); const auto Id = DataString(ReplyText, "job_id"); EXPECT_FALSE(Id.empty()); return Id;
        }
        auto Wait(std::string_view Id, bool Pump = true, std::string_view Owner = {}) -> std::string
        {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < Deadline)
            {
                if (Pump) Tools->TickTools();
                auto Result = Call(Owner.empty() ? Session : Owner, "editor.python.status", {{"job_id", std::string(Id)}});
                const auto State = DataString(Result, "state");
                if (State != "queued" && State != "running") return Result;
                Tools->TickScripts();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ADD_FAILURE() << "MCP Python job did not finish."; return {};
        }
        auto Delete(std::string_view Owner) -> void
        {
            httplib::Client Client("127.0.0.1", Port); auto Result = Client.Delete("/mcp", Headers(Owner));
            ASSERT_TRUE(Result); ASSERT_EQ(200, Result->status);
        }
        FMCPServer Server;
        std::shared_ptr<FPythonRuntime> Runtime;
        std::unique_ptr<FNativeToolset> PythonTools;
        FModularFeatureRegistration PythonRegistration;
        TStrongObjectPtr<DEditorEngine> Host;
        DEditorToolSubsystem* Tools = nullptr;
        uint16 Port = 0;
        std::string Session;
    };

    TEST_F(FMCPPythonJobTests, AdvertisesRegisteredToolsetAndReturnsAdmissionBeforeScriptCompletion)
    {
        auto Listed = Post(Session, "tools/list");
        for (const auto Name : {"editor.python.submit", "editor.python.status", "editor.python.cancel", "editor.pie.status"})
            EXPECT_NE(std::string::npos, Listed.find(Name));
        auto Id = Submit("import durin\nprint(durin.tools.call('editor.pie', 'status', {})['state'])");
        auto Snapshot = Call(Session, "editor.python.status", {{"job_id", Id}});
        EXPECT_FALSE(IsError(Snapshot));
        auto Result = Wait(Id); EXPECT_EQ("succeeded", DataString(Result, "state")); EXPECT_EQ("stopped\n", DataString(Result, "output"));
    }
    TEST_F(FMCPPythonJobTests, ReportsScriptFailureAsAJobSnapshot)
    {
        auto Submitted = Call(Session, "editor.python.submit", {{"code", "raise ValueError('script failed')"}});
        ASSERT_FALSE(IsError(Submitted)); auto Result = Wait(DataString(Submitted, "job_id"), false);
        EXPECT_FALSE(IsError(Result)); EXPECT_EQ("failed", DataString(Result, "state"));
        EXPECT_NE(std::string::npos, DataString(Result, "error").find("<durin>"));
        EXPECT_NE(std::string::npos, DataString(Result, "error").find("ValueError: script failed"));
    }
    TEST_F(FMCPPythonJobTests, CapturesProjectRelativeFilesBeforeExecutionAndAcceptsAbsoluteFiles)
    {
        struct FRestoreProject { std::string Old = FPaths::ProjectFile(); ~FRestoreProject() { (void)FPaths::SetProjectFile(Old); } } Restore;
        const auto Root = Testing::CreateTestFixtureDirectory("MCPPythonFiles");
        const auto Project = Root / "Test.dproject";
        { std::ofstream Stream(Project); Stream << "{}"; }
        ASSERT_TRUE(FPaths::SetProjectFile(Project.generic_string()));
        const auto Path = Root / "snapshot.py";
        { std::ofstream Stream(Path); Stream << '#' << std::string(70000, ' ') << "\nprint('original snapshot')\nprint(__file__)"; }
        const auto Blocker = Submit("while True:\n    pass\n");
        auto Submitted = Call(Session, "editor.python.submit", {{"file", "snapshot.py"}});
        ASSERT_FALSE(IsError(Submitted));
        { std::ofstream Stream(Path); Stream << "raise RuntimeError('modified after submission')"; }
        EXPECT_FALSE(IsError(Call(Session, "editor.python.cancel", {{"job_id", Blocker}})));
        auto Result = Wait(DataString(Submitted, "job_id"));
        EXPECT_EQ("succeeded", DataString(Result, "state"));
        EXPECT_NE(std::string::npos, DataString(Result, "output").find("original snapshot"));
        EXPECT_NE(std::string::npos, DataString(Result, "output").find("snapshot.py"));
        { std::ofstream Stream(Path); Stream << "print('absolute script')"; }
        auto Absolute = Call(Session, "editor.python.submit", {{"file", Path.generic_string()}});
        ASSERT_FALSE(IsError(Absolute)); EXPECT_EQ("absolute script\n", DataString(Wait(DataString(Absolute, "job_id")), "output"));
        { std::ofstream Stream(Path); Stream << "raise ValueError('file traceback')"; }
        auto Failure = Call(Session, "editor.python.submit", {{"file", "snapshot.py"}}); ASSERT_FALSE(IsError(Failure));
        auto Failed = Wait(DataString(Failure, "job_id")); EXPECT_EQ("failed", DataString(Failed, "state"));
        EXPECT_NE(std::string::npos, DataString(Failed, "error").find("snapshot.py"));
    }
    TEST_F(FMCPPythonJobTests, RejectsMissingOversizedAndConflictingFileInputsBeforeCreatingAJob)
    {
        const auto Root = Testing::CreateTestFixtureDirectory("MCPPythonInvalidFiles");
        const auto Path = Root / "large.py";
        { std::ofstream Stream(Path); Stream << std::string(1024 * 1024 + 1, ' '); }
        for (auto Args : {FObject{{"file", Path.generic_string()}}, FObject{{"file", (Root / "missing.py").generic_string()}},
            FObject{{"file", Path.generic_string()}, {"filename", "alias.py"}}})
            EXPECT_TRUE(IsError(Call(Session, "editor.python.submit", std::move(Args))));
    }
    TEST_F(FMCPPythonJobTests, CancelsRunningAndQueuedJobsThroughToolset)
    {
        auto Running = Submit("while True:\n    pass\n");
        auto Queued = Submit("print('must not run')");
        auto Controls = std::async(std::launch::async, [&] {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            std::string State;
            while (std::chrono::steady_clock::now() < Deadline)
            {
                State = DataString(Call(Session, "editor.python.status", {{"job_id", Running}}), "state");
                if (State == "running") break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_EQ("running", State);
            EXPECT_FALSE(IsError(Call(Session, "editor.python.cancel", {{"job_id", Queued}})));
            EXPECT_FALSE(IsError(Call(Session, "editor.python.cancel", {{"job_id", Running}})));
        });
        Tools->TickScripts(); Controls.get();
        EXPECT_EQ("cancelled", DataString(Wait(Running, false), "state"));
        EXPECT_EQ("cancelled", DataString(Wait(Queued, false), "state"));
        EXPECT_EQ("", DataString(Wait(Queued, false), "output"));
    }
    TEST_F(FMCPPythonJobTests, RejectsForeignUnknownAndMalformedJobIds)
    {
        auto Id = Submit("print('owner')"); auto Other = Connect();
        for (const auto Tool : {"editor.python.status", "editor.python.cancel"})
        {
            EXPECT_TRUE(IsError(Call(Other, Tool, {{"job_id", Id}})));
            EXPECT_TRUE(IsError(Call(Session, Tool, {{"job_id", "missing"}})));
            EXPECT_TRUE(IsError(Call(Session, Tool, {{"job_id", 7}})));
        }
        EXPECT_EQ("succeeded", DataString(Wait(Id, false), "state"));
    }
    TEST_F(FMCPPythonJobTests, ValidatesSubmissionAndPreservesExternalToolExposure)
    {
        for (auto Args : {FObject{}, FObject{{"code", 1}}, FObject{{"code", "pass"}, {"source", "Native"}},
            FObject{{"code", "pass"}, {"file", "script.py"}}, FObject{{"file", ""}}, FObject{{"file", 7}},
            FObject{{"code", std::string(65537, ' ')}}, FObject{{"code", "pass"}, {"filename", "legacy.py"}}})
            EXPECT_TRUE(IsError(Call(Session, "editor.python.submit", std::move(Args))));
        EXPECT_EQ("succeeded", DataString(Wait(Submit("#" + std::string(65535, ' ')), false), "state"));
        auto Result = Wait(Submit("import durin\ndurin.tools.call('editor.pie', 'pause', {'paused': True})"));
        EXPECT_EQ("failed", DataString(Result, "state")); EXPECT_NE(std::string::npos, DataString(Result, "error").find("No Play session"));
    }
    TEST_F(FMCPPythonJobTests, SessionDeleteAndServerStopCancelOnlyTheirOwnedWork)
    {
        auto Id = Submit("while True:\n    pass\n");
        auto Other = Connect(); auto Second = Submit("print('other session')", Other);
        Delete(Session);
        EXPECT_EQ("succeeded", DataString(Wait(Second, false, Other), "state"));
        EXPECT_TRUE(IsError(Call(Other, "editor.python.status", {{"job_id", Id}})));
        auto Third = Submit("while True:\n    pass\n", Other); (void)Third;
        Server.Stop();
        auto Local = Runtime->CreateSession(Tools->GetService()); ASSERT_TRUE(Local);
        auto LocalJob = Runtime->RunString(*Local, "print('runtime still alive')"); ASSERT_TRUE(LocalJob);
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < Deadline)
        {
            Tools->TickScripts();
            auto Job = Runtime->GetJobResult(*LocalJob);
            if (Job && Job->State == EPythonJobState::Succeeded) { EXPECT_EQ("runtime still alive\n", Job->Output); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        FAIL() << "Server stop affected unrelated runtime ownership.";
    }
    TEST_F(FMCPPythonJobTests, KeepsJobsAfterHttpDisconnectAndRequestCancellation)
    {
        auto Id = Submit("import durin\nprint(durin.tools.call('editor.pie', 'status', {})['state'])");
        httplib::Client Client("127.0.0.1", Port);
        auto Notification = Client.Post("/mcp", Headers(Session), Body("notifications/cancelled", {{"requestId", 7}}, true), "application/json");
        ASSERT_TRUE(Notification); EXPECT_EQ(202, Notification->status);
        EXPECT_EQ("succeeded", DataString(Wait(Id), "state"));
    }
    TEST_F(FMCPPythonJobTests, ReturnsValidUtf8AtOutputAndTracebackBudgets)
    {
        for (const auto Code : {"print('你' * 22000)", "raise ValueError('你' * 22000)"})
        {
            auto Result = Wait(Submit(Code), false); EXPECT_EQ("failed", DataString(Result, "state"));
            for (const auto Field : {"output", "error"})
            {
                auto TextValue = DataString(Result, Field); EXPECT_LE(TextValue.size(), 65536u);
                auto Parsed = ParseRequest(Body("ping", {{"text", TextValue}}));
                EXPECT_TRUE(Parsed) << (Parsed ? "" : Parsed.error().Message);
            }
        }
    }
    TEST_F(FMCPPythonJobTests, EvictsOldCompletedHandlesWithoutBlockingNewJobs)
    {
        auto First = Submit("pass"); EXPECT_EQ("succeeded", DataString(Wait(First, false), "state"));
        for (int I = 0; I < 65; ++I) EXPECT_EQ("succeeded", DataString(Wait(Submit("pass"), false), "state"));
        EXPECT_TRUE(IsError(Call(Session, "editor.python.status", {{"job_id", First}})));
    }
    TEST_F(FMCPPythonJobTests, EnforcesOutstandingJobLimitAndReleasesQueuedWork)
    {
        auto Running = Submit("while True:\n    pass\n"); (void)Running;
        for (int I = 0; I < 15; ++I) EXPECT_FALSE(Submit("print('queued')").empty());
        EXPECT_TRUE(IsError(Call(Session, "editor.python.submit", {{"code", "pass"}})));
        Delete(Session); Session = Connect();
        EXPECT_EQ("succeeded", DataString(Wait(Submit("print('new session')"), false), "state"));
    }
    TEST_F(FMCPPythonJobTests, CoexistsWithUiSessionsAndHandlesRuntimeOwnerStop)
    {
        auto Local = Runtime->CreateSession(Tools->GetService()); ASSERT_TRUE(Local);
        auto LocalJob = Runtime->RunString(*Local, "import durin\ndurin.ui_marker = 'shared interpreter'"); ASSERT_TRUE(LocalJob);
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < Deadline)
        {
            Tools->TickScripts();
            auto Snapshot = Runtime->GetJobResult(*LocalJob); if (Snapshot && Snapshot->State == EPythonJobState::Succeeded) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto Result = Wait(Submit("import durin\nprint(durin.ui_marker)"), false);
        EXPECT_EQ("shared interpreter\n", DataString(Result, "output"));
        EXPECT_TRUE(IsError(Call(Session, "editor.python.status", {{"job_id", std::to_string(*LocalJob)}})));
        auto Id = Submit("while True:\n    pass\n");
        auto Reader = std::async(std::launch::async, [&] {
            return Call(Session, "editor.python.status", {{"job_id", Id}});
        });
        Runtime->Stop();
        while (Reader.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) Tools->TickTools();
        EXPECT_TRUE(IsError(Reader.get()));
        EXPECT_TRUE(IsError(Call(Session, "editor.python.status", {{"job_id", Id}})));
        EXPECT_NE(std::string::npos, Post(Session, "ping").find("result"));
    }
    TEST_F(FMCPPythonJobTests, DiscoversPythonOnlyThroughRegisteredToolsetsAndRejectsAmbiguousProviders)
    {
        Server.Stop();
        PythonRegistration.Reset(); (void)Tools->ListToolsets();
        auto Without = Server.Start(*Host, {.BearerToken = std::string(Token)}); ASSERT_TRUE(Without); Port = *Without; Session = Connect();
        EXPECT_EQ(std::string::npos, Post(Session, "tools/list").find("editor.python."));
        PythonRegistration = FModularFeatureRegistry::Get().RegisterFeature(*PythonTools);
        Server.Stop(); auto With = Server.Start(*Host, {.BearerToken = std::string(Token)});
        ASSERT_TRUE(With); Port = *With; Session = Connect();
        FToolDescriptor Descriptor{.Name = "submit", .bExposeExternally = true}; FToolBinding Binding{Descriptor};
        Binding.Handler = [](const auto&, const auto&) { return FToolResult{EToolStatus::Succeeded, FObject{}}; };
        FNativeToolset Collision("editor.python", "collision", {std::move(Binding)});
        auto Registration = FModularFeatureRegistry::Get().RegisterFeature(Collision); (void)Tools->ListToolsets();
        httplib::Client Client("127.0.0.1", Port); auto Listed = Client.Post("/mcp", Headers(Session), Body("tools/list"), "application/json");
        ASSERT_TRUE(Listed); EXPECT_EQ(200, Listed->status);
        EXPECT_EQ(std::string::npos, Listed->body.find("editor.python.submit"));
        EXPECT_FALSE(Tools->GetService()->GetCatalog().Diagnostics.empty());
    }
}
