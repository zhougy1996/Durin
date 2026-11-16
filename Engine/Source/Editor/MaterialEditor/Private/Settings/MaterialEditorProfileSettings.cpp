#include "Settings/MaterialEditorProfileSettings.h"

#include "Misc/Paths.h"
#include "Yaml/Yaml.h"
#include "Misc/FileHelper.h"

namespace Durin::MaterialEditor
{
	auto FMaterialEditorProfileSettings::SelectStorage(std::string InFileName) -> bool
	{
		*this = FMaterialEditorProfileSettings(std::move(InFileName));
		return Load();
	}
	auto FMaterialEditorProfileSettings::Load() -> bool
	{
		Viewports.clear();
		DirtyViewports.clear();
		const std::string FilePath = FPaths::LaunchConfigsDir() + FileName;
		if (!std::filesystem::exists(FilePath)) return true;
		FYamlDocument Document;
		if (const auto Loaded = Document.LoadFromFile(FilePath); !Loaded)
		{
			DURIN_WARN("Failed to load material editor profile settings: {}", Loaded.error().ToString());
			return false;
		}

		const FYamlNodeView Root = Document.GetRootView();
		bAutoCompile = Root.GetView("AutoCompile").GetBool(true);
		const FYamlNodeView Layout = Root.GetView("Layout");
		bPreviewVisible = Layout.GetView("PreviewVisible").GetBool(true);
		bDetailsVisible = Layout.GetView("DetailsVisible").GetBool(true);
		bParametersVisible = Layout.GetView("ParametersVisible").GetBool(true);
		bDiagnosticsVisible = Layout.GetView("DiagnosticsVisible").GetBool(false);

		SavedPreferences = Preferences();
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

	auto FMaterialEditorProfileSettings::Save(bool bReplacePreferences) const -> bool
	{
		FMaterialEditorProfileSettings Stored(FileName);
		if (!Stored.Load()) return false;
		const bool Missing = !std::filesystem::exists(FPaths::LaunchConfigsDir() + FileName);
		const auto Current = Preferences();
		const std::array<bool*, 5> Fields{&Stored.bAutoCompile, &Stored.bPreviewVisible,
			&Stored.bDetailsVisible, &Stored.bParametersVisible, &Stored.bDiagnosticsVisible};
		for (size_t I = 0; I < Fields.size(); ++I)
			if (Missing || bReplacePreferences || Current[I] != SavedPreferences[I]) *Fields[I] = Current[I];
		for (const auto& Resource : DirtyViewports)
			if (auto It = Viewports.find(Resource); It != Viewports.end()) Stored.Viewports[Resource] = It->second;
			else Stored.Viewports.erase(Resource);
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		Root.SetChildValue("Version", 3);
		Root.SetChildValue("AutoCompile", Stored.bAutoCompile);
		FYamlNodeRef Layout = Root.AddMap("Layout");
		Layout.SetChildValue("PreviewVisible", Stored.bPreviewVisible);
		Layout.SetChildValue("DetailsVisible", Stored.bDetailsVisible);
		Layout.SetChildValue("ParametersVisible", Stored.bParametersVisible);
		Layout.SetChildValue("DiagnosticsVisible", Stored.bDiagnosticsVisible);

		FYamlNodeRef Entries = Root.AddSequence("GraphViewports");
		std::vector<std::string> ResourceIds;
		ResourceIds.reserve(Stored.Viewports.size());
		for (const auto& [ResourceId, State] : Stored.Viewports)
			ResourceIds.push_back(ResourceId);
		std::ranges::sort(ResourceIds);
		for (const std::string& ResourceId : ResourceIds)
		{
			const FMaterialGraphViewportState& State = Stored.Viewports.at(ResourceId);
			FYamlNodeRef Entry = Entries.AppendMap();
			Entry.SetChildValue("Resource", ResourceId);
			Entry.SetChildValue("Zoom", static_cast<double>(State.Zoom));
			Entry.SetChildValue("PanX", static_cast<double>(State.Pan.x));
			Entry.SetChildValue("PanY", static_cast<double>(State.Pan.y));
		}
		const auto Text = Document.ToString();
		if (!FFileHelper::SaveArrayToFileAtomically(std::as_bytes(std::span(Text.data(), Text.size())), FPaths::LaunchConfigsDir() + FileName))
		{
			DURIN_WARN("Failed to save material editor profile settings.");
			return false;
		}
		SavedPreferences = Preferences();
		DirtyViewports.clear();
		return true;
	}

	auto FMaterialEditorProfileSettings::FindViewport(std::string_view ResourceId) const
		-> const FMaterialGraphViewportState*
	{
		const auto It = Viewports.find(std::string(ResourceId));
		return It == Viewports.end() ? nullptr : &It->second;
	}

	auto FMaterialEditorProfileSettings::SetViewport(
		std::string_view ResourceId,
		const FMaterialGraphViewportState& State) -> void
	{
		if (!ResourceId.empty()) { Viewports[std::string(ResourceId)] = State; DirtyViewports.emplace(ResourceId); }
	}

	auto FMaterialEditorProfileSettings::MoveViewport(
		std::string_view OldResourceId,
		std::string_view NewResourceId) -> void
	{
		const auto It = Viewports.find(std::string(OldResourceId));
		if (It == Viewports.end()) return;
		Viewports[std::string(NewResourceId)] = It->second;
		Viewports.erase(It);
		DirtyViewports.emplace(OldResourceId); DirtyViewports.emplace(NewResourceId);
	}
	auto FMaterialEditorProfileSettings::RemapStoredViewports(const FAssetData& Asset, const FSoftObjectPath& Old) -> void
	{
		const auto Directory = std::filesystem::path(FPaths::LaunchConfigsDir()) / "MaterialEditor/Profiles";
		std::error_code Error;
		if (!std::filesystem::is_directory(Directory, Error)) return;
		for (const auto& Entry : std::filesystem::directory_iterator(Directory, Error))
		{
			if (Entry.path().extension() != ".yaml") continue;
			FMaterialEditorProfileSettings Settings("MaterialEditor/Profiles/" + Entry.path().filename().generic_string());
			if (!Settings.Load()) continue;
			std::vector<std::pair<std::string, std::string>> Renames;
			for (const auto& [Resource, Viewport] : Settings.Viewports)
			{
				FSoftObjectPath Path;
				if (!FSoftObjectPath::TryCreate(Resource, Path)) continue;
				if (Path.GetAssetPath() == Old.GetAssetPath())
				{
					FSoftObjectPath Destination;
					if (FSoftObjectPath::TryCreate(Asset.AssetPath, Path.GetSubobjectNames(), Destination)) Renames.emplace_back(Resource, Destination.ToString());
				}
			}
			for (const auto& [Source, Destination] : Renames) Settings.MoveViewport(Source, Destination);
			if (!Renames.empty() && !Settings.Save()) DURIN_WARN("Could not update material viewport preferences after resource relocation.");
		}
	}

}
