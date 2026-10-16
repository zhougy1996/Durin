#include "PythonRuntime.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/NativeToolset.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"
#include <gtest/gtest.h>

namespace Durin
{
    class FPythonTestEditor : public DEditorEngine
    {
        static auto GetPrivateStaticClass() -> DClass*;
        DECLARE_CLASS(FPythonTestEditor, DEditorEngine, FPythonTestEditor::GetPrivateStaticClass)
        DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FPythonTestEditor)
    public:
        explicit FPythonTestEditor(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
        auto Start() -> FSubsystemResult { return InitializeEditorSubsystems(); }
    };
    IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FPythonTestEditor)
    struct FEditorEngineTestAccess
    {
        static auto SetEditorWorld(DEditorEngine& Editor, DWorld* World) -> void { Editor.EditorWorld = World; Editor.SetWorld(World); }
    };
}

namespace
{
    using namespace Durin;
    class FPythonToolIntegrationTests : public testing::Test
    {
    protected:
        auto SetUp() -> void override
        {
            Testing::InitializeDObjectSystemForTests();
            Host = TStrongObjectPtr<FPythonTestEditor>(NewObject<FPythonTestEditor>(nullptr, {}));
            ASSERT_TRUE(Host->Start()); Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
            auto Started = Runtime.Start(); ASSERT_TRUE(Started) << Started.error();
            auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        }
        auto TearDown() -> void override
        {
            Runtime.Stop(); Registration.Reset(); Toolset.reset();
            Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); CollectGarbage();
        }
        auto Submit(std::string Code) -> uint64
        {
            auto Job = Runtime.RunString(Session, std::move(Code), "integration.py");
            EXPECT_TRUE(Job) << (Job ? "" : Job.error()); return Job.value_or(0);
        }
        auto Wait(uint64 Id, bool Pump = true) -> FPythonJobResult
        {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < Deadline)
            {
                auto Job = Runtime.GetJobResult(Id);
                if (!Job) { ADD_FAILURE() << Job.error(); return {}; }
                if (Job->State != EPythonJobState::Queued && Job->State != EPythonJobState::Running) return *Job;
                if (Pump) Tools->TickTools();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Runtime.CancelJob(Id); ADD_FAILURE() << "Python job did not finish."; return {};
        }
        auto WaitRunning(uint64 Id) -> void
        {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < Deadline)
            {
                auto Result = Runtime.GetJobResult(Id);
                if (Result && Result->State == EPythonJobState::Running) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL() << "Job never entered running state.";
        }
        auto Register(FToolSchema Input, bool Exposed = true,
            std::function<FToolResult(const FToolRequest&, const FToolContext&)> Handler = {},
            std::optional<FToolSchema> Output = {}) -> void
        {
            FToolDescriptor Descriptor{.Name = "echo", .Input = Input, .Output = Output.value_or(Input), .bExposeExternally = Exposed};
            FToolBinding Binding{Descriptor};
            Binding.Handler = [this, Handler](const auto& Request, const auto& Context) {
                EXPECT_TRUE(IsInGameThread()); if (!Handler) EXPECT_EQ(Host.Get(), GetToolEditor(Context));
                EXPECT_EQ(EToolSource::External, Context.Source); ++Calls;
                return Handler ? Handler(Request, Context) : FToolResult{EToolStatus::Succeeded, Request.Arguments};
            };
            Toolset = std::make_unique<FNativeToolset>("python.tests", "Python adapter tests", std::vector<FToolBinding>{std::move(Binding)});
            Registration = FModularFeatureRegistry::Get().RegisterFeature(*Toolset);
            (void)Tools->ListToolsets();
        }
        FPythonRuntime Runtime;
        TStrongObjectPtr<FPythonTestEditor> Host;
        DEditorToolSubsystem* Tools = nullptr;
        uint64 Session = 0;
        std::unique_ptr<FNativeToolset> Toolset;
        FModularFeatureRegistration Registration;
        int Calls = 0;
    };

    TEST_F(FPythonToolIntegrationTests, ImportsDeployedPackageAndPreservesNativeExceptions)
    {
        auto Result = Wait(Submit(R"PY(
import durin
import _durin
import sys
assert durin.__spec__.submodule_search_locations is not None
assert 'durin-python.zip' in durin.__file__
assert _durin.__spec__.origin == 'built-in'
assert durin.ToolError is _durin.ToolError
assert durin.CancelledError is _durin.CancelledError
assert durin.ToolError.__module__ == 'durin'
assert durin.CancelledError.__module__ == 'durin'
assert issubclass(durin.ToolError, RuntimeError)
assert issubclass(durin.CancelledError, BaseException)
assert not issubclass(durin.CancelledError, Exception)
assert sys.stdout.write('package') == 7
sys.stdout.flush()
print(' imported')
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        EXPECT_EQ("package imported\n", Result.Output);
    }
    TEST_F(FPythonToolIntegrationTests, CapturesOutputAndUsesFreshGlobals)
    {
        auto First = Wait(Submit("x = 42\nprint('hello', x)"));
        EXPECT_EQ(EPythonJobState::Succeeded, First.State); EXPECT_EQ("hello 42\n", First.Output);
        auto Second = Wait(Submit("assert 'x' not in globals()\nprint('fresh')"));
        EXPECT_EQ(EPythonJobState::Succeeded, Second.State);
    }
    TEST_F(FPythonToolIntegrationTests, ReportsTracebacksSyntaxAndUtf8Failures)
    {
        auto Result = Wait(Submit("raise ValueError('broken')"));
        EXPECT_EQ(EPythonJobState::Failed, Result.State); EXPECT_NE(std::string::npos, Result.Error.find("integration.py"));
        EXPECT_NE(std::string::npos, Result.Error.find("ValueError: broken"));
        EXPECT_EQ(EPythonJobState::Failed, Wait(Submit("def invalid(:")).State);
        EXPECT_EQ(EPythonJobState::Failed, Wait(Submit(std::string("# ") + char(0xff))).State);
    }
    TEST_F(FPythonToolIntegrationTests, ReadsBoundedAbsoluteFiles)
    {
        const auto Root = Testing::CreateTestFixtureDirectory("PythonFiles");
        const auto Path = Root / "script.py";
        { std::ofstream File(Path); File << "print(__file__)"; }
        auto Job = Runtime.RunFile(Session, Path); ASSERT_TRUE(Job);
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(*Job).State);
        EXPECT_FALSE(Runtime.RunFile(Session, "relative.py")); EXPECT_FALSE(Runtime.RunFile(Session, Root / "missing.py"));
        { std::ofstream File(Path); File << std::string(1024 * 1024 + 1, ' '); }
        EXPECT_FALSE(Runtime.RunFile(Session, Path));
        EXPECT_FALSE(Runtime.RunString(Session, std::string(1024 * 1024 + 1, ' ')));
    }
    TEST_F(FPythonToolIntegrationTests, BoundsOutputAndRecoversForTheNextJob)
    {
        auto Result = Wait(Submit("print('x' * 70000)"));
        EXPECT_EQ(EPythonJobState::Failed, Result.State); EXPECT_LE(Result.Output.size(), 65536u);
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("print('recovered')")).State);
    }
    TEST_F(FPythonToolIntegrationTests, DiscoversAndCallsRealPIEThroughGameThread)
    {
        auto* World = NewObject<DWorld>(Host.Get(), "PythonEditorWorld"); World->SetWorldType(EWorldType::Editor);
        ASSERT_TRUE(World->InitializeSubsystems());
        auto* Level = NewObject<DLevel>(World, "PythonEditorLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
        FEditorEngineTestAccess::SetEditorWorld(*Host, World);
        auto Result = Wait(Submit(R"PY(
import durin
assert any(t['toolset'] == 'editor.pie' and t['tool'] == 'pause' for t in durin.tools.list())
assert durin.tools.describe('editor.pie', 'pause')['version'] == 1
assert durin.tools.call('editor.pie', 'status', {})['state'] == 'stopped'
assert durin.tools.call('editor.pie', 'play', {})['state'] == 'playing'
assert durin.tools.call('editor.pie', 'pause', {'paused': True})['state'] == 'paused'
assert durin.tools.call('editor.pie', 'stop', {})['state'] == 'stopped'
print('PIE complete')
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        EXPECT_EQ("PIE complete\n", Result.Output);
    }
    TEST_F(FPythonToolIntegrationTests, PreservesDomainAndArgumentDiagnosticsWithoutRetry)
    {
        auto Result = Wait(Submit(R"PY(
import durin
for args in ({'paused': True}, {'paused': 'yes'}):
    try: durin.tools.call('editor.pie', 'pause', args)
    except durin.ToolError as e:
        assert e.diagnostics and 'code' in e.diagnostics[0] and 'path' in e.diagnostics[0]
    else: raise AssertionError('expected failure')
try: durin.tools.call('editor.pie', 'status', {}, version=999)
except durin.ToolError: pass
else: raise AssertionError('expected version mismatch')
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
    }
    TEST_F(FPythonToolIntegrationTests, PreservesNumericBoundariesAndNestedValues)
    {
        FToolSchema Schema(EToolValueType::Object);
        Schema.Properties.emplace("signed", FToolSchema(EToolValueType::Integer));
        Schema.Properties.emplace("unsigned", FToolSchema(EToolValueType::UnsignedInteger));
        Schema.Properties.emplace("number", FToolSchema(EToolValueType::Number));
        Schema.Properties.emplace("flag", FToolSchema(EToolValueType::Boolean));
        Schema.Properties.emplace("text", FToolSchema(EToolValueType::String));
        auto& Array = Schema.Properties.emplace("values", FToolSchema(EToolValueType::Array)).first->second;
        Array.Items = std::make_unique<FToolSchema>(EToolValueType::Null);
        Register(Schema);
        auto Result = Wait(Submit(R"PY(
import durin
args = {'signed': -(2**63), 'unsigned': 2**64-1, 'number': 4, 'flag': True, 'text': '你好', 'values': [None]}
value = durin.tools.call('python.tests', 'echo', args)
assert value == args and type(value['number']) is float and type(value['flag']) is bool
args['signed'] = 2**63
try: durin.tools.call('python.tests', 'echo', args)
except OverflowError: pass
else: raise AssertionError('overflow')
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error; EXPECT_EQ(1, Calls);
    }
    TEST_F(FPythonToolIntegrationTests, RejectsCyclesUnsupportedValuesAndConversionBudgets)
    {
        Register(FToolSchema(EToolValueType::Object));
        auto Result = Wait(Submit(R"PY(
import durin
cycle = []; cycle.append(cycle)
deep = None
for i in range(40): deep = [deep]
for value in (cycle, deep, [None]*1025, 'x'*65537, {1: None}, object(), float('nan'), 2**65):
    try: durin.tools.submit('python.tests', 'echo', {'value': value})
    except (ValueError, OverflowError): pass
    else: raise AssertionError('invalid conversion accepted')
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error; EXPECT_EQ(0, Calls);
    }
    TEST_F(FPythonToolIntegrationTests, FiltersPrivateToolsAndCannotElevateSource)
    {
        Register(FToolSchema(EToolValueType::Object), false);
        auto Result = Wait(Submit(R"PY(
import durin
assert not any(t['toolset'] == 'python.tests' for t in durin.tools.list())
try: durin.tools.describe('python.tests', 'echo')
except durin.ToolError: pass
else: raise AssertionError('private tool exposed')
try: durin.tools.call('editor.pie', 'status', {}, source='Native')
except TypeError: pass
else: raise AssertionError('source override accepted')
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error; EXPECT_EQ(0, Calls);
    }
    TEST_F(FPythonToolIntegrationTests, CancelsQueuedInvocationsAndTimeoutsWithoutATick)
    {
        auto Result = Wait(Submit(R"PY(
import durin
handle = durin.tools.submit('editor.pie', 'status', {})
assert handle.state() == 'queued'
handle.cancel()
assert handle.state() == 'cancelled'
try: handle.result()
except durin.ToolError: pass
else: raise AssertionError('cancelled call succeeded')
try: durin.tools.call('editor.pie', 'status', {}, timeout=0.01)
except TimeoutError: pass
else: raise AssertionError('timeout did not occur')
)PY"), false);
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        EXPECT_TRUE(Tools->GetService()->Configure({}));
    }
    TEST_F(FPythonToolIntegrationTests, CancelsPythonLoopAndClosesWaitingSessionWithoutATick)
    {
        auto Loop = Submit("while True:\n    x = 1\n"); WaitRunning(Loop);
        EXPECT_TRUE(Runtime.CancelJob(Loop)); EXPECT_EQ(EPythonJobState::Cancelled, Wait(Loop, false).State);
        auto Waiting = Submit("import durin\ndurin.tools.call('editor.pie', 'status', {})"); WaitRunning(Waiting);
        Runtime.CloseSession(Session);
        EXPECT_EQ(EPythonJobState::Cancelled, Wait(Waiting, false).State);
        EXPECT_FALSE(Runtime.RunString(Session, "pass"));
        Runtime.Stop(); Runtime.Stop();
    }
    TEST_F(FPythonToolIntegrationTests, BoundsAdmissionAndPreventsCrossJobHandles)
    {
        auto Loop = Submit("while True:\n    pass\n"); WaitRunning(Loop);
        std::vector<uint64> Queued;
        for (int I = 0; I < 15; ++I) Queued.push_back(Submit("print('queued')"));
        EXPECT_FALSE(Runtime.RunString(Session, "pass"));
        for (auto Id : Queued) EXPECT_TRUE(Runtime.CancelJob(Id));
        Runtime.CancelJob(Loop); (void)Wait(Loop, false);
        auto First = Wait(Submit("import durin\ndurin.saved_tools = durin.tools\ndurin.saved = durin.tools.submit('editor.pie', 'status', {})"), false);
        EXPECT_EQ(EPythonJobState::Succeeded, First.State);
        auto Second = Wait(Submit(R"PY(
import durin
try: durin.saved_tools.list()
except ValueError: pass
else: raise AssertionError('stale tools facade accepted')
try: durin.saved.result()
except ValueError: pass
else: raise AssertionError('cross-job handle accepted')
del durin.saved
del durin.saved_tools
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Second.State) << Second.Error;
    }
    TEST_F(FPythonToolIntegrationTests, RejectsRetiredProviderWithoutRedirectingAcceptedWork)
    {
        Register(FToolSchema(EToolValueType::Object));
        auto Job = Submit("import durin\nh = durin.tools.submit('python.tests', 'echo', {})\nprint('submitted', flush=True)\nprint(h.result())");
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < Deadline)
        {
            auto State = Runtime.GetJobResult(Job); if (State && State->Output.find("submitted") != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Registration.Reset();
        auto Result = Wait(Job); EXPECT_EQ(EPythonJobState::Failed, Result.State); EXPECT_EQ(0, Calls);
    }
    TEST_F(FPythonToolIntegrationTests, RejectsMissingRuntimeAndConcurrentInterpreterOwnership)
    {
        FPythonRuntime Other; EXPECT_FALSE(Other.Start());
        Runtime.Stop();
        EXPECT_FALSE(Other.Start({Testing::CreateTestFixtureDirectory("MissingPython")}));
        auto Restarted = Runtime.Start(); ASSERT_TRUE(Restarted) << Restarted.error();
        auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("print('restart')")).State);
    }
    TEST_F(FPythonToolIntegrationTests, DistinguishesSuccessfulNullAndInvalidNativeOutput)
    {
        Register(FToolSchema(EToolValueType::Object), true,
            [](const auto&, const auto&) { return FToolResult{EToolStatus::Succeeded, nullptr}; }, FToolSchema(EToolValueType::Null));
        auto Null = Wait(Submit("import durin\nassert durin.tools.call('python.tests', 'echo', {}) is None"));
        EXPECT_EQ(EPythonJobState::Succeeded, Null.State) << Null.Error;
        Registration.Reset(); Toolset.reset();
        Register(FToolSchema(EToolValueType::Object), true,
            [](const auto&, const auto&) { return FToolResult{EToolStatus::Succeeded, true}; });
        auto Invalid = Wait(Submit("import durin\ndurin.tools.call('python.tests', 'echo', {})"));
        EXPECT_EQ(EPythonJobState::Failed, Invalid.State); EXPECT_EQ(2, Calls);
    }
    TEST_F(FPythonToolIntegrationTests, AppliesDefaultsOnceAndPreservesExplicitNull)
    {
        FToolSchema Schema(EToolValueType::Object);
        auto& Number = Schema.Properties.emplace("value", FToolSchema(EToolValueType::Integer)).first->second;
        Number.Default = 42;
        Register(Schema);
        auto Result = Wait(Submit(R"PY(
import durin
assert durin.tools.call('python.tests', 'echo', {}) == {'value': 42}
try: durin.tools.call('python.tests', 'echo', {'value': None})
except durin.ToolError: pass
else: raise AssertionError('null was treated as absent')
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error; EXPECT_EQ(1, Calls);
    }
    TEST_F(FPythonToolIntegrationTests, CancelsRunningHandlerCooperatively)
    {
        std::atomic<bool> Running = false, SawCancellation = false;
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto&, const FToolContext& Context) {
            Running = true;
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!Context.Cancellation.IsCancellationRequested() && std::chrono::steady_clock::now() < Deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            SawCancellation = Context.Cancellation.IsCancellationRequested();
            return FToolResult{EToolStatus::Succeeded, FToolValue::FObject{}};
        });
        auto Job = Submit("import durin\ndurin.tools.call('python.tests', 'echo', {})");
        auto Canceller = std::async(std::launch::async, [&] {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!Running && std::chrono::steady_clock::now() < Deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (Running) Runtime.CancelJob(Job);
        });
        auto Result = Wait(Job); Canceller.get();
        EXPECT_TRUE(SawCancellation); EXPECT_EQ(EPythonJobState::Cancelled, Result.State); EXPECT_EQ(1, Calls);
    }
    TEST_F(FPythonToolIntegrationTests, KeepsHostsAndClosedSessionsIndependent)
    {
        auto Other = TStrongObjectPtr<FPythonTestEditor>(NewObject<FPythonTestEditor>(nullptr, {}));
        ASSERT_TRUE(Other->Start()); auto* OtherTools = Other->GetEditorSubsystem<DEditorToolSubsystem>();
        FToolSchema Schema(EToolValueType::Object); Schema.Properties.emplace("host", FToolSchema(EToolValueType::Integer));
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto&, const FToolContext& Context) {
            return FToolResult{EToolStatus::Succeeded, FToolValue::FObject{{"host", GetToolEditor(Context) == Host.Get() ? 1 : 2}}};
        }, Schema);
        (void)OtherTools->ListToolsets();
        auto SecondSession = Runtime.CreateSession(OtherTools->GetService()); ASSERT_TRUE(SecondSession);
        auto A = Submit("import durin\nassert durin.tools.call('python.tests', 'echo', {})['host'] == 1");
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(A).State);
        const auto Old = Session; Runtime.CloseSession(Old); Session = *SecondSession;
        auto B = Submit("import durin\nassert durin.tools.call('python.tests', 'echo', {})['host'] == 2");
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < Deadline)
        {
            OtherTools->TickTools(); auto Job = Runtime.GetJobResult(B);
            if (Job && Job->State == EPythonJobState::Succeeded) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(B, false).State);
        EXPECT_FALSE(Runtime.RunString(Old, "pass"));
        Runtime.CloseSession(Session); Runtime.Stop();
        Other->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Other.Get()); Other.Reset(); CollectGarbage();
    }
    TEST_F(FPythonToolIntegrationTests, ReportsExpiredReceiptsAndBoundsRetention)
    {
        FEditorToolLimits Limits; Limits.ResultTTL = std::chrono::milliseconds(0); ASSERT_TRUE(Tools->GetService()->Configure(Limits));
        auto Expired = Wait(Submit("import durin\ndurin.tools.call('editor.pie', 'status', {})"));
        EXPECT_EQ(EPythonJobState::Failed, Expired.State); EXPECT_NE(std::string::npos, Expired.Error.find("ToolError"));
        auto First = Submit("pass"); (void)Wait(First);
        for (int I = 0; I < 65; ++I) EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("pass")).State);
        EXPECT_FALSE(Runtime.GetJobResult(First)); EXPECT_FALSE(Runtime.CancelJob(First));
    }
    TEST_F(FPythonToolIntegrationTests, ClosesWhileCapturingOutputAndWaitsForFiniteNativeWork)
    {
        auto Job = Submit("import time\nwhile True:\n    print('working')\n    time.sleep(0.005)\n"); WaitRunning(Job);
        Runtime.CloseSession(Session); EXPECT_EQ(EPythonJobState::Cancelled, Wait(Job, false).State);
        auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        auto Native = Submit("import time\nprint('sleeping', flush=True)\ntime.sleep(0.1)");
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < Deadline)
        {
            auto State = Runtime.GetJobResult(Native); if (State && State->Output.find("sleeping") != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Runtime.CancelJob(Native); EXPECT_EQ(EPythonJobState::Cancelled, Wait(Native, false).State);
    }
}
