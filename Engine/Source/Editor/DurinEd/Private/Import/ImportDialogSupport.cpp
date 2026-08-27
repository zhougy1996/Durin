#include "Editor/Import/ImportDialogSupport.h"

#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "MonaImGui.h"

namespace Durin::Editor::Import
{
	auto FImportDialogCallbacks::Clear() const -> void
	{
		if (ClearError) ClearError();
	}

	auto FImportDialogCallbacks::Report(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}

	auto FImportDialogCallbacks::NotifyImported(std::string_view AssetPath) const -> void
	{
		if (Imported) Imported(std::string(AssetPath));
	}

	auto FImportDialogCallbacks::NotifyImportedDirectory(
		std::string_view DirectoryPath) const -> void
	{
		if (ImportedDirectory) ImportedDirectory(std::string(DirectoryPath));
	}

	auto FImportDialogDestinationModel::Reset(std::string_view InPreferredDirectory)
		-> void
	{
		PreferredDirectory = InPreferredDirectory;
		if (!PreferredDirectory.empty() && !PreferredDirectory.ends_with('/'))
			PreferredDirectory += '/';
		AssetPathBuffer.fill(0);
		LastSuggestedPath.clear();
	}

	auto FImportDialogDestinationModel::MakeSuggestedPath(
		std::string_view AssetName, std::string_view FallbackDirectory) const
		-> std::string
	{
		return std::string(PreferredDirectory.empty()
			? FallbackDirectory : PreferredDirectory) + std::string(AssetName);
	}

