#include "Assets/StaticMeshImportDialog.h"

#include "Assets/MountedSourceImport.h"
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
		SourceDestinationBuffer.fill(0);
		LastSuggestedAssetPath.clear();
		LastSuggestedSourceDestination.clear();
		PreferredDestinationDirectory = DestinationDirectory;
		ImportSettings = FStaticMeshImportSettings::MakeDurin();
		ImportPreset = EStaticMeshImportPreset::Durin;
		SourceMode = EMountedSourceImportMode::IngestExternal;
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
		ImGui::TextDisabled("Reference a mounted source in place, or ingest an external model into SourceAssets.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source model");
		if (ImGui::RadioButton("Reference Existing Source",
			SourceMode == EMountedSourceImportMode::ReferenceExisting))
			SourceMode = EMountedSourceImportMode::ReferenceExisting;
		ImGui::SameLine();
		if (ImGui::RadioButton("Ingest External Source",
			SourceMode == EMountedSourceImportMode::IngestExternal))
			SourceMode = EMountedSourceImportMode::IngestExternal;
		ImGui::TextDisabled(SourceMode == EMountedSourceImportMode::ReferenceExisting
			? "Keeps a source already inside an allowed mounted SourceAssets domain; no copy is created."
			: "Copies an external model transactionally to the explicit mounted source path.");
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
		if (SourceMode == EMountedSourceImportMode::IngestExternal)
		{
			ImGui::TextUnformatted("Source virtual path");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::InputTextWithHint("##StaticMeshSourceDestination",
				"/Game/Models/AssetName.fbx", SourceDestinationBuffer.data(),
				SourceDestinationBuffer.size());
			ImGui::SameLine();
			if (ImGui::Button("Choose source...", ImVec2(BrowseButtonWidth, 0.0f)))
				BrowseSourceDestination();
		}

		FAssetPath ParsedAssetPath;
		std::string AssetPathError;
		const bool bAssetPathValid = FAssetPath::TryCreate(AssetPathBuffer.data(), ParsedAssetPath, &AssetPathError);
		const bool bMountedDestination = bAssetPathValid;
		const bool bAssetExists = bAssetPathValid && (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath));
		std::string ImportSettingsError;
		const bool bImportSettingsValid = ImportSettings.IsValid(&ImportSettingsError);
		const FMountedSourceImportDiagnostic SourceDiagnostic = bAssetPathValid
			? InspectMountedSourceImport(
				SourcePathBuffer.data(), ParsedAssetPath.GetView(),
				SourceDestinationBuffer.data(), SourceMode)
			: FMountedSourceImportDiagnostic{};

		if (bAssetPathValid && bMountedDestination && bHasSource && SourceDiagnostic.bValid)
		{
			ImGui::BeginChild("ImportOutputPreview", ImVec2(0.0f, MonaImGui::ScaleUI(78.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset destination");
			ImGui::TextUnformatted(ParsedAssetPath.ToString().c_str());
			ImGui::TextDisabled("Source virtual path");
			ImGui::TextUnformatted(SourceDiagnostic.VirtualPath.c_str());
			ImGui::EndChild();
			ImGui::TextDisabled("Mount: %s (%s)  |  %s  |  dependency allowed",
				SourceDiagnostic.Mount->VirtualRoot.c_str(),
				DescribeMountOwner(SourceDiagnostic.Mount->Owner),
				SourceDiagnostic.Mount->bSourceWritable ? "writable" : "read-only");
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
		else if (!SourceDiagnostic.bValid)
			ValidationMessage = SourceDiagnostic.Message;

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
		if (const FProjectInfo* Project = GetCurrentProject()) Request.InitialDirectory = Project->ProjectDir;
		if (SourceMode == EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPathBuffer.data());
			if (Lookup && Lookup.Mount->SourceAssetsRoot)
				Request.InitialDirectory = Lookup.Mount->SourceAssetsRoot->generic_string();
		}
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
		const std::string PreviousSourceDestination = SourceDestinationBuffer.data();
		const std::string SuggestedSourceDestination = MakeDefaultSourceVirtualPath(
			AssetPathBuffer.data(), "Models",
			AssetName + std::filesystem::path(Result.FilePath).extension().generic_string());
		if (PreviousSourceDestination.empty()
			|| PreviousSourceDestination == LastSuggestedSourceDestination)
		{
			SourceDestinationBuffer.fill(0);
			std::memcpy(SourceDestinationBuffer.data(), SuggestedSourceDestination.data(),
				std::min(SuggestedSourceDestination.size(),
					SourceDestinationBuffer.size() - 1));
		}
		LastSuggestedSourceDestination = SuggestedSourceDestination;
	}

	auto FStaticMeshImportDialog::BrowseDestination() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose a Static Mesh Asset Path";
		Request.Filters = {{"Durin Asset", "*.dasset"}};
		Request.DefaultFileName = SourcePathBuffer[0] != '\0' ? StringUtils::SanitizeFileName(std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(), "StaticMesh") + ".dasset" : "StaticMesh.dasset";

		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Project->MountRoot + std::string("Destination"));
			if (Lookup && Lookup.Mount->ContentRoot)
				Request.InitialDirectory = Lookup.Mount->ContentRoot->generic_string();
		}

		FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}

		const PathUtilities::FContentPathResult Classified =
			PathUtilities::ClassifyContentPath(Result.FilePath);
		if (Classified)
		{
			std::filesystem::path VirtualPath(Classified.NormalizedVirtualPath);
			VirtualPath.replace_extension();
			const std::string VirtualPathString = VirtualPath.generic_string();
			if (VirtualPathString.size() >= AssetPathBuffer.size())
			{
				SetError("The selected asset path is too long for the import form.");
				return;
			}
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), VirtualPathString.data(), VirtualPathString.size());
			LastSuggestedAssetPath.clear();
			return;
		}

		SetError("Static mesh assets must be saved inside a mounted Content directory.");
	}

	auto FStaticMeshImportDialog::BrowseSourceDestination() -> void
	{
		FAssetPath AssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPathBuffer.data(), AssetPath, &Error))
		{
			SetError("Choose a valid asset destination before selecting the source destination.");
			return;
		}
		const PathUtilities::FMountLookupResult Lookup =
			PathUtilities::FindMountForVirtualPath(AssetPath.GetView());
		if (!Lookup || !Lookup.Mount->SourceAssetsRoot)
		{
			SetError("The selected asset mount has no available SourceAssets domain.");
			return;
		}
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose Static Mesh Source Destination";
		Request.Filters = {{"All Files", "*.*"}};
		Request.InitialDirectory = Lookup.Mount->SourceAssetsRoot->generic_string();
		Request.DefaultFileName = SourcePathBuffer[0] != '\0'
			? std::filesystem::path(SourcePathBuffer.data()).filename().generic_string()
			: "StaticMesh.fbx";
		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(Result.FilePath);
		if (!Classified)
		{
			SetError(Classified.Message);
			return;
		}
		if (Classified.NormalizedVirtualPath.size() >= SourceDestinationBuffer.size())
		{
			SetError("The selected source destination is too long for the import form.");
			return;
		}
		SourceDestinationBuffer.fill(0);
		std::memcpy(SourceDestinationBuffer.data(), Classified.NormalizedVirtualPath.data(),
			Classified.NormalizedVirtualPath.size());
		LastSuggestedSourceDestination.clear();
	}

	auto FStaticMeshImportDialog::Import() -> bool
	{
		if (ClearError) ClearError();
		FStaticMeshImportResult Result = DStaticMesh::ImportAsset(
			SourcePathBuffer.data(), AssetPathBuffer.data(), ImportSettings,
			SourceMode == EMountedSourceImportMode::IngestExternal
				? SourceDestinationBuffer.data() : std::string_view{});
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
