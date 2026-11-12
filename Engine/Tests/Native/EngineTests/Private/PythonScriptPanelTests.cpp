#include "Panels/PythonScriptPanel.h"
#include "DurinPythonModule.h"
#include "PythonEditorSubsystem.h"
#include "ContentBrowserDataSubsystem.h"
#include "Editor/EditorEnvironmentTestSupport.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/EditorScriptCall.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Class.h"
#include "Engine/World.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Transactions/Transactor.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
    class FPythonBrowserSource final : public DContentBrowserDataSource
    {
        static auto GetPrivateStaticClass() -> DClass*;
        DECLARE_CLASS(FPythonBrowserSource, DContentBrowserDataSource, FPythonBrowserSource::GetPrivateStaticClass)
        DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FPythonBrowserSource)
    public:
        explicit FPythonBrowserSource(const FObjectInitializer& I) : Super(I) {}
        struct FPayload final : ContentBrowser::IContentBrowserItemDataPayload { std::string Path; };
        auto MakeItem(const std::filesystem::path& Path, ContentBrowser::EContentBrowserItemFlags Flags = ContentBrowser::EContentBrowserItemFlags::Type_File) -> ContentBrowser::FContentBrowserItem
        {
            const auto Text = Path.u8string();
            auto Payload = std::make_shared<FPayload>(); Payload->Path.assign(reinterpret_cast<const char*>(Text.data()), Text.size());
            const auto Leaf = Path.filename().u8string();
            const std::string Name(reinterpret_cast<const char*>(Leaf.data()), Leaf.size());
            return {.InternalItems = {ContentBrowser::FContentBrowserItemData(*this, Flags, "/PythonScripts/" + Name, Name, "", std::move(Payload))}};
        }
        auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData& Item, std::string& Out) const -> bool override
        {
            Out.clear();
            const auto Payload = std::dynamic_pointer_cast<const FPayload>(Item.GetPayload());
            if (Item.GetOwnerDataSource() != this || !Payload) return false;
            Out = Payload->Path; return true;
        }
    };
    auto FPythonBrowserSource::GetPrivateStaticClass() -> DClass*
    {
        static DClass* Class = nullptr;
        if (!Class) GetPrivateStaticClassBody("", "FPythonBrowserSource", Class, nullptr, sizeof(FPythonBrowserSource), alignof(FPythonBrowserSource), EClassFlags::None, InternalConstructor<FPythonBrowserSource>, &DContentBrowserDataSource::StaticClass);
        return Class;
    }
}

namespace Durin::MainFrame
{
    struct FPythonScriptPanelTestAccess
    {
        static auto Job(const FPythonScriptPanel& Panel) -> uint64 { return Panel.Job; }
        static auto Result(const FPythonScriptPanel& Panel) -> const std::optional<FPythonJobResult>& { return Panel.Result; }
        static auto Message(const FPythonScriptPanel& Panel) -> const std::string& { return Panel.Message; }
        static auto OpenRequested(const FPythonScriptPanel& Panel) -> bool { return Panel.OpenRequested; }
    };
}

namespace
{
    using namespace Durin;
    using namespace Durin::MainFrame;
    namespace Browser = Durin::ContentBrowser;

