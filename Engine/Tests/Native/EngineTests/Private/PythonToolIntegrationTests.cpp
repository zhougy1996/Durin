#include "PythonRuntime.h"
#include "PythonToolset.h"
#include "Components/SceneComponent.h"
#include "Transactions/Transactor.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/NativeToolset.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"
#include <gtest/gtest.h>


namespace
{
    using namespace Durin;
    class FPythonToolIntegrationTests : public testing::Test
    {
    protected:
        auto SetUp() -> void override
        {
            Testing::InitializeDObjectSystemForTests();
            Host = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
            ASSERT_TRUE(FEditorEngineTestAccess::StartEditor(*Host));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("LevelEditor"));
            ASSERT_TRUE(Host->RefreshEditorSubsystems()); Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
            auto Started = Runtime.Start(); ASSERT_TRUE(Started) << Started.error();
            auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        }
        auto TearDown() -> void override
        {
            Runtime.Stop(); PendingRegistration.Reset(); PendingToolset.reset(); Registration.Reset(); Toolset.reset();
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
                Tools->TickScripts();
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
        auto WhileRunning(uint64 Id, std::function<void()> Action) -> std::future<void>
        {
            return std::async(std::launch::async, [this, Id, Action = std::move(Action)] { WaitRunning(Id); Action(); });
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
        auto RegisterPending(FToolFuture Future, FToolSchema Output = FToolSchema(EToolValueType::Object)) -> void
        {
            FToolDescriptor Descriptor{.Name = "read", .Output = std::move(Output), .bExposeExternally = true, .bMayPend = true};
            FToolBinding Binding{Descriptor};
            Binding.AsyncHandler = [this, Future](const auto&, const auto& Context) {
                EXPECT_TRUE(IsInGameThread()); EXPECT_NE(nullptr, GetToolEditor(Context));
                PendingCancellation = Context.Cancellation; ++PendingCalls; return Future;
            };
            PendingToolset = std::make_unique<FNativeToolset>("python.pending", "Test-only detached preparation", std::vector<FToolBinding>{std::move(Binding)});
            PendingRegistration = FModularFeatureRegistry::Get().RegisterFeature(*PendingToolset); (void)Tools->ListToolsets();
        }
        FToolCancellationToken PendingCancellation;
        int PendingCalls = 0;
        std::unique_ptr<FNativeToolset> PendingToolset;
        FModularFeatureRegistration PendingRegistration;
        FPythonRuntime Runtime;
        TStrongObjectPtr<DEditorEngine> Host;
        DEditorToolSubsystem* Tools = nullptr;
        uint64 Session = 0;
        std::unique_ptr<FNativeToolset> Toolset;
        FModularFeatureRegistration Registration;
        int Calls = 0;
    };

    TEST_F(FPythonToolIntegrationTests, DefersInactiveReflectionDependenciesUntilModuleActivation)
    {
        const auto Engine = FModuleManager::Get().FindModule("Engine");
        if (!Engine || Engine->State != EModuleState::Active)
        {
            const auto Before = Wait(Submit(R"(
import durin
assert not hasattr(durin, 'EditorActorSubsystem')
try:
    durin.get_type('Durin::DEditorActorSubsystem')
except durin.ScriptAccessError:
    pass
else:
    raise AssertionError('inactive native dependency was exported')
)"));
            ASSERT_EQ(Before.State, EPythonJobState::Succeeded) << Before.Error;
        }
        ASSERT_TRUE(FModuleManager::Get().LoadModule("Engine"));
        const auto After = Wait(Submit(R"(
import durin
assert durin.get_type('Durin::DEditorActorSubsystem') is durin.EditorActorSubsystem
assert durin.get_editor_subsystem(durin.EditorActorSubsystem).get_editor_level() is None
)"));
        EXPECT_EQ(After.State, EPythonJobState::Succeeded) << After.Error;
    }
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
    TEST_F(FPythonToolIntegrationTests, ScriptJobsUseTheSameNativeToolsetWithoutMCP)
    {
        auto Definition = CreatePythonToolset(std::shared_ptr<FPythonRuntime>(&Runtime, [](auto*) {}));
        auto Published = FModularFeatureRegistry::Get().RegisterFeature(*Definition);
        auto Description = Tools->DescribeToolset("editor.python"); ASSERT_TRUE(Description);
        ASSERT_EQ(3u, Description->Descriptor.Tools.size());
        for (const auto& Tool : Description->Descriptor.Tools) EXPECT_TRUE(ValidateToolDescriptor(Tool));
        const auto Call = [&](std::string Name, FToolValue::FObject Args) {
            return Tools->CallNow({{"editor.python", std::move(Name)}, 1, std::move(Args)});
        };
        EXPECT_EQ(EToolStatus::Failed, Call("submit", {{"code", "pass"}, {"file", "script.py"}}).Status);
        auto Submitted = Call("submit", {{"code", "while True:\n    pass\n"}});
        ASSERT_EQ(EToolStatus::Succeeded, Submitted.Status);
        const auto Id = std::get<FToolValue::FObject>(Submitted.Data.Data).at("job_id");
        EXPECT_EQ(EToolStatus::Succeeded, Call("status", {{"job_id", Id}}).Status);
        EXPECT_EQ(EToolStatus::Succeeded, Call("cancel", {{"job_id", Id}}).Status);
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::string State;
        while (std::chrono::steady_clock::now() < Deadline)
        {
            auto Result = Call("status", {{"job_id", Id}});
            State = std::get<std::string>(std::get<FToolValue::FObject>(Result.Data.Data).at("state").Data);
            if (State == "cancelled") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ("cancelled", State);
    }
    TEST_F(FPythonToolIntegrationTests, DetachedClientClosureCancelsItsJobsWithoutAnotherEditorTick)
    {
        FToolCancellationSource Lifetime;
        auto Client = Runtime.CreateSession(Tools->GetService(), EToolSource::External, {"detached-client", Lifetime.GetToken()});
        ASSERT_TRUE(Client);
        auto Running = Runtime.RunString(*Client, "while True:\n    pass\n"); ASSERT_TRUE(Running);
        auto Queued = Runtime.RunString(*Client, "print('must not execute')"); ASSERT_TRUE(Queued);
        auto Closer = WhileRunning(*Running, [&] { Lifetime.Cancel(); });
        EXPECT_EQ(EPythonJobState::Cancelled, Wait(*Running, false).State); Closer.get();
        auto Snapshot = Wait(*Queued, false); EXPECT_EQ(EPythonJobState::Cancelled, Snapshot.State); EXPECT_TRUE(Snapshot.Output.empty());
        EXPECT_FALSE(Runtime.RunString(*Client, "pass"));
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("print('other session')"), false).State);
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
    TEST_F(FPythonToolIntegrationTests, ImmediateHandlesNeverWaitForAToolTick)
    {
        auto Result = Wait(Submit(R"PY(
import durin
handle = durin.tools.submit('editor.pie', 'status', {})
assert handle.state() == 'completed'
handle.cancel()
assert handle.state() == 'completed'
assert handle.result()['state'] == 'stopped'
assert durin.tools.call('editor.pie', 'status', {})['state'] == 'stopped'
)PY"), false);
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        EXPECT_TRUE(Tools->GetService()->Configure({}));
    }
    TEST_F(FPythonToolIntegrationTests, CancelsRunningPythonAndClosesSessionWithoutAToolTick)
    {
        auto Loop = Submit("while True:\n    x = 1\n");
        auto Canceller = WhileRunning(Loop, [&] { EXPECT_TRUE(Runtime.CancelJob(Loop)); });
        EXPECT_EQ(EPythonJobState::Cancelled, Wait(Loop, false).State); Canceller.get();
        auto Waiting = Submit("while True:\n    pass\n");
        auto Closer = WhileRunning(Waiting, [&] { Runtime.CloseSession(Session); });
        EXPECT_EQ(EPythonJobState::Cancelled, Wait(Waiting, false).State); Closer.get();
        EXPECT_FALSE(Runtime.RunString(Session, "pass"));
        Runtime.Stop(); Runtime.Stop();
    }
    TEST_F(FPythonToolIntegrationTests, BoundsAdmissionAndPreventsCrossJobHandles)
    {
        auto Loop = Submit("while True:\n    pass\n");
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
        auto Job = Submit("import durin\nh = durin.tools.submit('python.tests', 'echo', {})\nprint(h.result())");
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
    TEST_F(FPythonToolIntegrationTests, KeepsClosedClientSessionsIndependentOnOneEditor)
    {
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto&, const FToolContext& Context) {
            EXPECT_EQ(GetToolEditor(Context), Host.Get());
            EXPECT_TRUE(Host->GetTransactor()->IsActive());
            return FToolResult{EToolStatus::Succeeded, FToolValue::FObject{}};
        });
        auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created);
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("import durin\nwith durin.ScopedEditorTransaction('First client'):\n    durin.tools.call('python.tests', 'echo', {})")).State);
        const auto Closed = Session; Runtime.CloseSession(Closed); Session = *Created;
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("import durin\nwith durin.ScopedEditorTransaction('Second client'):\n    durin.tools.call('python.tests', 'echo', {})")).State);
        EXPECT_FALSE(Runtime.RunString(Closed, "pass"));
        EXPECT_EQ(GEditor, Host.Get());
    }
    TEST_F(FPythonToolIntegrationTests, ImmediateHandlesIgnoreServiceReceiptTTLAndJobsBoundRetention)
    {
        FEditorToolLimits Limits; Limits.ResultTTL = std::chrono::milliseconds(0); ASSERT_TRUE(Tools->GetService()->Configure(Limits));
        auto Expired = Wait(Submit("import durin\ndurin.tools.call('editor.pie', 'status', {})"));
        EXPECT_EQ(EPythonJobState::Succeeded, Expired.State) << Expired.Error;
        auto First = Submit("pass"); (void)Wait(First);
        for (int I = 0; I < 65; ++I) EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("pass")).State);
        EXPECT_FALSE(Runtime.GetJobResult(First)); EXPECT_FALSE(Runtime.CancelJob(First));
    }
    TEST_F(FPythonToolIntegrationTests, ClosesWhileCapturingOutputAndWaitsForFiniteNativeWork)
    {
        auto Job = Submit("import time\nwhile True:\n    print('working')\n    time.sleep(0.005)\n");
        auto Closer = WhileRunning(Job, [&] { Runtime.CloseSession(Session); });
        EXPECT_EQ(EPythonJobState::Cancelled, Wait(Job, false).State); Closer.get();
        auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        auto Native = Submit("import time\nprint('sleeping', flush=True)\ntime.sleep(0.1)");
        auto Canceller = WhileRunning(Native, [&] {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < Deadline)
            {
                auto State = Runtime.GetJobResult(Native); if (State && State->Output.find("sleeping") != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Runtime.CancelJob(Native);
            EXPECT_EQ(EPythonJobState::Running, Runtime.GetJobResult(Native)->State);
        });
        EXPECT_EQ(EPythonJobState::Cancelled, Wait(Native, false).State); Canceller.get();
    }

    TEST_F(FPythonToolIntegrationTests, NestedScopesGroupResultDependentNativeEditsIntoOneHistoryEntry)
    {
        auto* Object = NewObject<DSceneComponent>(Host.Get(), "ScriptTarget", EObjectFlags::Transactional);
        FToolSchema Input(EToolValueType::Object); Input.Properties.emplace("value", FToolSchema(EToolValueType::Integer));
        auto Output = Input; Output.Required = {"value"};
        Register(Input, true, [&](const auto& Request, const auto&) {
            const auto& Args = std::get<FToolValue::FObject>(Request.Arguments.Data);
            if (Args.contains("value"))
            {
                Editor::FScopedTransaction Scope(Host->GetTransactor(), {.Description = "Native domain edit"});
                EXPECT_TRUE(Object->Modify());
                Object->SetRelativeLocation(FVector3(static_cast<double>(std::get<int64>(Args.at("value").Data)), 0, 0));
            }
            return FToolResult{EToolStatus::Succeeded, FToolValue::FObject{{"value", static_cast<int64>(Object->GetRelativeLocation().x)}}};
        }, Output);
        const auto Job = Submit(R"PY(
import durin
with durin.ScopedEditorTransaction('Script grouped edits') as outer:
    first = durin.tools.call('python.tests', 'echo', {'value': 1})['value']
    with durin.ScopedEditorTransaction('Inner'):
        second = durin.tools.call('python.tests', 'echo', {'value': first + 1})['value']
    assert durin.tools.call('python.tests', 'echo', {'value': second + 1})['value'] == 3
assert outer.__exit__(None, None, None) is None
)PY");
        auto Result = Wait(Job, false); EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        auto* Trans = Host->GetTransactor(); ASSERT_EQ(1u, Trans->GetQueueLength());
        EXPECT_EQ("Script grouped edits", Trans->GetTransaction(0)->GetContext().Description);
        EXPECT_EQ(3.0, Object->GetRelativeLocation().x); EXPECT_FALSE(Trans->IsActive());
        ASSERT_TRUE(Trans->Undo()); EXPECT_EQ(0.0, Object->GetRelativeLocation().x);
        ASSERT_TRUE(Trans->Redo()); EXPECT_EQ(3.0, Object->GetRelativeLocation().x);
    }
    TEST_F(FPythonToolIntegrationTests, ExceptionsEndScopesAndInnerCancelDiscardsRecordingWithoutRollback)
    {
        auto* Object = NewObject<DSceneComponent>(Host.Get(), "ScopeTarget", EObjectFlags::Transactional);
        FToolSchema Input(EToolValueType::Object); Input.Properties.emplace("value", FToolSchema(EToolValueType::Integer));
        Register(Input, true, [&](const auto& Request, const auto&) {
            (void)Object->Modify();
            Object->SetRelativeLocation(FVector3(static_cast<double>(std::get<int64>(std::get<FToolValue::FObject>(Request.Arguments.Data).at("value").Data)), 0, 0));
            return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        auto Failed = Wait(Submit(R"PY(
import durin
with durin.ScopedEditorTransaction('Exception keeps edit'):
    durin.tools.call('python.tests', 'echo', {'value': 1})
    raise ValueError('preserved')
)PY"), false);
        EXPECT_EQ(EPythonJobState::Failed, Failed.State); EXPECT_EQ(1.0, Object->GetRelativeLocation().x);
        auto* Trans = Host->GetTransactor(); ASSERT_EQ(1u, Trans->GetQueueLength()); ASSERT_TRUE(Trans->Undo());
        EXPECT_EQ(0.0, Object->GetRelativeLocation().x); ASSERT_TRUE(Trans->Redo());
        auto Cancelled = Wait(Submit(R"PY(
import durin
with durin.ScopedEditorTransaction('Outer'):
    with durin.ScopedEditorTransaction('Cancelled') as inner:
        durin.tools.call('python.tests', 'echo', {'value': 2})
        assert inner.cancel() is None
assert inner.cancel() is None
)PY"), false);
        EXPECT_EQ(EPythonJobState::Succeeded, Cancelled.State) << Cancelled.Error;
        EXPECT_EQ(2.0, Object->GetRelativeLocation().x); EXPECT_FALSE(Trans->IsActive()); EXPECT_EQ(1u, Trans->GetQueueLength());
    }
    TEST_F(FPythonToolIntegrationTests, StopRequestedInsideNativeHandlerFinalizesAfterScriptReturn)
    {
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto&, const auto&) {
            Runtime.Stop();
            return FToolResult{EToolStatus::Succeeded, FToolValue::FObject{}};
        });
        const auto Job = Submit("import durin\nwith durin.ScopedEditorTransaction('Stop'):\n    durin.tools.call('python.tests', 'echo', {})");
        const auto Queued = Submit("import durin\ndurin.tools.call('python.tests', 'echo', {})"); (void)Queued;
        Tools->TickScripts();
        EXPECT_EQ(1, Calls);
        EXPECT_FALSE(Runtime.GetJobResult(Job)); EXPECT_FALSE(Host->GetTransactor()->IsActive());
        Runtime.Stop(); // Registration retirement belongs to this outer safe point.
        ASSERT_TRUE(Runtime.Start());
        auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("pass"), false).State);
    }

    TEST_F(FPythonToolIntegrationTests, DetachedJobControlsRemainAvailableWhileScriptBlocksGameThread)
    {
        auto Definition = CreatePythonToolset(std::shared_ptr<FPythonRuntime>(&Runtime, [](auto*) {}));
        auto Published = FModularFeatureRegistry::Get().RegisterFeature(*Definition);
        const auto Endpoint = Tools->GetService();
        FEditorToolLimits Limits; Limits.MaxQueued = 1; ASSERT_TRUE(Endpoint->Configure(Limits));
        auto Admission = Tools->CallNow({{"editor.python", "submit"}, 1,
            FToolValue::FObject{{"code", "while True:\n    pass\n"}}}, EToolSource::External, {"owner", {}});
        ASSERT_EQ(EToolStatus::Succeeded, Admission.Status);
        const auto Id = std::get<FToolValue::FObject>(Admission.Data.Data).at("job_id");
        auto Controls = std::async(std::launch::async, [&] {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            // Submit can queue just before the pump starts. Wait on its receipt
            // without asking for an editor tick; the fallback cancels the job.
            std::optional<FEditorToolInvocationId> Status;
            while (std::chrono::steady_clock::now() < Deadline)
            {
                auto Call = Endpoint->Submit({{"editor.python", "status"}, 1, FToolValue::FObject{{"job_id", Id}}},
                    EToolSource::External, {"owner", {}});
                if (Call)
                {
                    auto Receipt = Endpoint->GetInvocation(*Call);
                    if (Receipt && Receipt->Result && Receipt->Result->Status == EToolStatus::Succeeded
                        && std::get<std::string>(std::get<FToolValue::FObject>(Receipt->Result->Data.Data).at("state").Data) == "running")
                    { Status = *Call; break; }
                    (void)Endpoint->Cancel(*Call);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_TRUE(Status);
            if (Status)
            {
                auto Snapshot = Endpoint->GetInvocation(*Status); ASSERT_TRUE(Snapshot && Snapshot->Result);
                EXPECT_EQ(EToolStatus::Succeeded, Snapshot->Result->Status);
                auto Ordinary = Endpoint->Submit({{"editor.pie", "status"}, 1, FToolValue::FObject{}}, EToolSource::External);
                ASSERT_TRUE(Ordinary); // Fill ordinary queue capacity while GameThread is in Python.
                auto Foreign = Endpoint->Submit({{"editor.python", "status"}, 1, FToolValue::FObject{{"job_id", Id}}},
                    EToolSource::External, {"foreign", {}});
                ASSERT_TRUE(Foreign); auto Rejected = Endpoint->GetInvocation(*Foreign);
                ASSERT_TRUE(Rejected && Rejected->Result); EXPECT_EQ(EToolStatus::Failed, Rejected->Result->Status);
                auto Cancel = Endpoint->Submit({{"editor.python", "cancel"}, 1, FToolValue::FObject{{"job_id", Id}}},
                    EToolSource::External, {"owner", {}});
                ASSERT_TRUE(Cancel); auto Cancelled = Endpoint->GetInvocation(*Cancel);
                ASSERT_TRUE(Cancelled && Cancelled->Result); EXPECT_EQ(EToolStatus::Succeeded, Cancelled->Result->Status);
                (void)Endpoint->Cancel(*Ordinary);
            }
            else Runtime.CloseSession(Session); // The runtime deadline also bounds failed control delivery.
        });
        Tools->TickScripts(); Controls.get();
        auto Snapshot = Tools->CallNow({{"editor.python", "status"}, 1, FToolValue::FObject{{"job_id", Id}}},
            EToolSource::External, {"owner", {}});
        ASSERT_EQ(EToolStatus::Succeeded, Snapshot.Status);
        EXPECT_EQ("cancelled", std::get<std::string>(std::get<FToolValue::FObject>(Snapshot.Data.Data).at("state").Data));
    }
    TEST_F(FPythonToolIntegrationTests, HostShutdownCancelsQueuedWorkAndRetainsRunningScopesThroughReturn)
    {
        auto Queued = Submit("print('must not run')");
        Host->PrepareForShutdown();
        auto Snapshot = Runtime.GetJobResult(Queued); ASSERT_TRUE(Snapshot);
        EXPECT_EQ(EPythonJobState::Cancelled, Snapshot->State); EXPECT_TRUE(Snapshot->Output.empty());
    }
    TEST_F(FPythonToolIntegrationTests, NativeHostShutdownDuringScriptDoesNotDestroyItsActiveTransaction)
    {
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto&, const auto&) {
            Host->PrepareForShutdown();
            EXPECT_TRUE(Host->GetTransactor()->IsActive());
            return FToolResult{EToolStatus::Succeeded, FToolValue::FObject{}};
        });
        const auto Job = Submit("import durin\nwith durin.ScopedEditorTransaction('Shutdown'):\n    durin.tools.call('python.tests', 'echo', {})");
        Tools->TickScripts();
        auto Result = Runtime.GetJobResult(Job); ASSERT_TRUE(Result); EXPECT_EQ(EPythonJobState::Cancelled, Result->State);
        EXPECT_FALSE(Host->GetTransactor()->IsActive());
    }

    TEST_F(FPythonToolIntegrationTests, CancellationEndsRecordingWithoutRollingBackNativeEdits)
    {
        auto* Object = NewObject<DSceneComponent>(Host.Get(), "CancelTarget", EObjectFlags::Transactional);
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto& Request, const auto&) {
            EXPECT_TRUE(Object->Modify()); Object->SetRelativeLocation(FVector3(7, 0, 0));
            return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        const auto Job = Submit(R"PY(
import durin
with durin.ScopedEditorTransaction('Cancelled job keeps applied edit'):
    durin.tools.call('python.tests', 'echo', {})
    print('edited', flush=True)
    while True: pass
)PY");
        auto Canceller = WhileRunning(Job, [&] {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < Deadline)
            {
                auto Snapshot = Runtime.GetJobResult(Job);
                if (Snapshot && Snapshot->Output.find("edited") != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_TRUE(Runtime.CancelJob(Job));
        });
        const auto Result = Wait(Job, false); Canceller.get();
        EXPECT_EQ(EPythonJobState::Cancelled, Result.State); EXPECT_FALSE(Host->GetTransactor()->IsActive());
        EXPECT_EQ(7.0, Object->GetRelativeLocation().x); ASSERT_EQ(1u, Host->GetTransactor()->GetQueueLength());
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(0.0, Object->GetRelativeLocation().x);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(7.0, Object->GetRelativeLocation().x);
    }
    TEST_F(FPythonToolIntegrationTests, DeadlineClosesUnfinishedScopesBeforePublishingCancellation)
    {
        Runtime.Stop();
        ASSERT_TRUE(Runtime.Start({.JobDeadline = std::chrono::milliseconds(100)}));
        auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        const auto Job = Submit("import durin\ndurin.pending_scope = durin.ScopedEditorTransaction('Deadline')\ndurin.pending_scope.__enter__()\nwhile True: pass");
        Tools->TickScripts(); // Explicit optional elapsed deadline; no worker cancellation.
        auto Result = Runtime.GetJobResult(Job); ASSERT_TRUE(Result);
        EXPECT_EQ(EPythonJobState::Cancelled, Result->State); EXPECT_NE(std::string::npos, Result->Error.find("deadline"));
        EXPECT_FALSE(Host->GetTransactor()->IsActive());
        auto Reuse = Wait(Submit(R"PY(
import durin
try: durin.pending_scope.__exit__(None, None, None)
except ValueError: pass
else: raise AssertionError('previous job scope reused')
del durin.pending_scope
with durin.ScopedEditorTransaction('Next job'): pass
)PY"), false);
        EXPECT_EQ(EPythonJobState::Succeeded, Reuse.State) << Reuse.Error;
        EXPECT_FALSE(Host->GetTransactor()->IsActive());
    }

    TEST_F(FPythonToolIntegrationTests, CallbackStopReleasesInterpreterOwnershipAfterReturn)
    {
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto&, const auto&) {
            Runtime.Stop();
            return FToolResult{EToolStatus::Succeeded, FToolValue::FObject{}};
        });
        Submit("import durin\ndurin.tools.call('python.tests', 'echo', {})");
        Tools->TickScripts();
        FPythonRuntime Other;
        auto Started = Other.Start(); ASSERT_TRUE(Started) << Started.error();
        Runtime.Stop(); // Retiring the old registration must not release the new owner's reservation.
        FPythonRuntime Third; auto Duplicate = Third.Start(); ASSERT_FALSE(Duplicate);
        EXPECT_NE(std::string::npos, Duplicate.error().find("Another Durin"));
        auto SessionB = Other.CreateSession(Tools->GetService()); ASSERT_TRUE(SessionB);
        auto Job = Other.RunString(*SessionB, "print('new owner')"); ASSERT_TRUE(Job);
        Tools->TickScripts();
        auto Result = Other.GetJobResult(*Job); ASSERT_TRUE(Result);
        EXPECT_EQ(EPythonJobState::Succeeded, Result->State); EXPECT_EQ("new owner\n", Result->Output);
    }

    TEST_F(FPythonToolIntegrationTests, ContinuationsPreserveRegistrationOrderAndRebindJobContext)
    {
        const auto First = Submit(R"PY(
import durin
label = 'first'
@durin.defer
def one():
    print(label + ':one')
    durin.defer(lambda: print(label + ':dynamic'))
@durin.defer
def two():
    saved = durin.tools
    print(label + ':two')
    yield
    assert durin.tools is saved
    assert saved.call('editor.pie', 'status', {})['state'] == 'stopped'
    print(label + ':three')
)PY");
        const auto Second = Submit(R"PY(
import durin
label = 'second'
@durin.defer
def other():
    print(label + ':one')
    yield
    print(label + ':two')
)PY");
        Tools->TickScripts();
        EXPECT_EQ(EPythonJobState::Running, Runtime.GetJobResult(First)->State);
        const auto SecondState = Runtime.GetJobResult(Second)->State;
        EXPECT_TRUE(SecondState == EPythonJobState::Queued || SecondState == EPythonJobState::Running);
        auto A = Wait(First, false), B = Wait(Second, false);
        EXPECT_EQ(EPythonJobState::Succeeded, A.State) << A.Error;
        EXPECT_EQ(EPythonJobState::Succeeded, B.State) << B.Error;
        EXPECT_EQ("first:one\nfirst:two\nfirst:three\nfirst:dynamic\n", A.Output);
        EXPECT_EQ("second:one\nsecond:two\n", B.Output);
    }

    TEST_F(FPythonToolIntegrationTests, PendingPreparationReturnsToTicksBeforeResultDependentShortApply)
    {
        TStrongObjectPtr<DSceneComponent> Object(NewObject<DSceneComponent>(nullptr, "PreparedObject", EObjectFlags::Transactional));
        TWeakObjectPtr<DSceneComponent> Target(Object.Get());
        uint64 Revision = 1;
        FToolSchema Schema(EToolValueType::Object);
        Schema.Properties.emplace("value", FToolSchema(EToolValueType::Integer));
        Schema.Properties.emplace("revision", FToolSchema(EToolValueType::UnsignedInteger));
        Schema.Properties.emplace("target", FToolSchema(EToolValueType::String));
        Schema.Required = {"value", "revision", "target"};
        Register(Schema, true, [&](const auto& Request, const auto& Context) {
            const auto& Arguments = std::get<FToolValue::FObject>(Request.Arguments.Data);
            auto* Resolved = Target.Get();
            if (std::get<std::string>(Arguments.at("target").Data) != "fixture-object" || !Resolved)
                return FToolResult{EToolStatus::Failed, {}, {{EToolError::TargetNotFound, "Exact fixture target retired", {}}}};
            if (std::get<uint64>(Arguments.at("revision").Data) != Revision)
                return FToolResult{EToolStatus::Failed, {}, {{EToolError::RevisionConflict, "Prepared revision is stale", {}}}};
            Editor::FScopedTransaction Scope(GetToolEditor(Context)->GetTransactor(), {.Description = "Apply prepared value"});
            EXPECT_TRUE(Resolved->Modify()); Resolved->SetRelativeLocation({static_cast<double>(std::get<int64>(std::get<FToolValue::FObject>(Request.Arguments.Data).at("value").Data)), 0, 0});
            ++Revision; return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        auto Source = FToolCompletionSource::CreateDetached(); RegisterPending(Source.TakeTask(), Schema);
        auto Job = Submit(R"PY(
import durin
try: durin.tools.call('python.pending', 'read', {})
except durin.ToolError: pass
else: raise AssertionError('may-pend call executed')
@durin.defer
def prepare():
    handle = durin.tools.submit('python.pending', 'read', {})
    try: handle.result()
    except durin.NotReady: pass
    else: raise AssertionError('pending result was exposed')
    value = yield handle
    with durin.ScopedEditorTransaction('Prepared apply'):
        durin.tools.call('python.tests', 'echo', value)
)PY");
        Tools->TickScripts(); EXPECT_EQ(0, PendingCalls);
        Tools->TickScripts(); EXPECT_EQ(0, PendingCalls); // A callback returning a generator is one step.
        Tools->TickScripts(); EXPECT_EQ(1, PendingCalls); EXPECT_EQ(0, Calls);
        EXPECT_FALSE(Host->GetTransactor()->IsActive());
        EXPECT_EQ(0u, Host->GetTransactor()->GetQueueLength());
        EXPECT_TRUE(Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{{"value", 7}, {"revision", uint64{1}}, {"target", "fixture-object"}}}));
        EXPECT_EQ(0, Calls); // Completion never reenters Python inline.
        Tools->TickScripts(); EXPECT_EQ(0, Calls); // Raw provider readiness is not validated publication.
        Tools->TickTools(); Tools->TickScripts();
        auto Result = Runtime.GetJobResult(Job); ASSERT_TRUE(Result);
        EXPECT_EQ(EPythonJobState::Succeeded, Result->State) << Result->Error;
        EXPECT_EQ(7.0, Object->GetRelativeLocation().x); EXPECT_EQ(1u, Host->GetTransactor()->GetQueueLength());
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(0.0, Object->GetRelativeLocation().x);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(7.0, Object->GetRelativeLocation().x);
    }

    TEST_F(FPythonToolIntegrationTests, CrossYieldRecordingIncludesNativeUiAndOtherJobEdits)
    {
        TStrongObjectPtr<DSceneComponent> Object(NewObject<DSceneComponent>(nullptr, "SharedRecording", EObjectFlags::Transactional));
        TStrongObjectPtr<DSceneComponent> UiObject(NewObject<DSceneComponent>(nullptr, "SharedUiRecording", EObjectFlags::Transactional));
        FToolSchema Schema(EToolValueType::Object); Schema.Properties.emplace("value", FToolSchema(EToolValueType::Integer)); Schema.Required = {"value"};
        Register(Schema, true, [&](const auto& Request, const auto& Context) {
            Editor::FScopedTransaction Scope(GetToolEditor(Context)->GetTransactor(), {.Description = "Native edit"});
            EXPECT_TRUE(Object->Modify()); Object->SetRelativeLocation({static_cast<double>(std::get<int64>(std::get<FToolValue::FObject>(Request.Arguments.Data).at("value").Data)), 0, 0});
            return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        auto Source = FToolCompletionSource::CreateDetached(); RegisterPending(Source.TakeTask());
        auto Job = Submit(R"PY(
import durin
@durin.defer
def edit():
    with durin.ScopedEditorTransaction('Across ticks'):
        durin.tools.call('python.tests', 'echo', {'value': 1})
        yield durin.tools.submit('python.pending', 'read', {})
        durin.tools.call('python.tests', 'echo', {'value': 3})
)PY");
        Tools->TickScripts(); Tools->TickScripts(); Tools->TickScripts();
        EXPECT_TRUE(Host->GetTransactor()->IsActive()); EXPECT_FALSE(Host->GetTransactor()->CanUndo());
        Host->DispatchSubsystemCallback([&] { EXPECT_TRUE(UiObject->Modify()); UiObject->SetRelativeLocation({9, 0, 0}); });
        auto Other = Submit("import durin\ndurin.tools.call('python.tests', 'echo', {'value': 2})");
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Other).State);
        EXPECT_EQ(2.0, Object->GetRelativeLocation().x); EXPECT_TRUE(Host->GetTransactor()->IsActive());
        Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{}});
        auto Result = Wait(Job); EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        EXPECT_EQ(1u, Host->GetTransactor()->GetQueueLength());
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(0.0, Object->GetRelativeLocation().x); EXPECT_EQ(0.0, UiObject->GetRelativeLocation().x);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(3.0, Object->GetRelativeLocation().x); EXPECT_EQ(9.0, UiObject->GetRelativeLocation().x);
    }

    TEST_F(FPythonToolIntegrationTests, SuspendedCancellationClosesFinallyAndDetachesLateResultsWithoutRollback)
    {
        auto Source = FToolCompletionSource::CreateDetached(); RegisterPending(Source.TakeTask());
        auto Job = Submit(R"PY(
import durin
@durin.defer
def work():
    try:
        with durin.ScopedEditorTransaction('Suspended cancellation'):
            yield durin.tools.submit('python.pending', 'read', {})
    finally:
        print('finally')
)PY");
        Tools->TickScripts(); Tools->TickScripts(); Tools->TickScripts(); ASSERT_TRUE(Host->GetTransactor()->IsActive());
        ASSERT_TRUE(Runtime.CancelJob(Job)); EXPECT_TRUE(PendingCancellation.IsCancellationRequested());
        EXPECT_EQ(EPythonJobState::Running, Runtime.GetJobResult(Job)->State);
        Tools->TickScripts();
        auto Result = Runtime.GetJobResult(Job); ASSERT_TRUE(Result);
        EXPECT_EQ(EPythonJobState::Cancelled, Result->State); EXPECT_EQ("finally\n", Result->Output) << Result->Error;
        EXPECT_FALSE(Host->GetTransactor()->IsActive());
        Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{}}); Tools->TickTools(); Tools->TickScripts();
        EXPECT_EQ(EPythonJobState::Cancelled, Runtime.GetJobResult(Job)->State);
        EXPECT_EQ("finally\n", Runtime.GetJobResult(Job)->Output);
    }

    TEST_F(FPythonToolIntegrationTests, SuspendedHostCloseRunsFinallyBeforeTerminalPublication)
    {
        auto Job = Submit(R"PY(
import durin
@durin.defer
def work():
    try:
        with durin.ScopedEditorTransaction('Host close'):
            yield
    finally:
        print('closed')
)PY");
        Tools->TickScripts(); Tools->TickScripts(); Tools->TickScripts(); ASSERT_TRUE(Host->GetTransactor()->IsActive());
        Host->PrepareForShutdown();
        auto Result = Runtime.GetJobResult(Job); ASSERT_TRUE(Result);
        EXPECT_EQ(EPythonJobState::Cancelled, Result->State); EXPECT_EQ("closed\n", Result->Output) << Result->Error;
        EXPECT_FALSE(Host->GetTransactor()->IsActive());
    }

    TEST_F(FPythonToolIntegrationTests, SuspendedStopClosesGeneratorsBeforeInterpreterRestart)
    {
        auto Job = Submit(R"PY(
import durin
@durin.defer
def work():
    with durin.ScopedEditorTransaction('Stop suspended'):
        yield
)PY");
        Tools->TickScripts(); Tools->TickScripts(); Tools->TickScripts(); EXPECT_TRUE(Host->GetTransactor()->IsActive());
        Runtime.Stop(); EXPECT_FALSE(Host->GetTransactor()->IsActive());
        ASSERT_TRUE(Runtime.Start()); auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Submit("print('restarted')")).State);
    }

    TEST_F(FPythonToolIntegrationTests, PendingFailureIsThrownIntoGeneratorAndForeignYieldFails)
    {
        auto Source = FToolCompletionSource::CreateDetached(); RegisterPending(Source.TakeTask());
        auto Job = Submit(R"PY(
import durin
@durin.defer
def work():
    try: yield durin.tools.submit('python.pending', 'read', {})
    except durin.ToolError: print('caught')
)PY");
        Tools->TickScripts(); Tools->TickScripts(); Tools->TickScripts();
        Source.TrySetValue({EToolStatus::Failed, {}, {{EToolError::DomainFailure, "Expected failure", {}}}});
        auto Result = Wait(Job); EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error; EXPECT_EQ("caught\n", Result.Output);
        auto Invalid = Wait(Submit(R"PY(
import durin
@durin.defer
def work():
    with durin.ScopedEditorTransaction('Invalid yield'):
        yield 123
)PY"));
        EXPECT_EQ(EPythonJobState::Failed, Invalid.State); EXPECT_NE(std::string::npos, Invalid.Error.find("may yield")); EXPECT_FALSE(Host->GetTransactor()->IsActive());
    }

    TEST_F(FPythonToolIntegrationTests, ClientSessionsShareSuspendedEditorRecording)
    {
        TStrongObjectPtr<DSceneComponent> Target(NewObject<DSceneComponent>(nullptr, "SharedRecordingTarget", EObjectFlags::Transactional));
        FToolSchema Schema(EToolValueType::Object); Schema.Properties.emplace("value", FToolSchema(EToolValueType::Integer)); Schema.Required = {"value"};
        Register(Schema, true, [&](const auto& Request, const auto& Context) {
            EXPECT_EQ(GetToolEditor(Context), Host.Get());
            Editor::FScopedTransaction Scope("Shared editor edit");
            EXPECT_TRUE(Target->Modify());
            Target->SetRelativeLocation({static_cast<double>(std::get<int64>(std::get<FToolValue::FObject>(Request.Arguments.Data).at("value").Data)), 0, 0});
            return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        auto Source = FToolCompletionSource::CreateDetached(); RegisterPending(Source.TakeTask());
        auto OtherSession = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(OtherSession);
        const std::string Code = R"PY(
import durin
@durin.defer
def edit():
    with durin.ScopedEditorTransaction('Shared suspended recording'):
        durin.tools.call('python.tests', 'echo', {'value': 1})
        yield durin.tools.submit('python.pending', 'read', {})
        durin.tools.call('python.tests', 'echo', {'value': 3})
)PY";
        auto A = Submit(Code); auto B = Runtime.RunString(*OtherSession, Code); ASSERT_TRUE(B);
        // The per-pump time budget can defer a session under load. Observe both
        // pending native requests before checking their shared recording.
        for (int I = 0; I < 100 && PendingCalls < 2; ++I) Tools->TickScripts();
        ASSERT_EQ(2, PendingCalls);
        EXPECT_TRUE(Host->GetTransactor()->IsActive());
        EXPECT_EQ(1.0, Target->GetRelativeLocation().x);
        Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{}});
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(A).State);
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(*B).State);
        EXPECT_FALSE(Host->GetTransactor()->IsActive());
        EXPECT_EQ(Host->GetTransactor()->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(0.0, Target->GetRelativeLocation().x);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(3.0, Target->GetRelativeLocation().x);
        Runtime.CloseSession(*OtherSession);
    }
    TEST_F(FPythonToolIntegrationTests, UnconsumedPendingInvocationKeepsJobLiveUntilSettlement)
    {
        auto Source = FToolCompletionSource::CreateDetached(); RegisterPending(Source.TakeTask());
        auto Job = Submit("import durin\ndurin.tools.submit('python.pending', 'read', {})\nprint('submitted')");
        Tools->TickScripts(); EXPECT_EQ(EPythonJobState::Running, Runtime.GetJobResult(Job)->State);
        EXPECT_EQ("submitted\n", Runtime.GetJobResult(Job)->Output);
        Source.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{}});
        EXPECT_EQ(EPythonJobState::Running, Runtime.GetJobResult(Job)->State);
        Tools->TickTools(); Tools->TickScripts();
        EXPECT_EQ(EPythonJobState::Succeeded, Runtime.GetJobResult(Job)->State);
    }

    TEST_F(FPythonToolIntegrationTests, ElapsedDeadlineIncludesSuspensionAndCleanupFailuresAreReported)
    {
        Runtime.Stop(); ASSERT_TRUE(Runtime.Start({.JobDeadline = std::chrono::milliseconds(100)}));
        auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        auto Job = Submit(R"PY(
import durin
@durin.defer
def wait():
    try:
        with durin.ScopedEditorTransaction('Elapsed deadline'):
            while True: yield
    finally:
        raise RuntimeError('expected cleanup diagnostic')
)PY");
        Tools->TickScripts(); Tools->TickScripts(); Tools->TickScripts();
        ASSERT_TRUE(Host->GetTransactor()->IsActive());
        std::this_thread::sleep_for(std::chrono::milliseconds(120)); // Measures total suspension time, not step execution.
        Tools->TickScripts();
        auto Result = Runtime.GetJobResult(Job); ASSERT_TRUE(Result);
        EXPECT_EQ(EPythonJobState::Cancelled, Result->State); EXPECT_FALSE(Host->GetTransactor()->IsActive());
        EXPECT_NE(std::string::npos, Result->Error.find("deadline"));
        EXPECT_NE(std::string::npos, Result->Error.find("expected cleanup diagnostic"));
    }

    TEST_F(FPythonToolIntegrationTests, CrossYieldInnerCancelLeavesAppliedDataAndStaleOuterScopeIsInert)
    {
        TStrongObjectPtr<DSceneComponent> Object(NewObject<DSceneComponent>(nullptr, "CrossYieldCancel", EObjectFlags::Transactional));
        FToolSchema Schema(EToolValueType::Object); Schema.Properties.emplace("value", FToolSchema(EToolValueType::Integer)); Schema.Required = {"value"};
        Register(Schema, true, [&](const auto& Request, const auto& Context) {
            Editor::FScopedTransaction Scope(GetToolEditor(Context)->GetTransactor(), {.Description = "Native edit"});
            EXPECT_TRUE(Object->Modify()); Object->SetRelativeLocation({static_cast<double>(std::get<int64>(std::get<FToolValue::FObject>(Request.Arguments.Data).at("value").Data)), 0, 0});
            return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        auto Result = Wait(Submit(R"PY(
import durin
@durin.defer
def edit():
    with durin.ScopedEditorTransaction('Discarded outer'):
        durin.tools.call('python.tests', 'echo', {'value': 6})
        yield
        with durin.ScopedEditorTransaction('Inner') as inner:
            inner.cancel()
        yield
        with durin.ScopedEditorTransaction('Replacement'):
            durin.tools.call('python.tests', 'echo', {'value': 9})
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        EXPECT_EQ(9.0, Object->GetRelativeLocation().x); EXPECT_EQ(1u, Host->GetTransactor()->GetQueueLength());
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(6.0, Object->GetRelativeLocation().x);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(9.0, Object->GetRelativeLocation().x);
    }

    TEST_F(FPythonToolIntegrationTests, CleanupRejectsNewNativeWorkFromCyclicFinalizers)
    {
        Register(FToolSchema(EToolValueType::Object));
        auto Result = Wait(Submit(R"PY(
import durin
class CleanupProbe:
    def __del__(self):
        try: durin.tools.call('python.tests', 'echo', {})
        except durin.CancelledError: print('blocked during cleanup')
probe = CleanupProbe()
probe.cycle = probe
)PY"));
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error;
        EXPECT_EQ("blocked during cleanup\n", Result.Output); EXPECT_EQ(0, Calls);
    }

    TEST_F(FPythonToolIntegrationTests, ReadyHandleRetainsNativeResultAcrossReceiptExpiryAndYield)
    {
        FEditorToolLimits Limits; Limits.ResultTTL = std::chrono::milliseconds(0);
        ASSERT_TRUE(Tools->GetService()->Configure(Limits));
        auto Result = Wait(Submit(R"PY(
import durin
@durin.defer
def read():
    handle = durin.tools.submit('editor.pie', 'status', {})
    yield
    print(handle.result()['state'])
)PY"), false);
        EXPECT_EQ(EPythonJobState::Succeeded, Result.State) << Result.Error; EXPECT_EQ("stopped\n", Result.Output);
    }

    TEST_F(FPythonToolIntegrationTests, ScopeFinalCaptureFailureCannotPublishSuccessfulJob)
    {
        TStrongObjectPtr<DSceneComponent> Object(NewObject<DSceneComponent>(nullptr, "FinalCaptureFailure", EObjectFlags::Transactional));
        Register(FToolSchema(EToolValueType::Object), true, [&](const auto& Request, const auto&) {
            EXPECT_TRUE(Object->Modify()); Object->SetRelativeLocation({1, 0, 0});
            return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        auto Job = Submit(R"PY(
import durin
@durin.defer
def edit():
    with durin.ScopedEditorTransaction('Fail final capture'):
        durin.tools.call('python.tests', 'echo', {})
        yield
)PY");
        Tools->TickScripts(); Tools->TickScripts(); Tools->TickScripts();
        ASSERT_TRUE(Host->GetTransactor()->IsActive());
        MarkObjectHierarchyAsGarbage(Object.Get()); Object.Reset(); CollectGarbage();
        auto Result = Wait(Job);
        EXPECT_EQ(EPythonJobState::Failed, Result.State); EXPECT_FALSE(Result.Error.empty());
        EXPECT_FALSE(Host->GetTransactor()->IsActive()); EXPECT_EQ(0u, Host->GetTransactor()->GetQueueLength());
    }

    TEST_F(FPythonToolIntegrationTests, IntegratedFoundationRevalidatesAndPreservesCancelledGlobalRecording)
    {
        TStrongObjectPtr<DSceneComponent> Object(NewObject<DSceneComponent>(nullptr, "IntegratedTarget", EObjectFlags::Transactional));
        TStrongObjectPtr<DSceneComponent> UiObject(NewObject<DSceneComponent>(nullptr, "IntegratedUi", EObjectFlags::Transactional));
        TWeakObjectPtr<DSceneComponent> Target(Object.Get());
        uint64 Revision = 1; int Effects = 0;
        FToolSchema Schema(EToolValueType::Object);
        Schema.Properties.emplace("value", FToolSchema(EToolValueType::Integer));
        Schema.Properties.emplace("revision", FToolSchema(EToolValueType::UnsignedInteger));
        Schema.Properties.emplace("target", FToolSchema(EToolValueType::String));
        Schema.Required = {"value", "revision", "target"};
        Register(Schema, true, [&](const auto& Request, const auto& Context) {
            const auto& Args = std::get<FToolValue::FObject>(Request.Arguments.Data);
            auto* Resolved = Target.Get();
            if (!Resolved || std::get<std::string>(Args.at("target").Data) != "fixture")
                return FToolResult{EToolStatus::Failed, {}, {{EToolError::TargetNotFound, "Target retired", {}}}};
            if (std::get<uint64>(Args.at("revision").Data) != Revision)
                return FToolResult{EToolStatus::Failed, {}, {{EToolError::RevisionConflict, "Revision changed during preparation", {}}}};
            Editor::FScopedTransaction Apply(GetToolEditor(Context)->GetTransactor(), {.Description = "Short native apply"});
            EXPECT_TRUE(Resolved->Modify()); Resolved->SetRelativeLocation({static_cast<double>(std::get<int64>(Args.at("value").Data)), 0, 0});
            ++Revision; ++Effects; return FToolResult{EToolStatus::Succeeded, Request.Arguments};
        });
        auto Preparation = FToolCompletionSource::CreateDetached(); RegisterPending(Preparation.TakeTask(), Schema);
        auto Stale = Submit(R"PY(
import durin
@durin.defer
def prepare():
    prepared = yield durin.tools.submit('python.pending', 'read', {})
    try:
        with durin.ScopedEditorTransaction('Revalidate short apply'):
            durin.tools.call('python.tests', 'echo', prepared)
    except durin.ToolError:
        print('revision conflict')
)PY");
        for (int I = 0; I < 3; ++I) Host->Tick(0.0f, false);
        ASSERT_EQ(1, PendingCalls); EXPECT_FALSE(Host->GetTransactor()->IsActive());
        Host->DispatchSubsystemCallback([&] {
            Editor::FScopedTransaction Ui(Host->GetTransactor(), {.Description = "UI changes revision"});
            EXPECT_TRUE(Object->Modify()); Object->SetRelativeLocation({2, 0, 0}); ++Revision;
        });
        Preparation.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{{"value", 4}, {"revision", uint64{1}}, {"target", "fixture"}}});
        Host->Tick(0.0f, false);
        auto Rejected = Runtime.GetJobResult(Stale); ASSERT_TRUE(Rejected);
        EXPECT_EQ(EPythonJobState::Succeeded, Rejected->State) << Rejected->Error;
        EXPECT_EQ("revision conflict\n", Rejected->Output); EXPECT_EQ(0, Effects);
        EXPECT_EQ(2.0, Object->GetRelativeLocation().x); EXPECT_EQ(1u, Host->GetTransactor()->GetQueueLength());

        PendingRegistration.Reset(); PendingToolset.reset();
        auto Pending = FToolCompletionSource::CreateDetached(); RegisterPending(Pending.TakeTask(), Schema);
        auto Cancelled = Submit(R"PY(
import durin
@durin.defer
def edit():
    try:
        with durin.ScopedEditorTransaction('Cancelled global recording'):
            durin.tools.call('python.tests', 'echo', {'value': 6, 'revision': 2, 'target': 'fixture'})
            yield durin.tools.submit('python.pending', 'read', {})
    finally:
        print('closed')
)PY");
        for (int I = 0; I < 3; ++I) Host->Tick(0.0f, false);
        ASSERT_TRUE(Host->GetTransactor()->IsActive()); EXPECT_FALSE(Host->GetTransactor()->CanUndo());
        auto Other = Submit("import durin\ndurin.tools.call('python.tests', 'echo', {'value': 7, 'revision': 3, 'target': 'fixture'})");
        EXPECT_EQ(EPythonJobState::Succeeded, Wait(Other).State);
        Host->DispatchSubsystemCallback([&] { EXPECT_TRUE(UiObject->Modify()); UiObject->SetRelativeLocation({9, 0, 0}); });

        TStrongObjectPtr<DSceneComponent> OtherDocument(NewObject<DSceneComponent>(nullptr, "IntegratedOtherDocument", EObjectFlags::Transactional));
        Host->DispatchSubsystemCallback([&] {
            Editor::FScopedTransaction Scope("Other document");
            EXPECT_TRUE(OtherDocument->Modify()); OtherDocument->SetRelativeLocation({4, 0, 0});
        });
        EXPECT_EQ(4.0, OtherDocument->GetRelativeLocation().x);
        EXPECT_TRUE(Host->GetTransactor()->IsActive()); EXPECT_EQ(7.0, Object->GetRelativeLocation().x);
        ASSERT_TRUE(Runtime.CancelJob(Cancelled)); EXPECT_TRUE(PendingCancellation.IsCancellationRequested());
        Host->Tick(0.0f, false);
        auto Result = Runtime.GetJobResult(Cancelled); ASSERT_TRUE(Result);
        EXPECT_EQ(EPythonJobState::Cancelled, Result->State); EXPECT_EQ("closed\n", Result->Output) << Result->Error;
        EXPECT_FALSE(Host->GetTransactor()->IsActive()); EXPECT_EQ(2u, Host->GetTransactor()->GetQueueLength());
        EXPECT_EQ(2, Effects); EXPECT_EQ(7.0, Object->GetRelativeLocation().x); EXPECT_EQ(9.0, UiObject->GetRelativeLocation().x);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_EQ(2.0, Object->GetRelativeLocation().x); EXPECT_EQ(0.0, UiObject->GetRelativeLocation().x);
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(7.0, Object->GetRelativeLocation().x); EXPECT_EQ(9.0, UiObject->GetRelativeLocation().x);
        Pending.TrySetValue({EToolStatus::Succeeded, FToolValue::FObject{{"value", 999}, {"revision", uint64{2}}, {"target", "fixture"}}});
        Host->Tick(0.0f, false);
        EXPECT_EQ(EPythonJobState::Cancelled, Runtime.GetJobResult(Cancelled)->State);
        EXPECT_EQ(2, Effects); EXPECT_EQ(7.0, Object->GetRelativeLocation().x);
    }

}
