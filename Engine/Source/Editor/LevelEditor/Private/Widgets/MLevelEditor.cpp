#include "Widgets/MLevelEditor.h"

#include "AssetSystem.h"
#include "Dialogs/FileDialog.h"
#include "Actors/CameraActor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "LevelEditorContext.h"
#include "Misc/StringConvert.h"
#include "Misc/Paths.h"
#include "MonaImGui.h"
#include "Application/GenericApplication.h"
#include "Application/MonaApplication.h"
#include "Widgets/MWindow.h"
#include "Panels/DetailsPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/OutputLogPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/WorldOutlinerPanel.h"
#include "StaticMesh/StaticMesh.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	namespace
	{
		constexpr const char* DockSpaceName = "DurinEditorDockSpace";
		constexpr const char* SessionSettingsFileName = "LevelEditorSession.yaml";

		auto GetMountRoot(std::string_view Path) -> std::string
		{
			if (Path.empty() || Path.front() != '/') return {};
			const size_t Separator = Path.find('/', 1);
			return Separator == std::string_view::npos ? std::string() : std::string(Path.substr(0, Separator + 1));
		}
	}

	MLevelEditor::MLevelEditor() = default;
	MLevelEditor::~MLevelEditor() = default;

	auto MLevelEditor::Construct() -> void
	{
		Context = std::make_unique<FLevelEditorContext>();
		Context->ReportError = [this](std::string Message) { SetError(std::move(Message)); };
		Asset::GetAssetRegistry().ScanMountedContent();
		LoadSessionSettings();
		Panels.emplace_back(std::make_unique<FSceneViewportPanel>());
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		Panels.emplace_back(std::make_unique<FDetailsPanel>());
		Panels.emplace_back(std::make_unique<FOutputLogPanel>());
		Context->Synchronize(GEngine != nullptr ? GEngine->GetWorld() : nullptr);
		InitializeStartupLevel();
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
		bAlwaysAskForStartupLevel = Root.GetView("AlwaysAskForStartupLevel").GetBool(false);
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
		const FYamlNodeView RecentLevels = Root.GetView("RecentLevels");
		if (RecentLevels.IsMap())
		{
			for (size_t Index = 0; Index < RecentLevels.Num(); ++Index)
			{
				const FYamlNodeView Entry = RecentLevels.GetView(Index);
				const std::string Path = Entry.GetString();
				if (!Entry.GetKey().empty() && !Path.empty()) RecentLevelByMount[Entry.GetKey()] = Path;
			}
		}
		return true;
	}

	auto MLevelEditor::SaveSessionSettings() const -> bool
	{
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		Root.SetChildValue("AlwaysAskForStartupLevel", bAlwaysAskForStartupLevel);
		FYamlNodeRef Display = Root.AddMap("Display");
		Display.SetChildValue("WindowWidth", WindowWidth);
		Display.SetChildValue("WindowHeight", WindowHeight);
		Display.SetChildValue("UIScale", static_cast<double>(UIScale));
		FYamlNodeRef RecentLevels = Root.AddMap("RecentLevels");
		for (const auto& [MountRoot, Path] : RecentLevelByMount) RecentLevels.SetChildValue(MountRoot, Path);
		if (!Document.SaveToFile(FPaths::LaunchDir() + SessionSettingsFileName))
		{
			DURIN_WARN("Failed to save level editor session settings.");
			return false;
		}
		return true;
	}

	auto MLevelEditor::GetStartupMountRoot() const -> std::string
	{
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			if (Mount.VirtualRoot != "/Engine/") return Mount.VirtualRoot;
		}
		return {};
	}

	auto MLevelEditor::InitializeStartupLevel() -> void
	{
		if (!bAlwaysAskForStartupLevel)
		{
			const auto Found = RecentLevelByMount.find(GetStartupMountRoot());
			if (Found != RecentLevelByMount.end())
			{
				OpenLevel(Found->second);
				if (EditorError.empty()) return;
				DURIN_WARN("Could not restore startup level {}: {}", Found->second, EditorError);
				EditorError.clear();
			}
		}
		QueuedFilePopup = EQueuedFilePopup::StartupLevel;
	}

	auto MLevelEditor::RecordRecentLevel(std::string_view Path) -> void
	{
		const std::string MountRoot = GetMountRoot(Path);
		if (MountRoot.empty()) return;
		RecentLevelByMount[MountRoot] = Path;
		SaveSessionSettings();
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
			if (ImGui::MenuItem("Open Startup Level...")) RequestFileAction(EPendingFileAction::StartupLevel);
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
			ImGui::MenuItem("Undo/redo is not available yet", nullptr, false, false);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::BeginMenu("Display"))
			{
				const std::vector<FMonitorInfo> Monitors = EnumerateMonitors();
				FIntPoint Recommended{1280, 800};
				float RecommendedScale = 1.0f;
				if (!Monitors.empty())
				{
					Recommended = {std::min(1600, static_cast<int32>(Monitors.front().WorkSize.x * 0.9f)), std::min(1000, static_cast<int32>(Monitors.front().WorkSize.y * 0.9f))};
					RecommendedScale = Monitors.front().WorkSize.y >= 1800 ? 1.5f : Monitors.front().WorkSize.y >= 1300 ? 1.25f : 1.0f;
				}
				if (ImGui::MenuItem("Recommended")) ApplyDisplaySettings(Recommended.x, Recommended.y, RecommendedScale);
				ImGui::SeparatorText("Window Size");
				if (ImGui::MenuItem("1280 x 800")) ApplyDisplaySettings(1280, 800, UIScale);
				if (ImGui::MenuItem("1600 x 900")) ApplyDisplaySettings(1600, 900, UIScale);
				if (ImGui::MenuItem("1920 x 1080")) ApplyDisplaySettings(1920, 1080, UIScale);
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

	auto MLevelEditor::ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void
	{
		WindowWidth = Width;
		WindowHeight = Height;
		UIScale = Scale;
		MonaImGui::SetGlobalUIScale(UIScale);
		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow())
		{
			Window->ResizeWindow({static_cast<float>(WindowWidth), static_cast<float>(WindowHeight)});
		}
		SaveSessionSettings();
	}

	auto MLevelEditor::RequestFileAction(EPendingFileAction Action) -> void
	{
		PendingFileAction = Action;
		if (Context && Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty())
		{
			QueuedFilePopup = EQueuedFilePopup::UnsavedLevel;
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
			QueuedFilePopup = EQueuedFilePopup::NewLevel;
		}
		else if (PendingFileAction == EPendingFileAction::OpenLevel)
		{
			OpenFilterBuffer.fill(0);
			QueuedFilePopup = EQueuedFilePopup::OpenLevel;
		}
		else if (PendingFileAction == EPendingFileAction::StartupLevel)
		{
			OpenFilterBuffer.fill(0);
			QueuedFilePopup = EQueuedFilePopup::StartupLevel;
		}
	}

	auto MLevelEditor::DrawFileDialogs() -> void
	{
		switch (QueuedFilePopup)
		{
		case EQueuedFilePopup::UnsavedLevel: ImGui::OpenPopup("Unsaved Level"); break;
		case EQueuedFilePopup::NewLevel: ImGui::OpenPopup("New Level"); break;
		case EQueuedFilePopup::OpenLevel: ImGui::OpenPopup("Open Level"); break;
		case EQueuedFilePopup::StartupLevel: ImGui::OpenPopup("Choose Startup Level"); break;
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
			DrawLevelAssetList(false);
			ImGui::EndChild();
			if (ImGui::Button("Cancel")) { PendingFileAction = EPendingFileAction::None; ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Choose Startup Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Open a level, create one, or continue with an empty editor.");
			ImGui::InputTextWithHint("##StartupLevelFilter", "Filter virtual paths...", OpenFilterBuffer.data(), OpenFilterBuffer.size());
			ImGui::BeginChild("StartupLevelAssets", ImVec2(520.0f, 280.0f), true);
			const bool bOpened = DrawLevelAssetList(true);
			ImGui::EndChild();
			if (bOpened) ImGui::CloseCurrentPopup();
			if (ImGui::Button("New Level"))
			{
				ImGui::CloseCurrentPopup();
				PendingFileAction = EPendingFileAction::NewLevel;
				ExecutePendingFileAction();
			}
			ImGui::SameLine();
			if (ImGui::Button("Continue Empty")) ImGui::CloseCurrentPopup();
			if (ImGui::Checkbox("Always ask when the editor starts", &bAlwaysAskForStartupLevel)) SaveSessionSettings();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Import Static Mesh", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputText("Source File", ImportSourcePathBuffer.data(), ImportSourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
			ImGui::SameLine();
			if (ImGui::Button("Browse...")) BrowseStaticMeshSource();
			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputText("Asset Path", ImportAssetPathBuffer.data(), ImportAssetPathBuffer.size());
			ImGui::SameLine();
			if (ImGui::Button("Browse...##AssetPath")) BrowseStaticMeshDestination();
			if (ImGui::Button("Import"))
			{
				ImportStaticMesh();
				if (EditorError.empty()) ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
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

	auto MLevelEditor::DrawLevelAssetList(bool bStartupPicker) -> bool
	{
		(void)bStartupPicker;
		const std::string Filter = OpenFilterBuffer.data();
		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			if (Data.AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString()) continue;
			const std::string PathString = Path.ToString();
			if (!Filter.empty() && PathString.find(Filter) == std::string::npos) continue;
			if (ImGui::Selectable(PathString.c_str()))
			{
				OpenLevel(PathString);
				if (EditorError.empty()) return true;
			}
		}
		return false;
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
		const std::string SuggestedPath = "/SandBox/StaticMeshes/" + AssetName;
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
			if (Mount.VirtualRoot == "/SandBox/")
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

	auto MLevelEditor::ImportStaticMesh() -> void
	{
		EditorError.clear();
		FStaticMeshImportResult Result = DStaticMesh::ImportAsset(ImportSourcePathBuffer.data(), ImportAssetPathBuffer.data());
		if (!Result) SetError(Result.Message);
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
		if (DPackage* Package = Level->GetPackage()) RecordRecentLevel(Package->GetPackagePath());
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