    class PythonScriptPanelTests : public testing::Test
    {
    protected:
        TStrongObjectPtr<DEditorEngine> Host;
        TStrongObjectPtr<FPythonBrowserSource> BrowserSource;
        struct FBrowserSink final : Browser::IContentBrowserItemDataSink
        {
            auto QueueItemDataUpdate(Browser::FContentBrowserItemDataUpdate&&) -> void override {}
        } BrowserSink;
        DEditorToolSubsystem* Tools = nullptr;
        std::unique_ptr<FPythonScriptPanel> Panel;
        FDurinPythonModule* Python = nullptr;
        auto SetUp() -> void override
        {
            Testing::InitializeDObjectSystemForTests();
            ASSERT_TRUE(FModuleManager::Get().LoadModule("Engine"));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("DurinEd"));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("ContentBrowser"));
            Python = &FModuleManager::LoadModuleChecked<FDurinPythonModule>("DurinPython");
            BrowserSource = NewObject<FPythonBrowserSource>(nullptr, {});
            BrowserSource->Initialize(); BrowserSource->SetDataSink(&BrowserSink);
            Host = NewObject<DEditorEngine>(nullptr, {});
            ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
            Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>(); ASSERT_NE(Tools, nullptr);
            Panel = std::make_unique<FPythonScriptPanel>(*Host);
        }
        auto TearDown() -> void override
        {
            Panel.reset();
            if (BrowserSource) { BrowserSource->Shutdown(); MarkObjectHierarchyAsGarbage(BrowserSource.Get()); BrowserSource.Reset(); }
            if (Host) { Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); }
            CollectGarbage(Durin::EObjectFlags::Standalone);
            if (Python) { EXPECT_EQ(0u, Python->GetSession()); EXPECT_EQ(0u, Python->GetStartupJob()); }
        }
        auto RestartHostWithoutPanel() -> void
        {
            Panel.reset(); Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
            Host = NewObject<DEditorEngine>(nullptr, {});
            ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
            Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
            ASSERT_NE(Tools, nullptr);
        }
        auto Action() -> Browser::FExtensionDescriptor
        {
            for (const auto& Entry : Browser::CaptureExtensions(Browser::EExtensionCategory::ContextMenu))
                if (Entry.Id == "MainFrame.RunPythonScript") return Entry;
            ADD_FAILURE() << "Python action not registered"; return {};
        }
        auto Context(const std::filesystem::path& Path) -> Browser::FExtensionContext
        {
            const auto Item = BrowserSource->MakeItem(Path);
            return {.Selection = {Item}, .PrimaryItem = Item};
        }
        static auto Script(std::string_view Source, std::string_view Filename = "browser_script.py") -> std::filesystem::path
        {
            const auto Path = Testing::GetTestWorkDirectory() / std::filesystem::u8path(Filename);
            std::ofstream Stream(Path, std::ios::binary); Stream << Source; Stream.close();
            EXPECT_TRUE(Stream); return Path;
        }
        auto Pump() -> void
        {
            for (int I = 0; I < 100; ++I)
            {
                Tools->TickScripts(); Panel->Tick();
                const auto& Result = FPythonScriptPanelTestAccess::Result(*Panel);
                if (Result && Result->State != EPythonJobState::Queued && Result->State != EPythonJobState::Running) return;
            }
            FAIL() << "Script did not finish";
        }
    };

    TEST_F(PythonScriptPanelTests, BrowserActionFiltersSelectionAndRespectsMutationPolicy)
    {
        const auto Entry = Action();
        auto Selection = Context(Script("print('browser-run')", "browser \xc3\xa9.PY"));
        EXPECT_TRUE(Entry.IsApplicable(Selection));
        EXPECT_FALSE(Browser::CanInvokeExtension(Entry, Selection, false));
        EXPECT_FALSE(Browser::InvokeExtension(Entry, {.Context = Selection, .bAllowAssetMutation = false}));
        EXPECT_EQ(FPythonScriptPanelTestAccess::Job(*Panel), 0u);
        auto Invalid = Selection; Invalid.Selection.clear(); EXPECT_FALSE(Entry.IsApplicable(Invalid));
        Invalid = Selection; Invalid.Selection.push_back(*Selection.PrimaryItem); EXPECT_FALSE(Entry.IsApplicable(Invalid));
        Invalid = Selection; Invalid.PrimaryItem = BrowserSource->MakeItem("folder", Browser::EContentBrowserItemFlags::Type_Folder); EXPECT_FALSE(Entry.IsApplicable(Invalid));
        Invalid = Context("not_python.txt"); EXPECT_FALSE(Entry.IsApplicable(Invalid));
        Invalid.PrimaryItem = BrowserSource->MakeItem("asset", Browser::EContentBrowserItemFlags::Type_File | Browser::EContentBrowserItemFlags::Category_Asset); EXPECT_FALSE(Entry.IsApplicable(Invalid));

        ASSERT_TRUE(Browser::InvokeExtension(Entry, {.Context = Selection}));
        EXPECT_TRUE(FPythonScriptPanelTestAccess::OpenRequested(*Panel));
        const auto Job = FPythonScriptPanelTestAccess::Job(*Panel); ASSERT_NE(Job, 0u);
        std::string PhysicalPath; ASSERT_TRUE(Selection.PrimaryItem->GetItemPhysicalPath(PhysicalPath));
        EXPECT_FALSE(Panel->RunFile(PhysicalPath));
        EXPECT_EQ(FPythonScriptPanelTestAccess::Job(*Panel), Job);
        Pump();
        const auto& Result = FPythonScriptPanelTestAccess::Result(*Panel); ASSERT_TRUE(Result);
        EXPECT_EQ(Result->State, EPythonJobState::Succeeded) << Result->Error;
        EXPECT_NE(Result->Output.find("browser-run"), std::string::npos);
    }

    TEST_F(PythonScriptPanelTests, AdmissionAndScriptErrorsRemainVisibleAndRetiredActionsCannotRun)
    {
        const auto Entry = Action(); std::string Error;
        ASSERT_TRUE(Browser::InvokeExtension(Entry, {
            .Context = Context(Testing::GetTestWorkDirectory() / "missing.py"),
            .ReportError = [&](std::string Text) { Error = std::move(Text); }}));
        EXPECT_NE(Error.find("open Python script"), std::string::npos);
        EXPECT_TRUE(FPythonScriptPanelTestAccess::OpenRequested(*Panel));
        ASSERT_TRUE(Panel->RunFile(Script("raise RuntimeError('browser-failure')")));
        Pump();
        const auto& Result = FPythonScriptPanelTestAccess::Result(*Panel); ASSERT_TRUE(Result);
        EXPECT_EQ(Result->State, EPythonJobState::Failed);
        EXPECT_NE(Result->Error.find("browser-failure"), std::string::npos);
        Panel.reset();
        EXPECT_FALSE(Browser::InvokeExtension(Entry, {.Context = Context("retired.py")}));
    }

    TEST_F(PythonScriptPanelTests, CancelledPanelJobAllowsTheNextFileToRun)
    {
        ASSERT_TRUE(Panel->RunFile(Script("import durin\ndef steps():\n    while True:\n        yield None\ndurin.defer(steps())\n")));
        Tools->TickScripts(); Panel->Tick();
        const auto Job = FPythonScriptPanelTestAccess::Job(*Panel);
        ASSERT_TRUE(Python->GetRuntime().CancelJob(Job));
        Pump();
        ASSERT_TRUE(FPythonScriptPanelTestAccess::Result(*Panel));
        EXPECT_EQ(FPythonScriptPanelTestAccess::Result(*Panel)->State, EPythonJobState::Cancelled);
        ASSERT_TRUE(Panel->RunFile(Script("print('after-cancel')")));
        Pump();
        EXPECT_EQ(FPythonScriptPanelTestAccess::Result(*Panel)->State, EPythonJobState::Succeeded);
    }

    TEST_F(PythonScriptPanelTests, ExampleBuildsAnEmptySceneAndSupportsUndoRedo)
    {
        auto* World = NewObject<DWorld>(Host.Get(), "ScriptWorld"); World->SetWorldType(EWorldType::Editor);
        ASSERT_NO_THROW(World->InitializeSubsystems());
        auto* Level = NewObject<DLevel>(World, "ScriptLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
        FEditorEngineTestAccess::SetEditorWorld(*Host, World);
        FEditorScriptTargetPolicyRegistration Policy(*Host, *Level, [] { return EEditorScriptTargetAccess::Writable; });
        ASSERT_TRUE(Level->GetActors().empty());
        ASSERT_TRUE(Panel->RunFile(std::filesystem::u8path(DURIN_PYTHON_EXAMPLE_PATH)));
        Pump();
        const auto& Result = FPythonScriptPanelTestAccess::Result(*Panel); ASSERT_TRUE(Result);
        ASSERT_EQ(Result->State, EPythonJobState::Succeeded) << Result->Error;
        ASSERT_EQ(Level->GetActors().size(), 6u);
        const auto FindCamera = [&]() -> AActor* {
            for (const auto& Actor : Level->GetActors())
                if (Actor->GetActorLabel() == "Example_Camera") return Actor.Get();
            return nullptr;
        };
        auto* Camera = FindCamera(); ASSERT_NE(Camera, nullptr);
        const FName OriginalCameraName = Camera->GetFName();
        EXPECT_DOUBLE_EQ(Camera->GetActorTransform().Translation.z, 4.0);
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_TRUE(Level->GetActors().empty());
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(Level->GetActors().size(), 6u);
        EXPECT_EQ(FindCamera(), Camera);
        ASSERT_TRUE(Panel->RunFile(std::filesystem::u8path(DURIN_PYTHON_EXAMPLE_PATH)));
        Pump();
        EXPECT_EQ(FPythonScriptPanelTestAccess::Result(*Panel)->State, EPythonJobState::Failed);
        EXPECT_EQ(Level->GetActors().size(), 6u);
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
        // Undo + rerun reuses visible labels, while retained object names stay unique.
        ASSERT_TRUE(Host->GetTransactor()->Undo());
        ASSERT_TRUE(Panel->RunFile(std::filesystem::u8path(DURIN_PYTHON_EXAMPLE_PATH)));
        Pump();
        ASSERT_EQ(FPythonScriptPanelTestAccess::Result(*Panel)->State, EPythonJobState::Succeeded);
        ASSERT_EQ(Level->GetActors().size(), 6u);
        auto* Recreated = FindCamera(); ASSERT_NE(Recreated, nullptr);
        EXPECT_NE(Recreated, Camera);
        EXPECT_NE(Recreated->GetFName(), OriginalCameraName);
        EXPECT_EQ(Recreated->GetActorLabel(), "Example_Camera");
        // Renaming labels must not defeat the example's persistent membership marker.
        ASSERT_TRUE(Recreated->SetActorLabel("User camera label"));
        for (const auto& Actor : Level->GetActors()) ASSERT_TRUE(Actor->SetActorLabel("Changed"));
        ASSERT_TRUE(Panel->RunFile(std::filesystem::u8path(DURIN_PYTHON_EXAMPLE_PATH)));
        Pump();
        EXPECT_EQ(FPythonScriptPanelTestAccess::Result(*Panel)->State, EPythonJobState::Failed);
        EXPECT_EQ(Level->GetActors().size(), 6u);
        ASSERT_TRUE(Host->GetTransactor()->Undo());
        ASSERT_TRUE(Host->GetTransactor()->Redo());
        EXPECT_EQ(Level->GetActors().size(), 6u);
    }
}

