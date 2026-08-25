#include "Settings/LevelEditorSessionSettings.h"

#include "AssetAuthoring.h"
#include "DObject/Package.h"
#include "Engine/Level.h"
#include "Workspace/LevelEditorContext.h"
#include "Misc/FilesystemMigration.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Panels/SceneViewportPanel.h"
#include "Yaml/Yaml.h"

namespace Durin::Editor::Level
{
	// Session persistence stays independent from the editor widget's menu and popup orchestration.
	namespace
	{
		constexpr const char* SessionSettingsFileName = "LevelEditorSession.yaml";
	}

	auto FLevelEditorSessionSettings::Load() -> bool
	{
		FYamlDocument Document;
		FYamlParseError Error;
		const std::string FilePath = FPaths::LaunchConfigsDir() + SessionSettingsFileName;
		std::string MigrationWarning;
		if (!MigrateLegacyFileIfMissing(
				std::filesystem::path(FPaths::LaunchDir()) / SessionSettingsFileName,
				FilePath,
				&MigrationWarning))
			DURIN_WARN("{}", MigrationWarning);
		if (!std::filesystem::exists(FilePath)) return true;
		if (!Document.LoadFromFile(FilePath, &Error))
		{
			DURIN_WARN("Failed to load level editor session settings: {}", Error.Message);
			return false;
		}

		const FYamlNodeView Root = Document.GetRootView();
		LoadLevelViewportStates(Root, ViewportStates);

		const FYamlNodeView Gizmo = Root.GetView("TransformGizmo");
		GizmoMode = static_cast<uint8>(std::clamp<int64>(Gizmo.GetView("Mode").GetInt(0), 0, 2));
		GizmoSpace = static_cast<uint8>(std::clamp<int64>(Gizmo.GetView("Space").GetInt(0), 0, 2));
		bGizmoSnapEnabled = Gizmo.GetView("SnapEnabled").GetBool(false);
		GizmoTranslationSnap = static_cast<float>(Gizmo.GetView("TranslationSnap").GetDouble(0.5));
		GizmoRotationSnap = static_cast<float>(Gizmo.GetView("RotationSnap").GetDouble(15.0));
		GizmoScaleSnap = static_cast<float>(Gizmo.GetView("ScaleSnap").GetDouble(0.1));
		const FYamlNodeView SceneViewport = Root.GetView("SceneViewport");
		bShowWorldGrid = SceneViewport.GetView("ShowWorldGrid").GetBool(true);
		bShowViewportStatistics = SceneViewport.GetView("ShowStatistics").GetBool(false);
		const double LoadedCameraMovementSpeed = SceneViewport.GetView("CameraMovementSpeed").GetDouble(5.0);
		CameraMovementSpeed = std::isfinite(LoadedCameraMovementSpeed)
			? static_cast<float>(std::clamp(LoadedCameraMovementSpeed, 0.05, 10000.0))
			: 5.0f;

		const FYamlNodeView ContentBrowser = Root.GetView("ContentBrowser");
		ContentBrowserViewMode = static_cast<uint8>(std::clamp<int64>(ContentBrowser.GetView("ViewMode").GetInt(0), 0, 1));
		ContentBrowserIconSize = static_cast<float>(std::clamp(ContentBrowser.GetView("IconSize").GetDouble(DefaultContentBrowserIconSize), static_cast<double>(MinimumContentBrowserIconSize), static_cast<double>(MaximumContentBrowserIconSize)));
		bContentBrowserIconSizeLocked = ContentBrowser.GetView("IconSizeLocked").GetBool(false);
		ContentBrowserTreeWidth = static_cast<float>(std::clamp(ContentBrowser.GetView("TreeWidth").GetDouble(DefaultContentBrowserTreeRatio), static_cast<double>(MinimumContentBrowserTreeRatio), static_cast<double>(MaximumContentBrowserTreeRatio)));
		bContentBrowserShowHiddenFiles = ContentBrowser.GetView("ShowHiddenFiles").GetBool(false);
		ContentBrowserLastDirectory = ContentBrowser.GetView("LastDirectory").GetString();
		const FYamlNodeView Details = Root.GetView("Details");
		DetailsPaneRatio = static_cast<float>(std::clamp(Details.GetView("ComponentPaneRatio").GetDouble(DefaultDetailsPaneRatio), static_cast<double>(MinimumDetailsPaneRatio), static_cast<double>(MaximumDetailsPaneRatio)));
		bDetailsPaneAutoSized = Details.GetView("ComponentPaneAutoSized").GetBool(true);
		return true;
	}