	auto FImportDialogDestinationModel::SuggestPath(std::string_view SuggestedPath)
		-> void
	{
		const std::string_view CurrentPath = AssetPathBuffer.data();
		if (CurrentPath.empty() || CurrentPath == LastSuggestedPath)
		{
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), SuggestedPath.data(),
				std::min(SuggestedPath.size(), AssetPathBuffer.size() - 1));
		}
		LastSuggestedPath = SuggestedPath;
	}

	auto FImportDialogDestinationModel::SetPath(std::string_view AssetPath) -> bool
	{
		if (AssetPath.size() >= AssetPathBuffer.size()) return false;
		AssetPathBuffer.fill(0);
		std::memcpy(AssetPathBuffer.data(), AssetPath.data(), AssetPath.size());
		LastSuggestedPath.clear();
		return true;
	}

	auto FImportDialogDestinationModel::Inspect(
		FAssetDestinationOccupancyQuery OccupancyQuery) const
		-> FAssetDestinationValidation
	{
		return InspectAssetDestination(AssetPathBuffer.data(), OccupancyQuery);
	}

	auto FImportDialogDestinationModel::DrawRow(const char* Label,
		const char* InputId, const char* Hint, const char* BrowseLabel,
		float BrowseButtonWidth) -> bool
	{
		ImGui::TextUnformatted(Label);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
			- BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint(InputId, Hint, AssetPathBuffer.data(),
			AssetPathBuffer.size());
		ImGui::SameLine();
		return ImGui::Button(BrowseLabel, ImVec2(BrowseButtonWidth, 0.0f));
	}

	auto FImportDialogDestinationModel::Browse(std::string_view Title,
		std::string_view DefaultFileName, std::string_view TooLongMessage,
		std::string_view OutsideMountMessage,
		const FImportDialogCallbacks& Callbacks) -> bool
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = Title;
		Request.Filters = {{"Durin Asset", "*.dasset"}};
		Request.DefaultFileName = DefaultFileName;
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(
					Project->MountRoot + std::string("Destination"));
			if (Lookup)
				Request.InitialDirectory =
					Lookup.Mount->GetContentDir().generic_string();
		}

		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return false;
		if (Result.Status == EFileDialogStatus::Error)
		{
			Callbacks.Report(Result.ErrorMessage);
			return false;
		}
		const FAssetDestinationValidation Destination =
			ClassifyAssetDestination(Result.FilePath);
		if (!Destination.bMountedDestination)
		{
			Callbacks.Report(std::string(OutsideMountMessage));
			return false;
		}
		if (!SetPath(Destination.AssetPath.ToString()))
		{
			Callbacks.Report(std::string(TooLongMessage));
			return false;
		}
		return true;
	}

	auto FImportDialogDirectoryModel::Reset(
		std::string_view InPreferredDirectory) -> void
	{
		PreferredDirectory = InPreferredDirectory;
		if (!PreferredDirectory.empty() && !PreferredDirectory.ends_with('/'))
			PreferredDirectory += '/';
		DirectoryPathBuffer.fill(0);
		LastSuggestedPath.clear();
	}

	auto FImportDialogDirectoryModel::MakeSuggestedPath(
		std::string_view DirectoryName,
		std::string_view FallbackDirectory) const -> std::string
	{
		return std::string(PreferredDirectory.empty()
			? FallbackDirectory : PreferredDirectory) + std::string(DirectoryName);
	}

	auto FImportDialogDirectoryModel::SuggestPath(
		std::string_view SuggestedPath) -> void
	{
		const std::string_view CurrentPath = DirectoryPathBuffer.data();
		if (CurrentPath.empty() || CurrentPath == LastSuggestedPath)
		{
			DirectoryPathBuffer.fill(0);
			std::memcpy(DirectoryPathBuffer.data(), SuggestedPath.data(),
				std::min(SuggestedPath.size(), DirectoryPathBuffer.size() - 1));
		}
		LastSuggestedPath = SuggestedPath;
	}

	auto FImportDialogDirectoryModel::SetPath(
		std::string_view DirectoryPath) -> bool
	{
		if (DirectoryPath.size() >= DirectoryPathBuffer.size()) return false;
		DirectoryPathBuffer.fill(0);
		std::memcpy(DirectoryPathBuffer.data(), DirectoryPath.data(),
			DirectoryPath.size());
		LastSuggestedPath.clear();
		return true;
	}

	auto FImportDialogDirectoryModel::Inspect() const
		-> FContentDirectoryValidation
	{
		return InspectContentDirectory(DirectoryPathBuffer.data());
	}

	auto FImportDialogDirectoryModel::DrawRow(const char* Label,
		const char* InputId, const char* Hint, const char* BrowseLabel,
		float BrowseButtonWidth) -> bool
	{
		ImGui::TextUnformatted(Label);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
			- BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint(InputId, Hint, DirectoryPathBuffer.data(),
			DirectoryPathBuffer.size());
		ImGui::SameLine();
		return ImGui::Button(BrowseLabel, ImVec2(BrowseButtonWidth, 0.0f));
	}

	auto FImportDialogDirectoryModel::Browse(std::string_view Title,
		std::string_view TooLongMessage, std::string_view OutsideMountMessage,
		const FImportDialogCallbacks& Callbacks) -> bool
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = Title;
		const FContentDirectoryValidation Current = Inspect();
		if (Current && std::filesystem::is_directory(Current.PhysicalPath))
			Request.InitialDirectory = Current.PhysicalPath.generic_string();
		else if (const FProjectInfo* Project = GetCurrentProject())
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(
					Project->MountRoot + std::string("Destination"));
			if (Lookup)
				Request.InitialDirectory = Lookup.Mount->GetContentDir().generic_string();
		}

		const FFileDialogResult Result = OpenFolderDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return false;
		if (Result.Status == EFileDialogStatus::Error)
		{
			Callbacks.Report(Result.ErrorMessage);
			return false;
		}
		const FContentDirectoryValidation Directory =
			ClassifyContentDirectory(Result.FilePath);
		if (!Directory.bMountedDestination)
		{
			Callbacks.Report(std::string(OutsideMountMessage));
			return false;
		}
		if (!SetPath(Directory.DirectoryPath.ToString()))
		{
			Callbacks.Report(std::string(TooLongMessage));
			return false;
		}
		return true;
	}

	auto FImportDialogModalState::OpenPopupIfRequested(
		const char* PopupName) -> void
	{
		if (!bOpenRequested) return;
		ImGui::OpenPopup(PopupName);
		bOpenRequested = false;
	}

	auto FMeshCoordinateImportModel::Reset() -> void
	{
		SetPreset(EPreset::Durin);
	}

	auto FMeshCoordinateImportModel::SetPreset(EPreset InPreset) -> void
	{
		Preset = InPreset;
		if (Preset == EPreset::Durin)
			Settings = FStaticMeshImportSettings::MakeDurin();
		else if (Preset == EPreset::YUpNegativeZForward)
			Settings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
	}

	auto FMeshCoordinateImportModel::Draw() -> void
	{
		static constexpr const char* PresetNames[] = {
			"Durin (+X Forward, +Y Right, +Z Up)",
			"Y-Up / -Z Forward (+X Right)",
			"Custom"};
		static constexpr const char* AxisNames[] = {
			"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
		int PresetIndex = static_cast<int>(Preset);
		if (ImGui::Combo("Preset", &PresetIndex, PresetNames, std::size(PresetNames)))
		{
			SetPreset(static_cast<EPreset>(PresetIndex));
		}
		if (Preset == EPreset::Custom)
		{
			auto DrawAxis = [&](const char* Label, EStaticMeshImportAxis& Axis) {
				int Value = static_cast<int>(Axis);
				if (!ImGui::Combo(Label, &Value, AxisNames, std::size(AxisNames))) return;
				Axis = static_cast<EStaticMeshImportAxis>(Value);
			};
			DrawAxis("Forward", Settings.ForwardAxis);
			DrawAxis("Right", Settings.RightAxis);
			DrawAxis("Up", Settings.UpAxis);
		}
		else
		{
			ImGui::TextDisabled(
				"Source axes are baked into Durin's +X Forward / +Y Right / +Z Up basis.");
		}
	}

	auto DrawImportDialogWarning(std::string_view Message) -> void
	{
		if (Message.empty()) return;
		ImGui::PushStyleColor(ImGuiCol_Text,
			MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
		ImGui::TextWrapped("%s", std::string(Message).c_str());
		ImGui::PopStyleColor();
	}
} // namespace Durin::Editor::Import
