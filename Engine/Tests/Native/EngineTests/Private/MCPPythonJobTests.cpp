#include <httplib.h>
#include "MCPServer.h"
#include "MCPCodec.h"
#include "PythonRuntime.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/NativeToolset.h"
#include "Editor/EditorEngine.h"
#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
    class FMCPPythonTestEditor : public DEditorEngine
    {
        static auto GetPrivateStaticClass() -> DClass*;
        DECLARE_CLASS(FMCPPythonTestEditor, DEditorEngine, FMCPPythonTestEditor::GetPrivateStaticClass)
        DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FMCPPythonTestEditor)
    public:
        explicit FMCPPythonTestEditor(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
        auto Start() -> FSubsystemResult { return InitializeEditorSubsystems(); }
    };
    IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FMCPPythonTestEditor)
}
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
            Host = TStrongObjectPtr<FMCPPythonTestEditor>(NewObject<FMCPPythonTestEditor>(nullptr, {}));
            ASSERT_TRUE(Host->Start());
            ASSERT_TRUE(FModuleManager::Get().LoadModule("LevelEditor"));
            ASSERT_TRUE(Host->RefreshEditorSubsystems()); Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
            Runtime = std::make_shared<FPythonRuntime>();
            auto Started = Runtime->Start(); ASSERT_TRUE(Started) << Started.error();
            auto Listening = Server.Start(*Host, {.BearerToken = std::string(Token), .PythonRuntime = Runtime});
            ASSERT_TRUE(Listening) << Listening.error(); Port = *Listening; Session = Connect();
        }
        auto TearDown() -> void override
        {
            Server.Stop(); Runtime->Stop();
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
            EXPECT_EQ(200, Result->status) << Result->body; return Result->body;
        }
        auto Call(std::string_view Owner, std::string Tool, FObject Args = {}) -> std::string
        { return Post(Owner, "tools/call", {{"name", std::move(Tool)}, {"arguments", std::move(Args)}}); }
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
        TStrongObjectPtr<FMCPPythonTestEditor> Host;
        DEditorToolSubsystem* Tools = nullptr;
        uint16 Port = 0;
        std::string Session;
    };

    TEST_F(FMCPPythonJobTests, AdvertisesJobsAndReturnsAdmissionBeforeEditorTick)
    {
        auto Listed = Post(Session, "tools/list");
        for (const auto Name : {"editor.python.submit", "editor.python.status", "editor.python.cancel", "editor.pie.status"})
            EXPECT_NE(std::string::npos, Listed.find(Name));
        auto Id = Submit("import durin\nprint(durin.tools.call('editor.pie', 'status', {})['state'])");
        auto Snapshot = Call(Session, "editor.python.status", {{"job_id", Id}});
        EXPECT_FALSE(IsError(Snapshot)); EXPECT_NE("succeeded", DataString(Snapshot, "state"));
        auto Result = Wait(Id); EXPECT_EQ("succeeded", DataString(Result, "state")); EXPECT_EQ("stopped\n", DataString(Result, "output"));
    }
    TEST_F(FMCPPythonJobTests, ReportsFailureAsAJobSnapshotWithDiagnosticFilename)
    {
        auto Submitted = Call(Session, "editor.python.submit", {{"code", "raise ValueError('script failed')"}, {"filename", "agent_script.py"}});
        ASSERT_FALSE(IsError(Submitted)); auto Result = Wait(DataString(Submitted, "job_id"), false);
        EXPECT_FALSE(IsError(Result)); EXPECT_EQ("failed", DataString(Result, "state"));
        EXPECT_NE(std::string::npos, DataString(Result, "error").find("agent_script.py"));
        EXPECT_NE(std::string::npos, DataString(Result, "error").find("ValueError: script failed"));
    }
    TEST_F(FMCPPythonJobTests, CancelsRunningAndQueuedJobsWithoutAnEditorTick)
    {
        auto Running = Submit("while True:\n    pass\n");
        auto Queued = Submit("print('must not run')");
        auto Cancelled = Call(Session, "editor.python.cancel", {{"job_id", Queued}}); EXPECT_FALSE(IsError(Cancelled));
        EXPECT_EQ("cancelled", DataString(Wait(Queued, false), "state"));
        EXPECT_FALSE(IsError(Call(Session, "editor.python.cancel", {{"job_id", Running}})));
        EXPECT_EQ("cancelled", DataString(Wait(Running, false), "state"));
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
            FObject{{"code", std::string(65537, ' ')}}, FObject{{"code", "pass"}, {"filename", std::string(4097, 'x')}}})
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
            auto Snapshot = Runtime->GetJobResult(*LocalJob); if (Snapshot && Snapshot->State == EPythonJobState::Succeeded) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto Result = Wait(Submit("import durin\nprint(durin.ui_marker)"), false);
        EXPECT_EQ("shared interpreter\n", DataString(Result, "output"));
        EXPECT_TRUE(IsError(Call(Session, "editor.python.status", {{"job_id", std::to_string(*LocalJob)}})));
        auto Id = Submit("while True:\n    pass\n");
        std::atomic<bool> Done = false;
        auto Reader = std::async(std::launch::async, [&] {
            while (!Done) { (void)Call(Session, "editor.python.status", {{"job_id", Id}}); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        });
        Runtime->Stop(); Done = true; Reader.get();
        EXPECT_TRUE(IsError(Call(Session, "editor.python.status", {{"job_id", Id}})));
        EXPECT_NE(std::string::npos, Post(Session, "ping").find("result"));
    }
    TEST_F(FMCPPythonJobTests, OmitsScriptToolsWithoutARuntimeAndRejectsCatalogCollisions)
    {
        Server.Stop();
        auto Without = Server.Start(*Host, {.BearerToken = std::string(Token)}); ASSERT_TRUE(Without); Port = *Without; Session = Connect();
        EXPECT_EQ(std::string::npos, Post(Session, "tools/list").find("editor.python."));
        Server.Stop(); auto With = Server.Start(*Host, {.BearerToken = std::string(Token), .PythonRuntime = Runtime});
        ASSERT_TRUE(With); Port = *With; Session = Connect();
        FToolDescriptor Descriptor{.Name = "submit", .bExposeExternally = true}; FToolBinding Binding{Descriptor};
        Binding.Handler = [](const auto&, const auto&) { return FToolResult{EToolStatus::Succeeded, FObject{}}; };
        FNativeToolset Collision("editor.python", "collision", {std::move(Binding)});
        auto Registration = FModularFeatureRegistry::Get().RegisterFeature(Collision); (void)Tools->ListToolsets();
        httplib::Client Client("127.0.0.1", Port); auto Listed = Client.Post("/mcp", Headers(Session), Body("tools/list"), "application/json");
        ASSERT_TRUE(Listed); EXPECT_EQ(500, Listed->status); EXPECT_NE(std::string::npos, Listed->body.find("collision"));
    }
}
