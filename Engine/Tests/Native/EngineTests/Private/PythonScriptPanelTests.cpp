#include "Panels/PythonScriptPanel.h"
#include "DurinPythonModule.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Tools/EditorToolSubsystem.h"
#include "Tools/EditorScriptCall.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Transactions/Transactor.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include <gtest/gtest.h>

namespace Durin::Editor::MainFrame
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
    using namespace Durin::Editor::MainFrame;
    namespace Browser = Durin::Editor::ContentBrowser;

    class PythonScriptPanelTests : public testing::Test
    {
    protected:
        TStrongObjectPtr<DEditorEngine> Host;
        DEditorToolSubsystem* Tools = nullptr;
        std::unique_ptr<FPythonScriptPanel> Panel;
        FDurinPythonModule* Python = nullptr;
        auto SetUp() -> void override
        {
            Testing::InitializeDObjectSystemForTests();
            ASSERT_TRUE(FModuleManager::Get().LoadModule("Engine"));
            ASSERT_TRUE(FModuleManager::Get().LoadModule("DurinEd"));
            Python = &FModuleManager::LoadModuleChecked<FDurinPythonModule>("DurinPython");
            Host = NewObject<DEditorEngine>(nullptr, {});
            ASSERT_TRUE(FEditorEngineTestAccess::StartEditor(*Host));
            Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>(); ASSERT_NE(Tools, nullptr);
            Panel = std::make_unique<FPythonScriptPanel>(*Host);
        }
        auto TearDown() -> void override
        {
            Panel.reset();
            if (Python) Python->StopRuntime();
            if (Host) { Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); }
            CollectGarbage();
        }
        auto Action() -> Browser::FExtensionDescriptor
        {
            for (const auto& Entry : Browser::CaptureExtensions(Browser::EExtensionCategory::ContextMenu))
                if (Entry.Id == "MainFrame.RunPythonScript") return Entry;
            ADD_FAILURE() << "Python action not registered"; return {};
        }
        static auto Context(const std::filesystem::path& Path) -> Browser::FExtensionContext
        {
            const auto Utf8 = Path.u8string();
            const std::string Text(reinterpret_cast<const char*>(Utf8.data()), Utf8.size());
            Browser::FExtensionItem Item{Browser::EExtensionItemKind::File, Text, {}, Text, {}};
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
        Invalid = Selection; Invalid.PrimaryItem->Kind = Browser::EExtensionItemKind::Directory; EXPECT_FALSE(Entry.IsApplicable(Invalid));
        Invalid = Context("not_python.txt"); EXPECT_FALSE(Entry.IsApplicable(Invalid));
        Invalid.PrimaryItem->Kind = Browser::EExtensionItemKind::Asset; EXPECT_FALSE(Entry.IsApplicable(Invalid));

        ASSERT_TRUE(Browser::InvokeExtension(Entry, {.Context = Selection}));
        EXPECT_TRUE(FPythonScriptPanelTestAccess::OpenRequested(*Panel));
        const auto Job = FPythonScriptPanelTestAccess::Job(*Panel); ASSERT_NE(Job, 0u);
        EXPECT_FALSE(Panel->RunFile(Selection.PrimaryItem->PhysicalPath));
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
        ASSERT_TRUE(World->InitializeSubsystems());
        auto* Level = NewObject<DLevel>(World, "ScriptLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
        FEditorEngineTestAccess::SetEditorWorld(*Host, World);
        FEditorScriptTargetPolicyRegistration Policy(*Host, *Level, [] { return EEditorScriptTargetAccess::Writable; });
        ASSERT_TRUE(Level->GetActors().empty());
        ASSERT_TRUE(Panel->RunFile(std::filesystem::u8path(DURIN_PYTHON_EXAMPLE_PATH)));
        Pump();
        const auto& Result = FPythonScriptPanelTestAccess::Result(*Panel); ASSERT_TRUE(Result);
        ASSERT_EQ(Result->State, EPythonJobState::Succeeded) << Result->Error;
        ASSERT_EQ(Level->GetActors().size(), 6u);
        auto* Camera = Level->FindActorByName("Example_Camera"); ASSERT_NE(Camera, nullptr);
        EXPECT_DOUBLE_EQ(Camera->GetActorTransform().Translation.z, 4.0);
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
        ASSERT_TRUE(Host->GetTransactor()->Undo()); EXPECT_TRUE(Level->GetActors().empty());
        ASSERT_TRUE(Host->GetTransactor()->Redo()); EXPECT_EQ(Level->GetActors().size(), 6u);
        EXPECT_EQ(Level->FindActorByName("Example_Camera"), Camera);
        ASSERT_TRUE(Panel->RunFile(std::filesystem::u8path(DURIN_PYTHON_EXAMPLE_PATH)));
        Pump();
        EXPECT_EQ(FPythonScriptPanelTestAccess::Result(*Panel)->State, EPythonJobState::Failed);
        EXPECT_EQ(Level->GetActors().size(), 6u);
        EXPECT_EQ(Cast<DTransBuffer>(Host->GetTransactor())->GetQueueLength(), 1u);
    }
}
