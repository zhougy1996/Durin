#include "Widgets/MLevelEditor.h"

#include "Application/MonaApplication.h"
#include "AssetSystem.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "EditorSessionSettings.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "IRendererModule.h"
#include "LevelDocumentController.h"
#include "LevelEditorContext.h"
#include "Misc/Project.h"
#include "MonaImGui.h"
#include "Panels/ConsolePanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/WorldOutlinerPanel.h"
#include "StaticMeshImportDialog.h"
#include "Widgets/MWindow.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	// MLevelEditor is the composition root for the editor-specific controllers.
	namespace
	{
		constexpr const char* DockSpaceName = "DurinEditorDockSpace";
	}

	MLevelEditor::MLevelEditor(FEditorSessionSettings& InSessionSettings)
		: SessionSettings(InSessionSettings)
	{
	}

	MLevelEditor::~MLevelEditor()
	{
		if (Context && SceneViewportPanel)
		{
			SessionSettings.CaptureViewportState(*Context, *SceneViewportPanel);
			SessionSettings.Save(SceneViewportPanel);
		}
	}

	auto MLevelEditor::Construct() -> void
	{
		Context = std::make_unique<FLevelEditorContext>();
		Context->ReportError = [this](std::string Message) { SetError(std::move(Message)); };
		Context->RenameLevel = [this](std::string_view NewName) {
			return DocumentController && DocumentController->RenameCurrentLevel(NewName);
		};

		Asset::GetAssetRegistry().ScanMountedContent();
		SessionSettings.PruneInvalidViewportStates();
		LoadProjectSettings();
		SessionSettings.Save(nullptr);

		auto SceneViewport = std::make_unique<FSceneViewportPanel>();
		SceneViewportPanel = SceneViewport.get();
		SessionSettings.ApplyTo(*SceneViewportPanel);
		Panels.emplace_back(std::move(SceneViewport));
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		Panels.emplace_back(std::make_unique<FDetailsPanel>());
		Panels.emplace_back(std::make_unique<FConsolePanel>());

		DocumentController = std::make_unique<FLevelDocumentController>(
			*Context,
			SessionSettings,
			*SceneViewportPanel,
			DefaultLevel,
			[this] { EditorError.clear(); },
			[this](std::string Message) { SetError(std::move(Message)); },
			[this] { return SaveProjectSettings(); }
		);
		StaticMeshImportDialog = std::make_unique<FStaticMeshImportDialog>(
			[this] { EditorError.clear(); },
			[this](std::string Message) { SetError(std::move(Message)); },
			[this](std::string AssetPath) {
				Asset::GetAssetRegistry().ScanMountedContent();
				if (ContentBrowserPanel) ContentBrowserPanel->RevealAsset(AssetPath);
			}
		);
		auto ContentBrowser = std::make_unique<FContentBrowserPanel>(
			SessionSettings,
			[this](const std::string& Path, const std::string& AssetClassName) {
				if (AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString()) return false;
				return DocumentController && DocumentController->RequestOpenLevel(Path);
			},
			[this](const std::string& DestinationDirectory) {
				if (StaticMeshImportDialog) StaticMeshImportDialog->Open(DestinationDirectory);
			}
		);
		ContentBrowserPanel = ContentBrowser.get();
		Panels.emplace_back(std::move(ContentBrowser));

		Context->Synchronize(GEngine != nullptr ? GEngine->GetWorld() : nullptr);
		DocumentController->OpenDefaultLevel();
		if (!EditorError.empty())
		{
			DURIN_WARN("Could not open project default level {}: {}", DefaultLevel, EditorError);
			EditorError.clear();
		}
	}

	auto MLevelEditor::LoadProjectSettings() -> bool
	{
		DefaultLevel.clear();
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project) return false;
		const std::string File = Project->ProjectDir + "Configs/Project.yaml";
		if (!std::filesystem::exists(File)) return true;
		FYamlDocument Document;
		FYamlParseError Error;
		if (!Document.LoadFromFile(File, &Error))
		{
			DURIN_WARN("Failed to load project settings: {}", Error.Message);
			return false;
		}
		DefaultLevel = Document.GetRootView().GetView("Editor").GetView("DefaultLevel").GetString();
		return true;
	}

	auto MLevelEditor::SaveProjectSettings() -> bool
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project)
		{
			SetError("No project is open.");
			return false;
		}
		if (!DefaultLevel.empty() && !DefaultLevel.starts_with(Project->MountRoot))
		{
			SetError("The default level must belong to the current project.");
			return false;
		}
		const auto Found = std::ranges::find_if(Asset::GetAssetRegistry().GetAssets(), [this](const auto& Entry) {
			return Entry.first.ToString() == DefaultLevel && Entry.second.AssetClassName == DLevel::StaticClass()->GetQualifiedName().ToString();
		});
		if (!DefaultLevel.empty() && Found == Asset::GetAssetRegistry().GetAssets().end())
		{
			SetError("The default level is not a registered Level asset.");
			return false;
		}
		std::error_code Error;
		std::filesystem::create_directories(std::filesystem::path(Project->ProjectDir) / "Configs", Error);
		if (Error)
		{
			SetError("Could not create the project Configs directory.");
			return false;
		}
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		Root.AddMap("Editor").SetChildValue("DefaultLevel", DefaultLevel);
		if (!Document.SaveToFile(Project->ProjectDir + "Configs/Project.yaml"))
		{
			SetError("Could not save project settings.");
			return false;
		}
		return true;
	}

	auto MLevelEditor::Draw() -> void
	{
		if (!Context || !DocumentController || !StaticMeshImportDialog) return;

		Context->Synchronize(GEngine != nullptr ? GEngine->GetWorld() : nullptr);
		const ImGuiIO& IO = ImGui::GetIO();
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) DocumentController->RequestAction(ELevelDocumentAction::NewLevel);
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) DocumentController->SaveCurrentLevel();
		const bool bGizmoDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
		if (!bGizmoDragging && !IO.WantTextInput && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && GEditor) GEditor->GetTransactionManager().Undo();
		if (!bGizmoDragging && !IO.WantTextInput && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && GEditor) GEditor->GetTransactionManager().Redo();

		DrawMainMenu();
		DocumentController->DrawDialogs();
		StaticMeshImportDialog->Draw();
		DrawProjectSettings();

		if (!EditorError.empty()) ImGui::OpenPopup("Editor Error");
		if (ImGui::BeginPopupModal("Editor Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", EditorError.c_str());
			if (ImGui::Button("OK"))
			{
				EditorError.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow())
		{
			const bool bCurrentMaximized = Window->IsMaximized();
			if (bCurrentMaximized != SessionSettings.IsWindowMaximized())
			{
				SessionSettings.SetWindowMaximized(bCurrentMaximized);
				SessionSettings.Save(SceneViewportPanel);
			}
		}

		ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		const ImGuiID DockSpaceId = ImGui::GetID(DockSpaceName);
		const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
		ImGui::DockSpaceOverViewport(DockSpaceId, MainViewport, ImGuiDockNodeFlags_None);
		if (bNeedsDefaultLayout || bResetLayoutRequested)
		{
			BuildDefaultLayout(DockSpaceId);
			bResetLayoutRequested = false;
		}

		for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
		{
			if (Panel->IsOpen()) Panel->Draw(*Context);
		}
	}

	auto MLevelEditor::DrawMainMenu() -> void
	{
		if (!ImGui::BeginMainMenuBar()) return;

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Level", "Ctrl+N")) DocumentController->RequestAction(ELevelDocumentAction::NewLevel);
			if (ImGui::MenuItem("Save Level", "Ctrl+S", false, Context && Context->Level && Context->Level->GetPackage())) DocumentController->SaveCurrentLevel();
			if (ImGui::MenuItem("Set Current Level as Project Default", nullptr, false, Context && Context->Level && Context->Level->GetPackage()))
			{
				DefaultLevel = Context->Level->GetPackage()->GetPackagePath();
				SaveProjectSettings();
			}
			if (ImGui::MenuItem("Open Project...")) DocumentController->RequestAction(ELevelDocumentAction::OpenProject);
			ImGui::Separator();
			if (ImGui::BeginMenu("Import"))
			{
				if (ImGui::MenuItem("Static Mesh...")) StaticMeshImportDialog->Open();
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (Context && Context->Level)
			{
				DPackage* Package = Context->Level->GetPackage();
				const std::string Label = Package ? Package->GetPackagePath() + (Package->IsDirty() ? " *" : "") : "Transient Level";
				ImGui::TextDisabled("%s", Label.c_str());
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
			FEditorTransactionManager* Transactions = GEditor && !bDragging ? &GEditor->GetTransactionManager() : nullptr;
			const std::string UndoLabel = Transactions && Transactions->CanUndo() ? std::format("Undo {}", Transactions->GetUndoDescription()) : "Undo";
			const std::string RedoLabel = Transactions && Transactions->CanRedo() ? std::format("Redo {}", Transactions->GetRedoDescription()) : "Redo";
			if (ImGui::MenuItem(UndoLabel.c_str(), "Ctrl+Z", false, Transactions && Transactions->CanUndo())) Transactions->Undo();
			if (ImGui::MenuItem(RedoLabel.c_str(), "Ctrl+Y", false, Transactions && Transactions->CanRedo())) Transactions->Redo();
			ImGui::Separator();
			if (ImGui::MenuItem("Project Settings...")) bProjectSettingsOpen = true;
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::BeginMenu("Display"))
			{
				if (ImGui::BeginMenu("Color Theme"))
				{
					const MonaImGui::EColorTheme CurrentTheme = MonaImGui::GetColorTheme();
					if (ImGui::MenuItem("Dark", nullptr, CurrentTheme == MonaImGui::EColorTheme::Dark))
					{
						MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Dark);
						SessionSettings.Save(SceneViewportPanel);
					}
					if (ImGui::MenuItem("Light", nullptr, CurrentTheme == MonaImGui::EColorTheme::Light))
					{
						MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Light);
						SessionSettings.Save(SceneViewportPanel);
					}
					ImGui::EndMenu();
				}
				ImGui::Separator();
				ImGui::SeparatorText("UI Scale");
				for (const float Scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
				{
					const std::string Label = std::format("{}%", static_cast<int32>(Scale * 100.0f));
					if (ImGui::MenuItem(Label.c_str(), nullptr, std::abs(SessionSettings.GetUIScale() - Scale) < 0.01f))
						ApplyDisplaySettings(SessionSettings.GetWindowWidth(), SessionSettings.GetWindowHeight(), Scale);
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Reset Layout")) bResetLayoutRequested = true;
			ImGui::Separator();
			if (GEngine)
			{
				if (IRendererModule* RendererModule = GEngine->GetRendererModule())
				{
					bool bEnableFXAA = RendererModule->IsFXAAEnabled();
					if (ImGui::MenuItem("FXAA", nullptr, &bEnableFXAA)) RendererModule->SetFXAAEnabled(bEnableFXAA);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Window"))
		{
			for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
			{
				bool bOpen = Panel->IsOpen();
				if (ImGui::MenuItem(Panel->GetWindowName(), nullptr, &bOpen)) Panel->SetOpen(bOpen);
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("Durin Level Editor - early development", nullptr, false, false);
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	auto MLevelEditor::DrawProjectSettings() -> void
	{
		if (!bProjectSettingsOpen) return;
		if (ImGui::Begin("Project Settings", &bProjectSettingsOpen))
		{
			const FProjectInfo* Project = GetCurrentProject();
			if (Project)
			{
				ImGui::Text("Project: %s", Project->Name.c_str());
				ImGui::TextWrapped("Path: %s", Project->ProjectFile.c_str());
				ImGui::SeparatorText("Editor Default Level");
				if (ImGui::BeginCombo("Default Level", DefaultLevel.empty() ? "None" : DefaultLevel.c_str()))
				{
					if (ImGui::Selectable("None", DefaultLevel.empty())) DefaultLevel.clear();
					for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
					{
						const std::string Value = Path.ToString();
						if (!Value.starts_with(Project->MountRoot) || Data.AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString()) continue;
						if (ImGui::Selectable(Value.c_str(), Value == DefaultLevel)) DefaultLevel = Value;
					}
					ImGui::EndCombo();
				}
				if (ImGui::Button("Save")) SaveProjectSettings();
			}
		}
		ImGui::End();
	}

	auto MLevelEditor::ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void
	{
		SessionSettings.SetDisplaySettings(Width, Height, Scale);
		MonaImGui::SetGlobalUIScale(Scale);
		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow())
		{
			if (!Window->IsMaximized()) Window->ResizeWindow({static_cast<float>(Width), static_cast<float>(Height)});
		}
		SessionSettings.Save(SceneViewportPanel);
	}

	auto MLevelEditor::SetError(std::string Message) -> void
	{
		EditorError = std::move(Message);
		DURIN_ERROR("Level editor: {}", EditorError);
	}

	auto MLevelEditor::BuildDefaultLayout(uint32 DockSpaceId) -> void
	{
		ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		ImGui::DockBuilderRemoveNode(DockSpaceId);
		ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(DockSpaceId, MainViewport->WorkSize);

		ImGuiID MainDockId = DockSpaceId;
		const ImGuiID LeftDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Left, 0.20f, nullptr, &MainDockId);
		const ImGuiID RightDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Right, 0.25f, nullptr, &MainDockId);
		const ImGuiID BottomDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Down, 0.25f, nullptr, &MainDockId);

		ImGui::DockBuilderDockWindow("World Outliner###WorldOutliner", LeftDockId);
		ImGui::DockBuilderDockWindow("Details###Details", RightDockId);
		ImGui::DockBuilderDockWindow("Content Browser###FileBrowser", BottomDockId);
		ImGui::DockBuilderDockWindow("Output Log###OutputLog", BottomDockId);
		ImGui::DockBuilderDockWindow("Scene Viewport###SceneViewport", MainDockId);
		ImGui::DockBuilderFinish(DockSpaceId);
	}
} // namespace Durin
