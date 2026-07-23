#include "Assets/StaticMeshImportDialog.h"

#include "AssetSystem.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	namespace
	{
		constexpr const char* ImportPresetNames[] = {
			"Durin (+X Forward, +Y Right, +Z Up)",
			"Y-Up / -Z Forward (+X Right)",
			"Custom"
		};
		constexpr const char* ImportAxisNames[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

		auto DrawImportAxisCombo(const char* Label, EStaticMeshImportAxis& Axis) -> bool
		{
			int Value = static_cast<int>(Axis);
			if (!ImGui::Combo(Label, &Value, ImportAxisNames, std::size(ImportAxisNames))) return false;
			Axis = static_cast<EStaticMeshImportAxis>(Value);
			return true;
		}
	}

	FStaticMeshImportDialog::FStaticMeshImportDialog(std::function<void()> InClearError, std::function<void(std::string)> InReportError, std::function<void(std::string)> InImported)
		: ClearError(std::move(InClearError))
		, ReportError(std::move(InReportError))
		, Imported(std::move(InImported))
	{
	}

	auto FStaticMeshImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		SourcePathBuffer.fill(0);
		AssetPathBuffer.fill(0);
		LastSuggestedAssetPath.clear();
		PreferredDestinationDirectory = DestinationDirectory;
		ImportSettings = FStaticMeshImportSettings::MakeDurin();
		ImportPreset = EStaticMeshImportPreset::Durin;
		if (!PreferredDestinationDirectory.empty() && !PreferredDestinationDirectory.ends_with('/')) PreferredDestinationDirectory += '/';
		bOpenRequested = true;
	}

	auto FStaticMeshImportDialog::Draw() -> void
	{
		if (bOpenRequested)
		{
			ImGui::OpenPopup("Import Static Mesh");
			bOpenRequested = false;
		}

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(
			"Import Static Mesh",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
		))
			return;

		ImGui::TextUnformatted("Create a static mesh asset from a model file.");
		ImGui::TextDisabled("The source model is copied next to the .dasset package so they can be moved together.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source model");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##ImportSource", "Choose an OBJ, FBX, glTF, or other supported model...", SourcePathBuffer.data(), SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		if (bHasSource)
			ImGui::TextDisabled("%s", std::format("{}  |  {}", SourcePath.extension().generic_string(), SourcePath.filename().generic_string()).c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		int PresetIndex = static_cast<int>(ImportPreset);
		if (ImGui::Combo("Preset", &PresetIndex, ImportPresetNames, std::size(ImportPresetNames)))
		{
			ImportPreset = static_cast<EStaticMeshImportPreset>(PresetIndex);
			if (ImportPreset == EStaticMeshImportPreset::Durin)
				ImportSettings = FStaticMeshImportSettings::MakeDurin();
			else if (ImportPreset == EStaticMeshImportPreset::YUpNegativeZForward)
				ImportSettings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
		}
		if (ImportPreset == EStaticMeshImportPreset::Custom)
		{
			DrawImportAxisCombo("Forward", ImportSettings.ForwardAxis);
			DrawImportAxisCombo("Right", ImportSettings.RightAxis);
			DrawImportAxisCombo("Up", ImportSettings.UpAxis);
		}
		else
		{
			ImGui::TextDisabled("Source axes are baked into Durin's +X Forward / +Y Right / +Z Up basis.");
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		ImGui::TextUnformatted("Asset path");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##ImportAssetPath", "/Project/StaticMeshes/AssetName", AssetPathBuffer.data(), AssetPathBuffer.size());
		ImGui::SameLine();
		if (ImGui::Button("Choose...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseDestination();

		FAssetPath ParsedAssetPath;
		std::string AssetPathError;
		const bool bAssetPathValid = FAssetPath::TryCreate(AssetPathBuffer.data(), ParsedAssetPath, &AssetPathError);
		bool bMountedDestination = false;
		if (bAssetPathValid)
		{
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				if (ParsedAssetPath.GetView().starts_with(Mount.VirtualRoot))
				{
					bMountedDestination = true;
					break;
				}
			}
		}
		const bool bAssetExists = bAssetPathValid && (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath));
		std::string ImportSettingsError;
		const bool bImportSettingsValid = ImportSettings.IsValid(&ImportSettingsError);

		if (bAssetPathValid && bMountedDestination && bHasSource)
		{
			const std::string SourceFileName = std::string(ParsedAssetPath.GetAssetName()) + SourcePath.extension().generic_string();
			ImGui::BeginChild("ImportOutputPreview", ImVec2(0.0f, MonaImGui::ScaleUI(58.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Files to create");
			ImGui::TextUnformatted(std::format("{}.dasset   +   {}", ParsedAssetPath.GetAssetName(), SourceFileName).c_str());
			ImGui::EndChild();
		}

		std::string ValidationMessage;
		if (!bHasSource)
			ValidationMessage = "Select a source model to continue.";
		else if (!bSourceExists)
			ValidationMessage = "The selected source file no longer exists.";
		else if (!bImportSettingsValid)
			ValidationMessage = ImportSettingsError;
		else if (!bAssetPathValid)
			ValidationMessage = AssetPathError;
		else if (!bMountedDestination)
			ValidationMessage = "Choose a destination inside a mounted Content directory.";
		else if (bAssetExists)
			ValidationMessage = "An asset already exists at this path.";

		if (!ValidationMessage.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
			ImGui::TextWrapped("%s", ValidationMessage.c_str());
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!ValidationMessage.empty());
		if (ImGui::Button("Import Static Mesh", ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f)) && Import()) ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FStaticMeshImportDialog::BrowseSource() -> void
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
		if (SourcePathBuffer[0] != '\0') Request.InitialDirectory = std::filesystem::path(SourcePathBuffer.data()).parent_path().generic_string();

		FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		if (Result.FilePath.size() >= SourcePathBuffer.size())
		{
			SetError("The selected file path is too long for the import form.");
			return;
		}

		const std::string PreviousAssetPath = AssetPathBuffer.data();
		SourcePathBuffer.fill(0);
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(), std::min(Result.FilePath.size(), SourcePathBuffer.size() - 1));
		std::string Extension = std::filesystem::path(Result.FilePath).extension().generic_string();
		std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
		if (Extension == ".obj")
		{
			ImportPreset = EStaticMeshImportPreset::YUpNegativeZForward;
			ImportSettings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
		}
		else
		{
			ImportPreset = EStaticMeshImportPreset::Durin;
			ImportSettings = FStaticMeshImportSettings::MakeDurin();
		}
		const std::string AssetName = StringUtils::SanitizeFileName(std::filesystem::path(Result.FilePath).stem().generic_string(), "StaticMesh");
		const FProjectInfo* Project = GetCurrentProject();
		const std::string SuggestedPath = !PreferredDestinationDirectory.empty() ? PreferredDestinationDirectory + AssetName : (Project ? Project->MountRoot : "/") + "StaticMeshes/" + AssetName;
		if (PreviousAssetPath.empty() || PreviousAssetPath == LastSuggestedAssetPath)
		{
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), SuggestedPath.data(), std::min(SuggestedPath.size(), AssetPathBuffer.size() - 1));
		}
		LastSuggestedAssetPath = SuggestedPath;
	}

	auto FStaticMeshImportDialog::BrowseDestination() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose a Static Mesh Asset Path";
		Request.Filters = {{"Durin Asset", "*.dasset"}};
		Request.DefaultFileName = SourcePathBuffer[0] != '\0' ? StringUtils::SanitizeFileName(std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(), "StaticMesh") + ".dasset" : "StaticMesh.dasset";

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
			if (VirtualPath.size() >= AssetPathBuffer.size())
			{
				SetError("The selected asset path is too long for the import form.");
				return;
			}
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), VirtualPath.data(), VirtualPath.size());
			LastSuggestedAssetPath.clear();
			return;
		}

		SetError("Static mesh assets must be saved inside a mounted Content directory.");
	}

	auto FStaticMeshImportDialog::Import() -> bool
	{
		if (ClearError) ClearError();
		FStaticMeshImportResult Result = DStaticMesh::ImportAsset(SourcePathBuffer.data(), AssetPathBuffer.data(), ImportSettings);
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		if (Imported) Imported(AssetPathBuffer.data());
		FAssetPath ImportedPath;
		if (FAssetPath::TryCreate(AssetPathBuffer.data(), ImportedPath)) Asset::UnloadPackage(ImportedPath);
		return true;
	}

	auto FStaticMeshImportDialog::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin
