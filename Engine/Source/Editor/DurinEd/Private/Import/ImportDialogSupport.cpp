#include "Import/ImportDialogSupport.h"

#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/Project.h"
#include "MonaImGui.h"

namespace Durin::Editor
{
	auto FImportDialogCallbacks::Clear() const -> void
	{
		if (ClearError) ClearError();
	}

	auto FImportDialogCallbacks::Report(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}

	auto FImportDialogCallbacks::NotifyAssetCreated(std::string_view AssetPath) const -> void
	{
		if (AssetCreated) AssetCreated(std::string(AssetPath));
	}

	auto FImportDialogCallbacks::NotifyImportedDirectory(
		std::string_view DirectoryPath) const -> void
	{
		if (ImportedDirectory) ImportedDirectory(std::string(DirectoryPath));
	}

	auto FImportDialogPathModel::Reset(std::string_view InPreferredDirectory)
		-> void
	{
		PreferredDirectory = InPreferredDirectory;
		if (!PreferredDirectory.empty() && !PreferredDirectory.ends_with('/'))
			PreferredDirectory += '/';
		PathBuffer.fill(0);
		LastSuggestedPath.clear();
	}

	auto FImportDialogPathModel::MakeSuggestedPath(
		std::string_view AssetName, std::string_view FallbackDirectory) const
		-> std::string
	{
		return std::string(PreferredDirectory.empty()
			? FallbackDirectory : PreferredDirectory) + std::string(AssetName);
	}

	auto FImportDialogPathModel::SuggestPath(std::string_view SuggestedPath)
		-> void
	{
		const std::string_view CurrentPath = PathBuffer.data();
		if (CurrentPath.empty() || CurrentPath == LastSuggestedPath)
		{
			PathBuffer.fill(0);
			std::memcpy(PathBuffer.data(), SuggestedPath.data(),
				std::min(SuggestedPath.size(), PathBuffer.size() - 1));
		}
		LastSuggestedPath = SuggestedPath;
	}

	auto FImportDialogPathModel::SetPath(std::string_view AssetPath) -> bool
	{
		if (AssetPath.size() >= PathBuffer.size()) return false;
		PathBuffer.fill(0);
		std::memcpy(PathBuffer.data(), AssetPath.data(), AssetPath.size());
		LastSuggestedPath.clear();
		return true;
	}

	auto FImportDialogDestinationModel::Inspect(
		FAssetDestinationOccupancyQuery OccupancyQuery) const
		-> FAssetDestinationValidation
	{
		return InspectAssetDestination(PathBuffer.data(), OccupancyQuery);
	}

	auto FImportDialogPathModel::DrawRow(const char* Label,
		const char* InputId, const char* Hint, const char* BrowseLabel,
		float BrowseButtonWidth) -> bool
	{
		ImGui::TextUnformatted(Label);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
			- BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint(InputId, Hint, PathBuffer.data(),
			PathBuffer.size());
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
			const FMountLookupResult Lookup =
				FMountPaths::FindMountForVirtualPath(
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

	auto FImportDialogDirectoryModel::Inspect() const
		-> FContentDirectoryValidation
	{
		return InspectContentDirectory(PathBuffer.data());
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
			const FMountLookupResult Lookup =
				FMountPaths::FindMountForVirtualPath(
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

	auto DrawImportDialogWarning(std::string_view Message) -> void
	{
		if (Message.empty()) return;
		ImGui::PushStyleColor(ImGuiCol_Text,
			MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
		ImGui::TextWrapped("%s", std::string(Message).c_str());
		ImGui::PopStyleColor();
	}
} // namespace Durin::Editor