	auto FLevelEditorSessionSettings::PruneInvalidViewportStates() -> void
	{
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const auto ProjectStates = ViewportStates.find(Project->ProjectFile);
			if (ProjectStates != ViewportStates.end())
			{
				std::unordered_map<std::string, FLevelViewportCameraState>
					NormalizedStates;
				for (const auto& [StoredPath, State] : ProjectStates->second)
				{
					FAssetPath Path;
					if (!FAssetPath::TryCreate(StoredPath, Path)) continue;
					const Asset::FAssetPathResolveResult Resolution =
						Asset::ResolveAssetPath(
							Path, {.ExpectedClass = DLevel::StaticClass()});
					if (!Resolution) continue;
					const std::string FinalPath = Resolution.FinalPath.ToString();
					const bool bExact = Resolution.RedirectChain.empty();
					if (bExact || !NormalizedStates.contains(FinalPath))
						NormalizedStates.insert_or_assign(FinalPath, State);
				}
				ProjectStates->second = std::move(NormalizedStates);
				if (ProjectStates->second.empty()) ViewportStates.erase(ProjectStates);
			}
		}
	}

	auto FLevelEditorSessionSettings::Save(const FSceneViewportPanel* SceneViewportPanel) const -> bool
	{
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		if (const FProjectInfo* Project = GetCurrentProject()) Root.SetChildValue("RecentProject", Project->ProjectFile);
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
		FYamlNodeRef SceneViewportNode = Root.AddMap("SceneViewport");
		SceneViewportNode.SetChildValue("ShowWorldGrid", SceneViewportPanel ? SceneViewportPanel->IsGridVisible() : bShowWorldGrid);
		SceneViewportNode.SetChildValue("ShowStatistics", SceneViewportPanel
			? SceneViewportPanel->IsStatisticsVisible() : bShowViewportStatistics);
		SceneViewportNode.SetChildValue("CameraMovementSpeed", static_cast<double>(SceneViewportPanel ? SceneViewportPanel->GetCameraMovementSpeed() : CameraMovementSpeed));

		FYamlNodeRef ContentBrowserNode = Root.AddMap("ContentBrowser");
		ContentBrowserNode.SetChildValue("ViewMode", static_cast<int64>(ContentBrowserViewMode));
		ContentBrowserNode.SetChildValue("IconSize", static_cast<double>(ContentBrowserIconSize));
		ContentBrowserNode.SetChildValue("IconSizeLocked", bContentBrowserIconSizeLocked);
		ContentBrowserNode.SetChildValue("TreeWidth", static_cast<double>(ContentBrowserTreeWidth));
		ContentBrowserNode.SetChildValue("ShowHiddenFiles", bContentBrowserShowHiddenFiles);
		ContentBrowserNode.SetChildValue("LastDirectory", ContentBrowserLastDirectory);
		FYamlNodeRef DetailsNode = Root.AddMap("Details");
		DetailsNode.SetChildValue("ComponentPaneRatio", static_cast<double>(DetailsPaneRatio));
		DetailsNode.SetChildValue("ComponentPaneAutoSized", bDetailsPaneAutoSized);

		SaveLevelViewportStates(Root, ViewportStates);
		if (!Document.SaveToFile(FPaths::LaunchConfigsDir() + SessionSettingsFileName))
		{
			DURIN_WARN("Failed to save level editor session settings.");
			return false;
		}
		return true;
	}

	auto FLevelEditorSessionSettings::ApplyTo(FSceneViewportPanel& SceneViewportPanel) const -> void
	{
		if (FTransformGizmo* Gizmo = SceneViewportPanel.GetTransformGizmo())
		{
			Gizmo->SetMode(static_cast<ETransformGizmoMode>(std::min<uint8>(GizmoMode, 2)));
			Gizmo->SetSpace(static_cast<ETransformGizmoSpace>(std::min<uint8>(GizmoSpace, 2)));
			Gizmo->GetSnapSettings() = {bGizmoSnapEnabled, GizmoTranslationSnap, GizmoRotationSnap, GizmoScaleSnap};
		}
		SceneViewportPanel.SetGridVisible(bShowWorldGrid);
		SceneViewportPanel.SetStatisticsVisible(bShowViewportStatistics);
		SceneViewportPanel.SetCameraMovementSpeed(CameraMovementSpeed);
	}

	auto FLevelEditorSessionSettings::CaptureViewportState(const FLevelEditorContext& Context, const FSceneViewportPanel& SceneViewportPanel) -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || !Context.Level) return;
		DPackage* Package = Context.Level->GetPackage();
		if (!Package) return;
		FLevelViewportCameraState State;
		if (SceneViewportPanel.CaptureCameraState(Context.Level, State))
			ViewportStates[Project->ProjectFile][Package->GetPackagePath()] = State;
	}

	auto FLevelEditorSessionSettings::RestoreViewportState(DLevel* Level, FSceneViewportPanel& SceneViewportPanel) const -> void
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

	auto FLevelEditorSessionSettings::MoveViewportState(std::string_view OldPath, std::string_view NewPath) -> void
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

	auto FLevelEditorSessionSettings::SetContentBrowserState(uint8 ViewMode, float IconSize, bool bIconSizeLocked, float TreeWidth, bool bShowHiddenFiles, std::string LastDirectory) -> void
	{
		ContentBrowserViewMode = std::min<uint8>(ViewMode, 1);
		ContentBrowserIconSize = std::clamp(IconSize, MinimumContentBrowserIconSize, MaximumContentBrowserIconSize);
		bContentBrowserIconSizeLocked = bIconSizeLocked;
		ContentBrowserTreeWidth = std::clamp(TreeWidth, MinimumContentBrowserTreeRatio, MaximumContentBrowserTreeRatio);
		bContentBrowserShowHiddenFiles = bShowHiddenFiles;
		ContentBrowserLastDirectory = std::move(LastDirectory);
	}

	auto FLevelEditorSessionSettings::SetDetailsPaneRatio(float Ratio) -> void
	{
		DetailsPaneRatio = std::clamp(Ratio, MinimumDetailsPaneRatio, MaximumDetailsPaneRatio);
	}
} // namespace Durin::Editor::Level