TEST_F(PythonScriptPanelTests, StartupScriptRunsOnceWithoutMainFrameAndPanelsObserveTheExistingJob)
{
    const auto File = Script("print('startup once')", "startup_owned.py");
    Testing::FScopedEditorEnvironment Environment("DURIN_PYTHON_SCRIPT", File.string());
    RestartHostWithoutPanel();
    ASSERT_NE(Host->GetEditorSubsystem<DPythonEditorSubsystem>(), nullptr);
    const auto Job = Python->GetStartupJob();
    ASSERT_NE(Job, 0u);
    Tools->TickScripts();
    const auto Result = Python->GetRuntime().GetJobResult(Job);
    ASSERT_TRUE(Result);
    EXPECT_EQ(Result->State, EPythonJobState::Succeeded);
    EXPECT_EQ(Result->Output, "startup once\n");
    ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems());
    EXPECT_EQ(Job, Python->GetStartupJob());
    for (int Reopen = 0; Reopen < 2; ++Reopen)
    {
        Panel = std::make_unique<FPythonScriptPanel>(*Host);
        EXPECT_EQ(Job, FPythonScriptPanelTestAccess::Job(*Panel));
        EXPECT_TRUE(FPythonScriptPanelTestAccess::OpenRequested(*Panel));
        Panel->Tick();
        ASSERT_TRUE(FPythonScriptPanelTestAccess::Result(*Panel));
        EXPECT_EQ(FPythonScriptPanelTestAccess::Result(*Panel)->Output, "startup once\n");
        Panel.reset();
        EXPECT_NE(0u, Python->GetSession());
    }
}

