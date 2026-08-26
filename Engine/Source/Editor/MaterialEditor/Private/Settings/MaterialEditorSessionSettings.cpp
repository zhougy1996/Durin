#include "Settings/MaterialEditorSessionSettings.h"

#include "Misc/Paths.h"
#include "Yaml/Yaml.h"

namespace Durin::Editor::Material
{
	namespace
	{
		constexpr const char* SessionSettingsFileName = "MaterialEditorSession.yaml";
	}

	auto FMaterialEditorSessionSettings::Load() -> bool
	{
		Viewports.clear();
		const std::string FilePath = FPaths::LaunchConfigsDir() + SessionSettingsFileName;
		if (!std::filesystem::exists(FilePath)) return true;
		FYamlDocument Document;
		FYamlParseError Error;
		if (!Document.LoadFromFile(FilePath, &Error))
		{
			DURIN_WARN("Failed to load material editor session settings: {}", Error.Message);
			return false;
		}

		const FYamlNodeView Root = Document.GetRootView();
		const FYamlNodeView Layout = Root.GetView("Layout");
		LeftPaneRatio = static_cast<float>(std::clamp(
			Layout.GetView("LeftPaneRatio").GetDouble(LeftPaneRatio), 0.12, 0.40));
		RightPaneRatio = static_cast<float>(std::clamp(
			Layout.GetView("RightPaneRatio").GetDouble(RightPaneRatio), 0.16, 0.45));
		DiagnosticsRatio = static_cast<float>(std::clamp(
			Layout.GetView("DiagnosticsRatio").GetDouble(DiagnosticsRatio), 0.12, 0.55));
		bPreviewVisible = Layout.GetView("PreviewVisible").GetBool(true);
		bDetailsVisible = Layout.GetView("DetailsVisible").GetBool(true);
		bDiagnosticsVisible = Layout.GetView("DiagnosticsVisible").GetBool(false);

		const FYamlNodeView Entries = Root.GetView("GraphViewports");
		if (!Entries.IsSequence()) return true;
		for (size_t Index = 0; Index < Entries.Num(); ++Index)
		{
			const FYamlNodeView Entry = Entries.GetView(Index);
			const std::string ResourceId = Entry.GetView("Resource").GetString();
			const double Zoom = Entry.GetView("Zoom").GetDouble(1.0);
			const double PanX = Entry.GetView("PanX").GetDouble(40.0);
			const double PanY = Entry.GetView("PanY").GetDouble(40.0);
			if (ResourceId.empty() || !std::isfinite(Zoom)
				|| !std::isfinite(PanX) || !std::isfinite(PanY)) continue;
			Viewports[ResourceId] = {
				.Zoom = static_cast<float>(std::clamp(Zoom, 0.25, 2.0)),
				.Pan = {static_cast<float>(PanX), static_cast<float>(PanY)},
			};
		}
		return true;
	}

	auto FMaterialEditorSessionSettings::Save() const -> bool
	{
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		FYamlNodeRef Layout = Root.AddMap("Layout");
		Layout.SetChildValue("LeftPaneRatio", static_cast<double>(LeftPaneRatio));
		Layout.SetChildValue("RightPaneRatio", static_cast<double>(RightPaneRatio));
		Layout.SetChildValue("DiagnosticsRatio", static_cast<double>(DiagnosticsRatio));
		Layout.SetChildValue("PreviewVisible", bPreviewVisible);
		Layout.SetChildValue("DetailsVisible", bDetailsVisible);
		Layout.SetChildValue("DiagnosticsVisible", bDiagnosticsVisible);

		FYamlNodeRef Entries = Root.AddSequence("GraphViewports");
		std::vector<std::string> ResourceIds;
		ResourceIds.reserve(Viewports.size());
		for (const auto& [ResourceId, State] : Viewports)
			ResourceIds.push_back(ResourceId);
		std::ranges::sort(ResourceIds);
		for (const std::string& ResourceId : ResourceIds)
		{
			const FMaterialGraphViewportState& State = Viewports.at(ResourceId);
			FYamlNodeRef Entry = Entries.AppendMap();
			Entry.SetChildValue("Resource", ResourceId);
			Entry.SetChildValue("Zoom", static_cast<double>(State.Zoom));
			Entry.SetChildValue("PanX", static_cast<double>(State.Pan.x));
			Entry.SetChildValue("PanY", static_cast<double>(State.Pan.y));
		}
		if (!Document.SaveToFile(FPaths::LaunchConfigsDir() + SessionSettingsFileName))
		{
			DURIN_WARN("Failed to save material editor session settings.");
			return false;
		}
		return true;
	}

	auto FMaterialEditorSessionSettings::FindViewport(std::string_view ResourceId) const
		-> const FMaterialGraphViewportState*
	{
		const auto It = Viewports.find(std::string(ResourceId));
		return It == Viewports.end() ? nullptr : &It->second;
	}

	auto FMaterialEditorSessionSettings::SetViewport(
		std::string_view ResourceId,
		const FMaterialGraphViewportState& State) -> void
	{
		if (!ResourceId.empty()) Viewports[std::string(ResourceId)] = State;
	}

	auto FMaterialEditorSessionSettings::MoveViewport(
		std::string_view OldResourceId,
		std::string_view NewResourceId) -> void
	{
		const auto It = Viewports.find(std::string(OldResourceId));
		if (It == Viewports.end()) return;
		Viewports[std::string(NewResourceId)] = It->second;
		Viewports.erase(It);
	}
}
