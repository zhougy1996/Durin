#include "Widgets/MLevelEditor.h"

#include "AssetSystem.h"
#include "Dialogs/FileDialog.h"
#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "IRendererModule.h"
#include "LevelEditorContext.h"
#include "LevelViewportSessionSettings.h"
#include "Misc/StringConvert.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "MonaImGui.h"
#include "Application/GenericApplication.h"
#include "Application/MonaApplication.h"
#include "Widgets/MWindow.h"
#include "Panels/DetailsPanel.h"
#include "Panels/FileBrowserPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/OutputLogPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/WorldOutlinerPanel.h"
#include "StaticMesh/StaticMesh.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	struct FLevelViewportSessionState
	{
		FLevelViewportStateMap States;
	};

	namespace
	{
		constexpr const char* DockSpaceName = "DurinEditorDockSpace";
		constexpr const char* SessionSettingsFileName = "LevelEditorSession.yaml";

	}

	MLevelEditor::MLevelEditor()
		: ViewportSessionState(std::make_unique<FLevelViewportSessionState>())
	{
	}
	MLevelEditor::~MLevelEditor()
	{
		CaptureCurrentViewportState();
		SaveSessionSettings();
	}

	auto MLevelEditor::Construct() -> void
	{
		Context = std::make_unique<FLevelEditorContext>();
		Context->ReportError = [this](std::string Message) { SetError(std::move(Message)); };
		Asset::GetAssetRegistry().ScanMountedContent();
		LoadSessionSettings();
		LoadProjectSettings();
		SaveSessionSettings();
		auto SceneViewport = std::make_unique<FSceneViewportPanel>();
		SceneViewportPanel = SceneViewport.get();
		Panels.emplace_back(std::move(SceneViewport));
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		Panels.emplace_back(std::make_unique<FDetailsPanel>());
		Panels.emplace_back(std::make_unique<FOutputLogPanel>());
		Panels.emplace_back(std::make_unique<FFileBrowserPanel>([this](const std::string& Path) { return RequestOpenLevel(Path); }));
		Context->Synchronize(GEngine != nullptr ? GEngine->GetWorld() : nullptr);
		OpenDefaultLevel();
	}

	auto MLevelEditor::LoadSessionSettings() -> bool
	{
		FYamlDocument Document;
		FYamlParseError Error;
		const std::string FilePath = FPaths::LaunchDir() + SessionSettingsFileName;
		if (!std::filesystem::exists(FilePath)) return true;
		if (!Document.LoadFromFile(FilePath, &Error))
		{
			DURIN_WARN("Failed to load level editor session settings: {}", Error.Message);
			return false;
		}

		const FYamlNodeView Root = Document.GetRootView();
		LoadLevelViewportStates(Root, ViewportSessionState->States);
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const auto ProjectStates = ViewportSessionState->States.find(Project->ProjectFile);
			if (ProjectStates != ViewportSessionState->States.end())
			{
				std::erase_if(ProjectStates->second, [](const auto& Entry) {
					FAssetPath Path;
					if (!FAssetPath::TryCreate(Entry.first, Path)) return true;
					const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(Path);
					return !Data || Data->AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString();
				});
				if (ProjectStates->second.empty()) ViewportSessionState->States.erase(ProjectStates);
			}
		}
		const FYamlNodeView Display = Root.GetView("Display");
		const std::vector<FMonitorInfo> Monitors = EnumerateMonitors();
		if (!Monitors.empty())
		{
			WindowWidth = std::min(1600, static_cast<int32>(Monitors.front().WorkSize.x * 0.9f));
			WindowHeight = std::min(1000, static_cast<int32>(Monitors.front().WorkSize.y * 0.9f));
			UIScale = Monitors.front().WorkSize.y >= 1800 ? 1.5f : Monitors.front().WorkSize.y >= 1300 ? 1.25f : 1.0f;
		}
		WindowWidth = static_cast<int32>(Display.GetView("WindowWidth").GetInt(WindowWidth));
		WindowHeight = static_cast<int32>(Display.GetView("WindowHeight").GetInt(WindowHeight));
		UIScale = static_cast<float>(Display.GetView("UIScale").GetDouble(UIScale));
		bWindowMaximized = Display.GetView("WindowMaximized").GetBool(true);
		return true;
	}

	auto MLevelEditor::SaveSessionSettings() const -> bool
	{
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		if (const FProjectInfo* Project = GetCurrentProject()) Root.SetChildValue("RecentProject", Project->ProjectFile);
		FYamlNodeRef Display = Root.AddMap("Display");
		Display.SetChildValue("WindowWidth", WindowWidth);
		Display.SetChildValue("WindowHeight", WindowHeight);
		Display.SetChildValue("UIScale", static_cast<double>(UIScale));
		Display.SetChildValue("WindowMaximized", bWindowMaximized);
		SaveLevelViewportStates(Root, ViewportSessionState->States);
		if (!Document.SaveToFile(FPaths::LaunchDir() + SessionSettingsFileName))
		{
			DURIN_WARN("Failed to save level editor session settings.");
			return false;
		}
		return true;
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
		if (!Document.LoadFromFile(File, &Error)) { DURIN_WARN("Failed to load project settings: {}", Error.Message); return false; }
		DefaultLevel = Document.GetRootView().GetView("Editor").GetView("DefaultLevel").GetString();
		return true;
	}

	auto MLevelEditor::SaveProjectSettings() -> bool
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project) { SetError("No project is open."); return false; }
		if (!DefaultLevel.empty() && !DefaultLevel.starts_with(Project->MountRoot)) { SetError("The default level must belong to the current project."); return false; }
		const auto Found = std::ranges::find_if(Asset::GetAssetRegistry().GetAssets(), [this](const auto& Entry) {
			return Entry.first.ToString() == DefaultLevel && Entry.second.AssetClassName == DLevel::StaticClass()->GetQualifiedName().ToString();
		});
		if (!DefaultLevel.empty() && Found == Asset::GetAssetRegistry().GetAssets().end()) { SetError("The default level is not a registered Level asset."); return false; }
		std::error_code Error;
		std::filesystem::create_directories(std::filesystem::path(Project->ProjectDir) / "Configs", Error);
		if (Error) { SetError("Could not create the project Configs directory."); return false; }
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot(); Root.EnsureMap();
		Root.AddMap("Editor").SetChildValue("DefaultLevel", DefaultLevel);
		if (!Document.SaveToFile(Project->ProjectDir + "Configs/Project.yaml")) { SetError("Could not save project settings."); return false; }
		return true;
	}

	auto MLevelEditor::OpenDefaultLevel() -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || DefaultLevel.empty() || !DefaultLevel.starts_with(Project->MountRoot)) return;
		OpenLevel(DefaultLevel);
		if (!EditorError.empty())
		{
			DURIN_WARN("Could not open project default level {}: {}", DefaultLevel, EditorError);
			EditorError.clear();
		}
	}

	auto MLevelEditor::CaptureCurrentViewportState() -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || !ViewportSessionState || !SceneViewportPanel || !Context || !Context->Level) return;
		DPackage* Package = Context->Level->GetPackage();
		if (!Package) return;
		FLevelViewportCameraState State;
		if (SceneViewportPanel->CaptureCameraState(Context->Level, State))
			ViewportSessionState->States[Project->ProjectFile][Package->GetPackagePath()] = State;
	}

	auto MLevelEditor::RestoreViewportState(DLevel* Level) -> void
	{
		if (!SceneViewportPanel || !Level) return;
		const FProjectInfo* Project = GetCurrentProject();
		DPackage* Package = Level->GetPackage();
		const FLevelViewportCameraState* State = nullptr;
		if (Project && Package && ViewportSessionState)
		{
			const auto ProjectIt = ViewportSessionState->States.find(Project->ProjectFile);
			if (ProjectIt != ViewportSessionState->States.end())
			{
				const auto LevelIt = ProjectIt->second.find(Package->GetPackagePath());
				if (LevelIt != ProjectIt->second.end()) State = &LevelIt->second;
			}
		}
		SceneViewportPanel->RestoreCameraState(Level, State);
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
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveCurrentLevel();
		DrawMainMenu();
		DrawFileDialogs();
		DrawProjectSettings();

		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow())
		{
			const bool bCurrentMaximized = Window->IsMaximized();
			if (bCurrentMaximized != bWindowMaximized)
			{
				bWindowMaximized = bCurrentMaximized;
				SaveSessionSettings();
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
			if (ImGui::MenuItem("Save Level", "Ctrl+S", false, Context && Context->Level && Context->Level->GetPackage())) SaveCurrentLevel();
			if (ImGui::MenuItem("Set Current Level as Project Default", nullptr, false, Context && Context->Level && Context->Level->GetPackage()))
			{
				DefaultLevel = Context->Level->GetPackage()->GetPackagePath();
				SaveProjectSettings();
			}
			if (ImGui::MenuItem("Open Project...")) RequestFileAction(EPendingFileAction::OpenProject);
			ImGui::Separator();
			if (ImGui::BeginMenu("Import"))
			{
				if (ImGui::MenuItem("Static Mesh..."))
				{
					ImportSourcePathBuffer.fill(0);
					ImportAssetPathBuffer.fill(0);
					LastSuggestedImportAssetPath.clear();
					QueuedFilePopup = EQueuedFilePopup::ImportStaticMesh;
				}
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
			if (ImGui::MenuItem("Project Settings...")) bProjectSettingsOpen = true;
			ImGui::MenuItem("Undo/redo is not available yet", nullptr, false, false);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::BeginMenu("Display"))
			{
				ImGui::SeparatorText("UI Scale");
				for (const float Scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
				{
					const std::string Label = std::format("{}%", static_cast<int32>(Scale * 100.0f));
					if (ImGui::MenuItem(Label.c_str(), nullptr, std::abs(UIScale - Scale) < 0.01f)) ApplyDisplaySettings(WindowWidth, WindowHeight, Scale);
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Reset Layout"))
			{
				bResetLayoutRequested = true;
			}
			ImGui::Separator();
			if (GEngine != nullptr)
			{
				if (IRendererModule* RendererModule = GEngine->GetRendererModule())
				{
					bool bEnableFXAA = RendererModule->IsFXAAEnabled();
					if (ImGui::MenuItem("FXAA", nullptr, &bEnableFXAA))
					{
						RendererModule->SetFXAAEnabled(bEnableFXAA);
					}
				}
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
		WindowWidth = Width;
		WindowHeight = Height;
		UIScale = Scale;
		MonaImGui::SetGlobalUIScale(UIScale);
		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow())
		{
			if (!Window->IsMaximized())
			{
				Window->ResizeWindow({static_cast<float>(WindowWidth), static_cast<float>(WindowHeight)});
			}
		}
		SaveSessionSettings();
	}

	auto MLevelEditor::RequestFileAction(EPendingFileAction Action) -> void
	{
		PendingLevelPath.clear();
		PendingFileAction = Action;
		if (Context && Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty())
		{
			QueuedFilePopup = EQueuedFilePopup::UnsavedLevel;
			return;
		}
		ExecutePendingFileAction();
	}

	auto MLevelEditor::RequestOpenLevel(std::string Path) -> bool
	{
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(Path, AssetPath)) return false;
		const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(AssetPath);
		if (!Data || Data->AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString()) return false;
		PendingLevelPath = std::move(Path);
		PendingFileAction = EPendingFileAction::OpenLevel;
		if (Context && Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty())
		{
			QueuedFilePopup = EQueuedFilePopup::UnsavedLevel;
			return true;
		}
		OpenLevel(PendingLevelPath);
		PendingLevelPath.clear();
		return true;
	}

	auto MLevelEditor::ExecutePendingFileAction() -> void
	{
		if (PendingFileAction == EPendingFileAction::NewLevel)
		{
			LevelPathBuffer.fill(0);
			const FProjectInfo* Project = GetCurrentProject();
			const std::string DefaultPath = Project ? Project->MountRoot + "Levels/NewLevel" : "/Levels/NewLevel";
			std::memcpy(LevelPathBuffer.data(), DefaultPath.data(), DefaultPath.size());
			QueuedFilePopup = EQueuedFilePopup::NewLevel;
		}
		else if (PendingFileAction == EPendingFileAction::OpenLevel)
		{
			if (!PendingLevelPath.empty())
			{
				const std::string Path = std::exchange(PendingLevelPath, {});
				OpenLevel(Path);
				return;
			}
			PendingFileAction = EPendingFileAction::None;
		}
		else if (PendingFileAction == EPendingFileAction::OpenProject)
		{
			std::string Error;
			if (!RelaunchEditorForProject({}, &Error)) SetError(std::move(Error));
		}
	}

	auto MLevelEditor::DrawFileDialogs() -> void
	{
		switch (QueuedFilePopup)
		{
		case EQueuedFilePopup::UnsavedLevel: ImGui::OpenPopup("Unsaved Level"); break;
		case EQueuedFilePopup::NewLevel: ImGui::OpenPopup("New Level"); break;
		case EQueuedFilePopup::ImportStaticMesh: ImGui::OpenPopup("Import Static Mesh"); break;
		case EQueuedFilePopup::None: break;
		}
		QueuedFilePopup = EQueuedFilePopup::None;

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
			if (ImGui::Button("Cancel")) { PendingFileAction = EPendingFileAction::None; PendingLevelPath.clear(); ImGui::CloseCurrentPopup(); }
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


		ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Appearing);
		if (ImGui::BeginPopupModal("Import Static Mesh", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize))
		{
			ImGui::TextUnformatted("Create a static mesh asset from a model file.");
			ImGui::TextDisabled("The source model is copied next to the .dasset package so they can be moved together.");

			ImGui::Spacing();
			ImGui::SeparatorText("Source model");
			const float BrowseButtonWidth = 92.0f;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::InputTextWithHint("##ImportSource", "Choose an OBJ, FBX, glTF, or other supported model...", ImportSourcePathBuffer.data(), ImportSourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
			ImGui::SameLine();
			if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseStaticMeshSource();

			const std::filesystem::path SourcePath(ImportSourcePathBuffer.data());
			const bool bHasSource = ImportSourcePathBuffer[0] != '\0';
			const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
			if (bHasSource)
			{
				ImGui::TextDisabled("%s", std::format("{}  |  {}", SourcePath.extension().generic_string(), SourcePath.filename().generic_string()).c_str());
			}

			ImGui::Spacing();
			ImGui::SeparatorText("Destination");
			ImGui::TextUnformatted("Asset path");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::InputTextWithHint("##ImportAssetPath", "/Project/StaticMeshes/AssetName", ImportAssetPathBuffer.data(), ImportAssetPathBuffer.size());
			ImGui::SameLine();
			if (ImGui::Button("Choose...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseStaticMeshDestination();

			FAssetPath ParsedAssetPath;
			std::string AssetPathError;
			const bool bAssetPathValid = FAssetPath::TryCreate(ImportAssetPathBuffer.data(), ParsedAssetPath, &AssetPathError);
			bool bMountedDestination = false;
			if (bAssetPathValid)
			{
				for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
				{
					if (ParsedAssetPath.GetView().starts_with(Mount.VirtualRoot)) { bMountedDestination = true; break; }
				}
			}
			const bool bAssetExists = bAssetPathValid && (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath));

			if (bAssetPathValid && bMountedDestination && bHasSource)
			{
				const std::string SourceFileName = std::string(ParsedAssetPath.GetAssetName()) + SourcePath.extension().generic_string();
				ImGui::BeginChild("ImportOutputPreview", ImVec2(0.0f, 58.0f), ImGuiChildFlags_Borders);
				ImGui::TextDisabled("Files to create");
				ImGui::TextUnformatted(std::format("{}.dasset   +   {}", ParsedAssetPath.GetAssetName(), SourceFileName).c_str());
				ImGui::EndChild();
			}

			std::string ValidationMessage;
			if (!bHasSource) ValidationMessage = "Select a source model to continue.";
			else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
			else if (!bAssetPathValid) ValidationMessage = AssetPathError;
			else if (!bMountedDestination) ValidationMessage = "Choose a destination inside a mounted Content directory.";
			else if (bAssetExists) ValidationMessage = "An asset already exists at this path.";

			if (!ValidationMessage.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.25f, 1.0f));
				ImGui::TextWrapped("%s", ValidationMessage.c_str());
				ImGui::PopStyleColor();
			}

			ImGui::Spacing();
			ImGui::Separator();
			const bool bCanImport = ValidationMessage.empty();
			ImGui::BeginDisabled(!bCanImport);
			if (ImGui::Button("Import Static Mesh", ImVec2(150.0f, 0.0f)))
			{
				ImportStaticMesh();
				if (EditorError.empty()) ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (!EditorError.empty()) ImGui::OpenPopup("Editor Error");
		if (ImGui::BeginPopupModal("Editor Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", EditorError.c_str());
			if (ImGui::Button("OK")) { EditorError.clear(); ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}

	auto MLevelEditor::BrowseStaticMeshSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select a Static Mesh Source File";
		Request.Filters = {
			{"All Supported Models", "*.obj;*.fbx;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.stl"},
			{"Wavefront OBJ", "*.obj"},
			{"Autodesk FBX", "*.fbx"},
			{"glTF", "*.gltf;*.glb"},
			{"COLLADA", "*.dae"},
			{"All Files", "*.*"}
		};
		if (ImportSourcePathBuffer[0] != '\0')
		{
			Request.InitialDirectory = std::filesystem::path(ImportSourcePathBuffer.data()).parent_path().generic_string();
		}

		FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		if (Result.FilePath.size() >= ImportSourcePathBuffer.size())
		{
			SetError("The selected file path is too long for the import form.");
			return;
		}

		const std::string PreviousAssetPath = ImportAssetPathBuffer.data();
		ImportSourcePathBuffer.fill(0);
		std::memcpy(ImportSourcePathBuffer.data(), Result.FilePath.data(), FMath::Min(Result.FilePath.size(), ImportSourcePathBuffer.size() - 1));

		const std::string AssetName = String::SanitizeFileName(std::filesystem::path(Result.FilePath).stem().generic_string(), "StaticMesh");
		const FProjectInfo* Project = GetCurrentProject();
		const std::string SuggestedPath = (Project ? Project->MountRoot : "/") + "StaticMeshes/" + AssetName;
		if (PreviousAssetPath.empty() || PreviousAssetPath == LastSuggestedImportAssetPath)
		{
			ImportAssetPathBuffer.fill(0);
			std::memcpy(ImportAssetPathBuffer.data(), SuggestedPath.data(), FMath::Min(SuggestedPath.size(), ImportAssetPathBuffer.size() - 1));
		}
		LastSuggestedImportAssetPath = SuggestedPath;
	}

	auto MLevelEditor::BrowseStaticMeshDestination() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose a Static Mesh Asset Path";
		Request.Filters = {{"Durin Asset", "*.dasset"}};
		Request.DefaultFileName = ImportSourcePathBuffer[0] != '\0'
			? String::SanitizeFileName(std::filesystem::path(ImportSourcePathBuffer.data()).stem().generic_string(), "StaticMesh") + ".dasset"
			: "StaticMesh.dasset";

		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			if (const FProjectInfo* Project = GetCurrentProject(); Project && Mount.VirtualRoot == Project->MountRoot)
			{
				Request.InitialDirectory = Mount.PhysicalPath;
				break;
			}
		}

		FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}

		std::string SelectedPath = std::filesystem::absolute(Result.FilePath).lexically_normal().generic_string();
		std::ranges::transform(SelectedPath, SelectedPath.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			std::string MountPath = std::filesystem::absolute(Mount.PhysicalPath).lexically_normal().generic_string();
			if (!MountPath.ends_with('/')) MountPath += '/';
			std::string LowerMountPath = MountPath;
			std::ranges::transform(LowerMountPath, LowerMountPath.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (!SelectedPath.starts_with(LowerMountPath)) continue;

			std::filesystem::path RelativePath = std::filesystem::path(Result.FilePath).lexically_relative(std::filesystem::path(Mount.PhysicalPath));
			RelativePath.replace_extension();
			const std::string VirtualPath = Mount.VirtualRoot + RelativePath.generic_string();
			if (VirtualPath.size() >= ImportAssetPathBuffer.size())
			{
				SetError("The selected asset path is too long for the import form.");
				return;
			}
			ImportAssetPathBuffer.fill(0);
			std::memcpy(ImportAssetPathBuffer.data(), VirtualPath.data(), VirtualPath.size());
			LastSuggestedImportAssetPath.clear();
			return;
		}

		SetError("Static mesh assets must be saved inside a mounted Content directory.");
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
		ADirectionalLightActor* DirectionalLight = Level->SpawnActor<ADirectionalLightActor>("DirectionalLight");
		if (DirectionalLight != nullptr)
		{
			FTransform LightTransform = DirectionalLight->GetActorTransform();
			LightTransform.Rotation = FQuat(FVector3(glm::radians(-35.0), glm::radians(25.0), glm::radians(-20.0)));
			DirectionalLight->SetActorTransform(LightTransform);
		}
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
		CaptureCurrentViewportState();
		SaveSessionSettings();
		Asset::FAssetResult Result = Asset::SavePackage(Context->Level->GetPackage());
		if (!Result) { SetError(Result.Message); return false; }
		return true;
	}

	auto MLevelEditor::ImportStaticMesh() -> void
	{
		EditorError.clear();
		FStaticMeshImportResult Result = DStaticMesh::ImportAsset(ImportSourcePathBuffer.data(), ImportAssetPathBuffer.data());
		if (!Result) SetError(Result.Message);
	}

	auto MLevelEditor::ActivateLevel(DLevel* Level) -> bool
	{
		if (!Context || !Context->World || !Level) { SetError("No world is available to activate the level."); return false; }
		CaptureCurrentViewportState();
		SaveSessionSettings();
		DLevel* Previous = Context->World->GetCurrentLevel();
		DPackage* PreviousPackage = Previous ? Previous->GetPackage() : nullptr;
		if (!Context->World->SetCurrentLevel(Level)) { SetError("The level is already active in another world."); return false; }
		Context->Synchronize(Context->World);
		RestoreViewportState(Level);
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
		ImGui::DockBuilderDockWindow("File Browser###FileBrowser", BottomDockId);
		ImGui::DockBuilderDockWindow("Output Log###OutputLog", BottomDockId);
		ImGui::DockBuilderDockWindow("Scene Viewport###SceneViewport", MainDockId);
		ImGui::DockBuilderFinish(DockSpaceId);
	}
} // namespace Durin
