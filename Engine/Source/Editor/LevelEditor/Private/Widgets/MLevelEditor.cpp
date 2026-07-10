#include "Widgets/MLevelEditor.h"

#include "AssetSystem.h"
#include "Actors/CameraActor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"
#include "Panels/DetailsPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/OutputLogPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/WorldOutlinerPanel.h"

namespace Durin
{
	namespace
	{
		constexpr const char* DockSpaceName = "DurinEditorDockSpace";
	}

	MLevelEditor::MLevelEditor() = default;
	MLevelEditor::~MLevelEditor() = default;

	auto MLevelEditor::Construct() -> void
	{
		Context = std::make_unique<FLevelEditorContext>();
		Asset::GetAssetRegistry().ScanMountedContent();
		Panels.emplace_back(std::make_unique<FSceneViewportPanel>());
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		Panels.emplace_back(std::make_unique<FDetailsPanel>());
		Panels.emplace_back(std::make_unique<FOutputLogPanel>());
	}

	auto MLevelEditor::Draw() -> void
	{
		if (!Context)
		{
			return;
		}

		Context->Synchronize(GEngine != nullptr ? GEngine->GetWorld() : nullptr);
		const ImGuiIO& IO = ImGui::GetIO();
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) RequestFileAction(EPendingFileAction::NewLevel);
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) RequestFileAction(EPendingFileAction::OpenLevel);
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveCurrentLevel();
		DrawMainMenu();
		DrawFileDialogs();

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
			if (Panel->IsOpen())
			{
				Panel->Draw(*Context);
			}
		}
	}

	auto MLevelEditor::DrawMainMenu() -> void
	{
		if (!ImGui::BeginMainMenuBar())
		{
			return;
		}

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Level", "Ctrl+N")) RequestFileAction(EPendingFileAction::NewLevel);
			if (ImGui::MenuItem("Open Level", "Ctrl+O")) RequestFileAction(EPendingFileAction::OpenLevel);
			if (ImGui::MenuItem("Save Level", "Ctrl+S", false, Context && Context->Level && Context->Level->GetPackage())) SaveCurrentLevel();
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
			ImGui::MenuItem("Undo/redo is not available yet", nullptr, false, false);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::MenuItem("Reset Layout"))
			{
				bResetLayoutRequested = true;
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Window"))
		{
			for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
			{
				bool bOpen = Panel->IsOpen();
				if (ImGui::MenuItem(Panel->GetWindowName(), nullptr, &bOpen))
				{
					Panel->SetOpen(bOpen);
				}
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

	auto MLevelEditor::RequestFileAction(EPendingFileAction Action) -> void
	{
		PendingFileAction = Action;
		if (Context && Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty())
		{
			ImGui::OpenPopup("Unsaved Level");
			return;
		}
		ExecutePendingFileAction();
	}

	auto MLevelEditor::ExecutePendingFileAction() -> void
	{
		if (PendingFileAction == EPendingFileAction::NewLevel)
		{
			LevelPathBuffer.fill(0);
			const std::string DefaultPath = "/SandBox/Levels/NewLevel";
			std::memcpy(LevelPathBuffer.data(), DefaultPath.data(), DefaultPath.size());
			ImGui::OpenPopup("New Level");
		}
		else if (PendingFileAction == EPendingFileAction::OpenLevel)
		{
			OpenFilterBuffer.fill(0);
			ImGui::OpenPopup("Open Level");
		}
	}

	auto MLevelEditor::DrawFileDialogs() -> void
	{
		if (ImGui::BeginPopupModal("Unsaved Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("The current level has unsaved changes.");
			if (ImGui::Button("Save"))
			{
				if (SaveCurrentLevel()) { ImGui::CloseCurrentPopup(); ExecutePendingFileAction(); }
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard")) { ImGui::CloseCurrentPopup(); ExecutePendingFileAction(); }
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) { PendingFileAction = EPendingFileAction::None; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("New Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Virtual Path", LevelPathBuffer.data(), LevelPathBuffer.size());
			if (ImGui::Button("Create")) { CreateLevel(LevelPathBuffer.data()); if (EditorError.empty()) ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) { PendingFileAction = EPendingFileAction::None; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Open Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputTextWithHint("##LevelFilter", "Filter virtual paths...", OpenFilterBuffer.data(), OpenFilterBuffer.size());
			ImGui::BeginChild("LevelAssets", ImVec2(520.0f, 280.0f), true);
			const std::string Filter = OpenFilterBuffer.data();
			for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
			{
				if (Data.AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString()) continue;
				const std::string PathString = Path.ToString();
				if (!Filter.empty() && PathString.find(Filter) == std::string::npos) continue;
				if (ImGui::Selectable(PathString.c_str()))
				{
					OpenLevel(PathString);
					if (EditorError.empty()) ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndChild();
			if (ImGui::Button("Cancel")) { PendingFileAction = EPendingFileAction::None; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		if (!EditorError.empty()) ImGui::OpenPopup("Level Error");
		if (ImGui::BeginPopupModal("Level Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", EditorError.c_str());
			if (ImGui::Button("OK")) { EditorError.clear(); ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}

	auto MLevelEditor::CreateLevel(std::string_view PathString) -> void
	{
		EditorError.clear();
		FAssetPath Path;
		std::string PathError;
		if (!FAssetPath::TryCreate(PathString, Path, &PathError)) { SetError(PathError); return; }
		DLevel* Level = nullptr;
		Asset::FAssetResult Result = Asset::CreateAsset(Path, Level);
		if (!Result) { SetError(Result.Message); return; }
		ACameraActor* Camera = Level->SpawnActor<ACameraActor>("Camera");
		Level->SetPrimaryCameraActor(Camera);
		if (!ActivateLevel(Level)) Asset::UnloadPackage(Path);
		else PendingFileAction = EPendingFileAction::None;
	}

	auto MLevelEditor::OpenLevel(std::string_view PathString) -> void
	{
		EditorError.clear();
		FAssetPath Path;
		std::string PathError;
		if (!FAssetPath::TryCreate(PathString, Path, &PathError)) { SetError(PathError); return; }
		DLevel* Level = nullptr;
		Asset::FAssetResult Result = Asset::LoadAsset(Path, Level);
		if (!Result) { SetError(Result.Message); return; }
		if (!ActivateLevel(Level)) return;
		PendingFileAction = EPendingFileAction::None;
	}

	auto MLevelEditor::SaveCurrentLevel() -> bool
	{
		EditorError.clear();
		if (!Context || !Context->Level || !Context->Level->GetPackage()) { SetError("The current level is transient and cannot be saved."); return false; }
		Asset::FAssetResult Result = Asset::SavePackage(Context->Level->GetPackage());
		if (!Result) { SetError(Result.Message); return false; }
		return true;
	}

	auto MLevelEditor::ActivateLevel(DLevel* Level) -> bool
	{
		if (!Context || !Context->World || !Level) { SetError("No world is available to activate the level."); return false; }
		DLevel* Previous = Context->World->GetCurrentLevel();
		DPackage* PreviousPackage = Previous ? Previous->GetPackage() : nullptr;
		if (!Context->World->SetCurrentLevel(Level)) { SetError("The level is already active in another world."); return false; }
		Context->Synchronize(Context->World);
		if (PreviousPackage && PreviousPackage != Level->GetPackage())
		{
			FAssetPath PreviousPath;
			if (FAssetPath::TryCreate(PreviousPackage->GetPackagePath(), PreviousPath))
			{
				Asset::FAssetResult Result = Asset::UnloadPackage(PreviousPath);
				if (!Result && Result.Error != Asset::EAssetError::NotFound) DURIN_WARN("Failed to unload previous level: {}", Result.Message);
			}
		}
		return true;
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
		ImGui::DockBuilderDockWindow("Output Log###OutputLog", BottomDockId);
		ImGui::DockBuilderDockWindow("Scene Viewport###SceneViewport", MainDockId);
		ImGui::DockBuilderFinish(DockSpaceId);
	}
} // namespace Durin