TEST_F(PythonScriptPanelTests, SessionStartupPreservesTheRuntimeFailure)
{
    FPythonRuntime OtherRuntime;
    ASSERT_TRUE(OtherRuntime.Start());
    const auto Submitted = Python->RunFileForEditor(*Host, Script("pass", "runtime_conflict.py"));
    ASSERT_FALSE(Submitted);
    EXPECT_NE(std::string::npos, Submitted.error().find("Another Durin Python runtime owns the interpreter"));
    EXPECT_EQ(0u, Python->GetSession());
}

TEST_F(PythonScriptPanelTests, ProviderShutdownStateStillClosesTheEditorSession)
{
    ASSERT_TRUE(Python->RunFileForEditor(*Host, Script("pass", "provider_shutdown.py")));
    ASSERT_NE(0u, Python->GetSession());
    const auto Info = FModuleManager::Get().FindModule("DurinPython");
    const auto Previous = Info->State.exchange(EModuleState::ShuttingDown);
    struct FRestoreState
    {
        FModuleManager::FModuleInfoPtr Info;
        EModuleState Previous;
        ~FRestoreState() { Info->State = Previous; }
    } Restore{Info, Previous};
    Host->PreExit();
    EXPECT_EQ(0u, Python->GetSession());
}

TEST_F(PythonScriptPanelTests, StartupSubmissionFailureRemainsVisibleWithoutRetryingFromThePanel)
{
    const auto File = Testing::GetTestWorkDirectory() / "absent_startup_script.py";
    Testing::FScopedEditorEnvironment Environment("DURIN_PYTHON_SCRIPT", File.string());
    RestartHostWithoutPanel();
    EXPECT_EQ(0u, Python->GetStartupJob());
    ASSERT_FALSE(Python->GetStartupError().empty());
    Panel = std::make_unique<FPythonScriptPanel>(*Host);
    EXPECT_TRUE(FPythonScriptPanelTestAccess::OpenRequested(*Panel));
    EXPECT_EQ(Python->GetStartupError(), FPythonScriptPanelTestAccess::Message(*Panel));
    Panel->Tick();
    EXPECT_EQ(0u, FPythonScriptPanelTestAccess::Job(*Panel));
}
