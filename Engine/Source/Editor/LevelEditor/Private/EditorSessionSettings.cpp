#include "EditorSessionSettings.h"

#include "AssetSystem.h"
#include "Application/GenericApplication.h"
#include "Engine/Level.h"
#include "LevelEditorContext.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "MonaImGui.h"
#include "Panels/SceneViewportPanel.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	// Session persistence stays independent from the editor widget's menu and popup orchestration.
	namespace
	{
		constexpr const char* SessionSettingsFileName = "LevelEditorSession.yaml";
	}

	auto FEditorSessionSettings::Load() -> bool
	{
		MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Dark);
		const std::vector<FMonitorInfo> Monitors = EnumerateMonitors();
		if (!Monitors.empty())
		{
			WindowWidth = std::min(1600, static_cast<int32>(Monitors.front().WorkSize.x * 0.9f));
			WindowHeight = std::min(1000, static_cast<int32>(Monitors.front().WorkSize.y * 0.9f));
			UIScale = Monitors.front().WorkSize.y >= 1800 ? 1.5f : Monitors.front().WorkSize.y >= 1300 ? 1.25f :
																										 1.0f;
		}

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
		LoadLevelViewportStates(Root, ViewportStates);

		const FYamlNodeView Display = Root.GetView("Display");
		WindowWidth = static_cast<int32>(Display.GetView("WindowWidth").GetInt(WindowWidth));
		WindowHeight = static_cast<int32>(Display.GetView("WindowHeight").GetInt(WindowHeight));
		UIScale = static_cast<float>(Display.GetView("UIScale").GetDouble(UIScale));
		MonaImGui::SetColorTheme(Display.GetView("ColorTheme").GetString("Dark") == "Light" ? MonaImGui::EColorTheme::Light : MonaImGui::EColorTheme::Dark);
		bWindowMaximized = Display.GetView("WindowMaximized").GetBool(true);

		const FYamlNodeView Gizmo = Root.GetView("TransformGizmo");
		GizmoMode = static_cast<uint8>(std::clamp<int64>(Gizmo.GetView("Mode").GetInt(0), 0, 2));
		GizmoSpace = static_cast<uint8>(std::clamp<int64>(Gizmo.GetView("Space").GetInt(0), 0, 1));
		bGizmoSnapEnabled = Gizmo.GetView("SnapEnabled").GetBool(false);
		GizmoTranslationSnap = static_cast<float>(Gizmo.GetView("TranslationSnap").GetDouble(0.5));
		GizmoRotationSnap = static_cast<float>(Gizmo.GetView("RotationSnap").GetDouble(15.0));
		GizmoScaleSnap = static_cast<float>(Gizmo.GetView("ScaleSnap").GetDouble(0.1));

		const FYamlNodeView ContentBrowser = Root.GetView("ContentBrowser");
		ContentBrowserViewMode = static_cast<uint8>(std::clamp<int64>(ContentBrowser.GetView("ViewMode").GetInt(0), 0, 1));
		ContentBrowserIconSize = static_cast<float>(std::clamp(ContentBrowser.GetView("IconSize").GetDouble(88.0), 56.0, 160.0));
		bContentBrowserIconSizeLocked = ContentBrowser.GetView("IconSizeLocked").GetBool(false);
		ContentBrowserTreeWidth = static_cast<float>(std::clamp(ContentBrowser.GetView("TreeWidth").GetDouble(0.24), 0.15, 0.55));
		bContentBrowserShowSourceFiles = ContentBrowser.GetView("ShowSourceFiles").GetBool(false);
		ContentBrowserLastDirectory = ContentBrowser.GetView("LastDirectory").GetString();
		return true;
	}

	auto FEditorSessionSettings::PruneInvalidViewportStates() -> void
	{
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const auto ProjectStates = ViewportStates.find(Project->ProjectFile);
			if (ProjectStates != ViewportStates.end())
			{
				std::erase_if(ProjectStates->second, [](const auto& Entry) {
					FAssetPath Path;
					if (!FAssetPath::TryCreate(Entry.first, Path)) return true;
					const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(Path);
					return !Data || Data->AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString();
				});
				if (ProjectStates->second.empty()) ViewportStates.erase(ProjectStates);
			}
		}
	}

	auto FEditorSessionSettings::Save(const FSceneViewportPanel* SceneViewportPanel) const -> bool
	{
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		if (const FProjectInfo* Project = GetCurrentProject()) Root.SetChildValue("RecentProject", Project->ProjectFile);
		FYamlNodeRef Display = Root.AddMap("Display");
		Display.SetChildValue("WindowWidth", WindowWidth);
		Display.SetChildValue("WindowHeight", WindowHeight);
		Display.SetChildValue("UIScale", static_cast<double>(UIScale));
		Display.SetChildValue("ColorTheme", MonaImGui::GetColorTheme() == MonaImGui::EColorTheme::Light ? "Light" : "Dark");
		Display.SetChildValue("WindowMaximized", bWindowMaximized);

		FYamlNodeRef GizmoNode = Root.AddMap("TransformGizmo");
		if (SceneViewportPanel)
		{
			if (const FTransformGizmo* Gizmo = SceneViewportPanel->GetTransformGizmo())
			{
				const FTransformGizmoSnapSettings& Settings = Gizmo->GetSnapSettings();
				GizmoNode.SetChildValue("Mode", static_cast<int64>(Gizmo->GetMode()));
				GizmoNode.SetChildValue("Space", static_cast<int64>(Gizmo->GetSpace()));
				GizmoNode.SetChildValue("SnapEnabled", Settings.bEnabled);
				GizmoNode.SetChildValue("TranslationSnap", static_cast<double>(Settings.Translation));
				GizmoNode.SetChildValue("RotationSnap", static_cast<double>(Settings.RotationDegrees));
				GizmoNode.SetChildValue("ScaleSnap", static_cast<double>(Settings.Scale));
			}
		}
		else
		{
			GizmoNode.SetChildValue("Mode", static_cast<int64>(GizmoMode));
			GizmoNode.SetChildValue("Space", static_cast<int64>(GizmoSpace));
			GizmoNode.SetChildValue("SnapEnabled", bGizmoSnapEnabled);
			GizmoNode.SetChildValue("TranslationSnap", static_cast<double>(GizmoTranslationSnap));
			GizmoNode.SetChildValue("RotationSnap", static_cast<double>(GizmoRotationSnap));
			GizmoNode.SetChildValue("ScaleSnap", static_cast<double>(GizmoScaleSnap));
		}

		FYamlNodeRef ContentBrowserNode = Root.AddMap("ContentBrowser");
		ContentBrowserNode.SetChildValue("ViewMode", static_cast<int64>(ContentBrowserViewMode));
		ContentBrowserNode.SetChildValue("IconSize", static_cast<double>(ContentBrowserIconSize));
		ContentBrowserNode.SetChildValue("IconSizeLocked", bContentBrowserIconSizeLocked);
		ContentBrowserNode.SetChildValue("TreeWidth", static_cast<double>(ContentBrowserTreeWidth));
		ContentBrowserNode.SetChildValue("ShowSourceFiles", bContentBrowserShowSourceFiles);
		ContentBrowserNode.SetChildValue("LastDirectory", ContentBrowserLastDirectory);

		SaveLevelViewportStates(Root, ViewportStates);
		if (!Document.SaveToFile(FPaths::LaunchDir() + SessionSettingsFileName))
		{
			DURIN_WARN("Failed to save level editor session settings.");
			return false;
		}
		return true;
	}

	auto FEditorSessionSettings::ApplyTo(FSceneViewportPanel& SceneViewportPanel) const -> void
	{
		if (FTransformGizmo* Gizmo = SceneViewportPanel.GetTransformGizmo())
		{
			Gizmo->SetMode(static_cast<ETransformGizmoMode>(std::min<uint8>(GizmoMode, 2)));
			Gizmo->SetSpace(static_cast<ETransformGizmoSpace>(std::min<uint8>(GizmoSpace, 1)));
			Gizmo->GetSnapSettings() = {bGizmoSnapEnabled, GizmoTranslationSnap, GizmoRotationSnap, GizmoScaleSnap};
		}
	}

	auto FEditorSessionSettings::CaptureViewportState(const FLevelEditorContext& Context, const FSceneViewportPanel& SceneViewportPanel) -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || !Context.Level) return;
		DPackage* Package = Context.Level->GetPackage();
		if (!Package) return;
		FLevelViewportCameraState State;
		if (SceneViewportPanel.CaptureCameraState(Context.Level, State))
			ViewportStates[Project->ProjectFile][Package->GetPackagePath()] = State;
	}

	auto FEditorSessionSettings::RestoreViewportState(DLevel* Level, FSceneViewportPanel& SceneViewportPanel) const -> void
	{
		if (!Level) return;
		const FProjectInfo* Project = GetCurrentProject();
		DPackage* Package = Level->GetPackage();
		const FLevelViewportCameraState* State = nullptr;
		if (Project && Package)
		{
			const auto ProjectIt = ViewportStates.find(Project->ProjectFile);
			if (ProjectIt != ViewportStates.end())
			{
				const auto LevelIt = ProjectIt->second.find(Package->GetPackagePath());
				if (LevelIt != ProjectIt->second.end()) State = &LevelIt->second;
			}
		}
		SceneViewportPanel.RestoreCameraState(Level, State);
	}

	auto FEditorSessionSettings::MoveViewportState(std::string_view OldPath, std::string_view NewPath) -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project) return;
		auto ProjectIt = ViewportStates.find(Project->ProjectFile);
		if (ProjectIt == ViewportStates.end()) return;
		auto StateIt = ProjectIt->second.find(std::string(OldPath));
		if (StateIt == ProjectIt->second.end()) return;
		FLevelViewportCameraState State = StateIt->second;
		ProjectIt->second.erase(StateIt);
		ProjectIt->second[std::string(NewPath)] = State;
	}

	auto FEditorSessionSettings::SetDisplaySettings(int32 Width, int32 Height, float Scale) -> void
	{
		WindowWidth = Width;
		WindowHeight = Height;
		UIScale = Scale;
	}

	auto FEditorSessionSettings::SetContentBrowserState(uint8 ViewMode, float IconSize, bool bIconSizeLocked, float TreeWidth, bool bShowSourceFiles, std::string LastDirectory) -> void
	{
		ContentBrowserViewMode = std::min<uint8>(ViewMode, 1);
		ContentBrowserIconSize = std::clamp(IconSize, 56.0f, 160.0f);
		bContentBrowserIconSizeLocked = bIconSizeLocked;
		ContentBrowserTreeWidth = std::clamp(TreeWidth, 0.15f, 0.55f);
		bContentBrowserShowSourceFiles = bShowSourceFiles;
		ContentBrowserLastDirectory = std::move(LastDirectory);
	}
} // namespace Durin
