#include "PythonRuntime.h"
#include "PythonReflectionTestTypes.h"
#include "Editor/EditorEngine.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/EditorToolService.h"
#include "Tools/EditorScriptCall.h"
#include "Modules/ModuleManager.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Class.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
    class FPythonReflectionEditor : public DEditorEngine
    {
        static auto GetPrivateStaticClass() -> DClass*;
        DECLARE_CLASS(FPythonReflectionEditor, DEditorEngine, FPythonReflectionEditor::GetPrivateStaticClass)
        DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FPythonReflectionEditor)
    public:
        explicit FPythonReflectionEditor(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
        auto Start() -> FSubsystemResult { return ActivateEditor() ? InitializeEditorSubsystems() : FSubsystemResult{ESubsystemError::InvalidState, "Editor activation failed."}; }
    };
    IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FPythonReflectionEditor)
    struct FEditorEngineTestAccess
    {
        static auto SetEditorWorld(DEditorEngine& Editor, DWorld* World) -> void { Editor.EditorWorld = World; Editor.SetWorld(World); }
    };
}
namespace
{
    using namespace Durin;
    using namespace Durin::PythonFixtures;
    class PythonReflectionTests : public testing::Test
    {
    protected:
        FPythonRuntime Runtime;
        TStrongObjectPtr<FPythonReflectionEditor> Host;
        DEditorToolSubsystem* Tools = nullptr;
        std::unique_ptr<FEditorSubsystemRegistration> First, Second;
        uint64 Session = 0;
        auto SetUp() -> void override
        {
            Testing::InitializeDObjectSystemForTests();
            ASSERT_TRUE(FModuleManager::Get().LoadModule("Engine"));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("DurinEd"));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("DurinPython"));
            First = Register(DReflectionService::StaticClass()); Second = Register(DSecondaryService::StaticClass());
            Host = TStrongObjectPtr<FPythonReflectionEditor>(NewObject<FPythonReflectionEditor>(nullptr, {}));
            ASSERT_TRUE(Host->Start()); Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>(); ASSERT_NE(Tools, nullptr);
            const auto Started = Runtime.Start(); ASSERT_TRUE(Started) << Started.error();
            const auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        }
        auto TearDown() -> void override
        {
            Runtime.Stop();
            if (Host) { Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); }
            First.reset(); Second.reset(); CollectGarbage();
        }
        static auto Register(DClass* Type) -> std::unique_ptr<FEditorSubsystemRegistration>
        { return std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = Type, .Provider = "DurinPython"}); }
        auto Run(std::string Source, uint64 InSession = 0, DEditorToolSubsystem* Pump = nullptr) -> FPythonJobResult
        {
            const auto Submitted = Runtime.RunString(InSession ? InSession : Session, "import durin, builtins\n" + Source);
            EXPECT_TRUE(Submitted); if (!Submitted) return {.State = EPythonJobState::Failed, .Error = Submitted.error()};
            for (int Index = 0; Index < 100; ++Index)
            {
                (Pump ? Pump : Tools)->TickScripts();
                const auto Result = Runtime.GetJobResult(*Submitted);
                if (Result && Result->State != EPythonJobState::Queued && Result->State != EPythonJobState::Running) return *Result;
            }
            ADD_FAILURE() << "Script did not settle"; return {.State = EPythonJobState::Failed};
        }
        auto Pass(std::string Source) -> void
        { const auto Result = Run(std::move(Source)); EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error; }
        auto Save() -> void
        { Pass("builtins.proxy = durin.get_editor_subsystem(durin.ReflectionService)\nbuiltins.cached = builtins.proxy.get_name\nbuiltins.identity = hash(builtins.proxy)"); }
    };
    TEST_F(PythonReflectionTests, RuntimeTypesPreserveInheritanceAndFactoryIdentity)
    {
        Pass(R"(
a = durin.get_editor_subsystem(durin.ReflectionService)
b = durin.get_editor_subsystem(durin.SecondaryService)
assert a is durin.get_editor_subsystem(durin.ReflectionService)
assert type(a) is durin.ReflectionService and a.get_class() is type(a)
assert isinstance(a, durin.ReflectionBase) and isinstance(a, durin.EditorSubsystem)
assert a != b and a == a and isinstance(a.get_name(), str)
assert durin.get_type('Durin::PythonFixtures::DReflectionService') is type(a)
assert isinstance(durin.get_type('Durin::PythonFixtures::FSignatureDependency'), type)
assert isinstance(durin.ReflectionMode, type)
assert not hasattr(durin, 'SharedAlias')
assert durin.get_type('Durin::PythonFixtures::DFirstAlias') is not durin.get_type('Durin::PythonFixtures::DSecondAlias')
)");
    }
    TEST_F(PythonReflectionTests, NativeConstructionSubclassingAndHiddenTypesAreRejected)
    {
        Pass(R"(
for operation in (lambda: durin.ReflectionService(), lambda: type('Derived', (durin.ReflectionService,), {}),
                  lambda: type('DerivedValue', (durin.SignatureDependency,), {}),
                  lambda: type('DerivedEnum', (durin.ReflectionMode,), {}),
                  lambda: durin.get_editor_subsystem(int), lambda: setattr(durin.ReflectionService, 'extra', 1)):
    try:
        operation()
    except TypeError:
        pass
    else:
        raise AssertionError('native construction/type mutation admitted')
try:
    durin.get_type('Durin::PythonFixtures::DHiddenType')
except AttributeError:
    pass
else:
    raise AssertionError('excluded type exposed')
)");
    }
    TEST_F(PythonReflectionTests, SameHostJobsAndSessionsReuseRetainedObjects)
    {
        Save(); CollectGarbage();
        const auto Other = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Other);
        auto Result = Run("assert builtins.proxy is durin.get_editor_subsystem(durin.ReflectionService)\nassert builtins.cached() == builtins.proxy.get_name()", *Other);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        Runtime.CloseSession(Session);
        Result = Run("assert builtins.cached() == builtins.proxy.get_name()", *Other);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
    }
    TEST_F(PythonReflectionTests, RetiredEditorCachedMethodsRejectAfterSequentialReplacement)
    {
        Save(); Host->PrepareForShutdown();
        TStrongObjectPtr<FPythonReflectionEditor> Other(NewObject<FPythonReflectionEditor>(nullptr, {})); ASSERT_TRUE(Other->Start());
        auto* OtherTools = Other->GetEditorSubsystem<DEditorToolSubsystem>(); ASSERT_NE(OtherTools, nullptr);
        auto Created = Runtime.CreateSession(OtherTools->GetService()); ASSERT_TRUE(Created);
        const auto Result = Run(R"(
assert builtins.proxy != durin.get_editor_subsystem(durin.ReflectionService)
try:
    builtins.cached()
except ReferenceError:
    pass
else:
    raise AssertionError('retired editor cached method admitted')
)", *Created, OtherTools);
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        Runtime.CloseSession(*Created); Other->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Other.Get()); Other.Reset();
        EXPECT_FALSE(Runtime.RunString(Session, "pass"));
    }
    TEST_F(PythonReflectionTests, RetiredServicePurgesRootsAndCannotReviveCachedBoundMethods)
    {
        Save(); TWeakObjectPtr<DReflectionService> Old(Host->GetEditorSubsystem<DReflectionService>());
        First.reset(); ASSERT_TRUE(Host->RefreshEditorSubsystems()); CollectGarbage(); EXPECT_FALSE(Old.IsValid());
        First = Register(DReflectionService::StaticClass()); ASSERT_TRUE(Host->RefreshEditorSubsystems());
        Pass(R"(
assert hash(builtins.proxy) == builtins.identity
assert builtins.proxy != durin.get_editor_subsystem(durin.ReflectionService)
try:
    builtins.cached()
except ReferenceError:
    pass
else:
    raise AssertionError('retired proxy revived')
)");
    }
    TEST_F(PythonReflectionTests, ExplicitDestructionInvalidatesProxyBeforeCollection)
    {
        Save(); auto* Object = Host->GetEditorSubsystem<DReflectionService>(); ASSERT_NE(Object, nullptr);
        MarkAsGarbage(Object);
        Pass(R"(
try:
    builtins.cached()
except ReferenceError:
    pass
else:
    raise AssertionError('garbage object accessed')
)");
    }
    TEST_F(PythonReflectionTests, ReconciliationCannotReenterNativeScriptAdmission)
    {
        auto* Retiring = Host->GetEditorSubsystem<DReflectionService>();
        bool Observed = false;
        FObjectReferencePurgeRegistration Observer([&](DObject& Object) {
            if (&Object != Retiring) return;
            Observed = true;
            FScopedEditorScriptCall Call({Host.Get(), Tools->GetService()->GetWorkGate()});
            EXPECT_FALSE(Call.IsAdmitted());
        });
        First.reset(); ASSERT_TRUE(Host->RefreshEditorSubsystems()); EXPECT_TRUE(Observed);
    }
    TEST_F(PythonReflectionTests, HostCloseReleasesRetainedWrapperRoots)
    {
        Save(); TWeakObjectPtr<DReflectionService> Service(Host->GetEditorSubsystem<DReflectionService>());
        Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); CollectGarbage();
        EXPECT_FALSE(Service.IsValid()); Runtime.Stop();
    }
    TEST_F(PythonReflectionTests, InterpreterRestartRebuildsNativeTypesAndFactories)
    {
        Save(); Runtime.Stop(); ASSERT_TRUE(Runtime.Start());
        const auto Created = Runtime.CreateSession(Tools->GetService()); ASSERT_TRUE(Created); Session = *Created;
        Pass("assert not hasattr(builtins, 'proxy')\na = durin.get_editor_subsystem(durin.ReflectionService)\nassert a.get_class() is durin.ReflectionService");
    }
    TEST_F(PythonReflectionTests, CancelledJobDoesNotInvalidateSameHostObjects)
    {
        Save(); const auto Pending = Runtime.RunString(Session, "import durin\ndurin.defer(lambda: None)"); ASSERT_TRUE(Pending);
        Runtime.CancelJob(*Pending); Tools->TickScripts();
        EXPECT_EQ(Runtime.GetJobResult(*Pending)->State, EPythonJobState::Cancelled);
        Pass("assert builtins.cached() == builtins.proxy.get_name()");
    }
    TEST_F(PythonReflectionTests, CleanupCannotUseNativeObjectsWithoutActiveJobAdmission)
    {
        Save();
        const auto Result = Run(R"(
class Cleanup:
    def __del__(self):
        try:
            builtins.cached()
        except durin.ScriptAccessError:
            print('cleanup blocked')
        else:
            print('cleanup bypass')
cleanup = Cleanup()
)");
        EXPECT_EQ(Result.State, EPythonJobState::Succeeded) << Result.Error;
        EXPECT_NE(Result.Output.find("cleanup blocked"), std::string::npos) << Result.Output;
        EXPECT_EQ(Result.Output.find("cleanup bypass"), std::string::npos);
    }
    TEST_F(PythonReflectionTests, NativeCallContextsRestoreAndDocumentPolicyWinsOverHeadlessWrites)
    {
        auto* World = NewObject<DWorld>(Host.Get(), "ScriptWorld"); World->SetWorldType(EWorldType::Editor);
        ASSERT_TRUE(World->InitializeSubsystems());
        auto* Level = NewObject<DLevel>(World, "ScriptLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
        FEditorEngineTestAccess::SetEditorWorld(*Host, World);
        FScopedEditorScriptCall Outer({Host.Get(), Tools->GetService()->GetWorkGate()});
        EXPECT_EQ(Outer.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Unavailable);
        {
            FScopedEditorScriptCall Inner({Host.Get(), Tools->GetService()->GetWorkGate(), EEditorScriptEditPolicy::NonDocumentReadWrite});
            EXPECT_EQ(FScopedEditorScriptCall::Current(), &Inner);
            EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Writable);
            bool ReadOnly = true;
            FEditorScriptTargetPolicyRegistration Policy(*Host, *Level, [&] { return ReadOnly ? EEditorScriptTargetAccess::ReadOnly : EEditorScriptTargetAccess::Writable; });
            EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::ReadOnly);
            ReadOnly = false; EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Writable);
            FEditorScriptTargetPolicyRegistration Conflict(*Host, *Level, [] { return EEditorScriptTargetAccess::Writable; });
            EXPECT_EQ(Inner.ResolveEditAccess(*Level), EEditorScriptTargetAccess::Unavailable);
            Inner.Reject("inner"); EXPECT_FALSE(Outer.GetError().has_value());
        }
        EXPECT_EQ(FScopedEditorScriptCall::Current(), &Outer);
    }
}
