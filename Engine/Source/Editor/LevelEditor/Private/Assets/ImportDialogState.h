#pragma once

#include "Assets/AssetDestinationValidation.h"

namespace Durin
{
	// Routes import-dialog outcomes to the owning editor workspace.
	struct FImportDialogCallbacks
	{
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;

		auto Clear() const -> void;
		auto Report(std::string Message) const -> void;
		auto NotifyImported(std::string_view AssetPath) const -> void;
	};

	// Owns editable asset-destination state and its suggestion and browse rules.
	class FImportDialogDestinationModel
	{
	public:
		static constexpr size_t AssetPathCapacity = 256;

		auto Reset(std::string_view PreferredDirectory = {}) -> void;
		auto GetPathBuffer() -> std::array<char, AssetPathCapacity>& { return AssetPathBuffer; }
		auto GetPathBuffer() const -> const std::array<char, AssetPathCapacity>& { return AssetPathBuffer; }
		auto GetPath() const -> std::string_view { return AssetPathBuffer.data(); }
		auto MakeSuggestedPath(std::string_view AssetName,
			std::string_view FallbackDirectory) const -> std::string;
		auto SuggestPath(std::string_view SuggestedPath) -> void;
		auto SetPath(std::string_view AssetPath) -> bool;
		auto Inspect(FAssetDestinationOccupancyQuery OccupancyQuery = nullptr) const
			-> FAssetDestinationValidation;

		auto DrawRow(const char* Label, const char* InputId, const char* Hint,
			const char* BrowseLabel, float BrowseButtonWidth) -> bool;
		auto Browse(std::string_view Title, std::string_view DefaultFileName,
			std::string_view TooLongMessage, std::string_view OutsideMountMessage,
			const FImportDialogCallbacks& Callbacks) -> bool;

	private:
		std::string PreferredDirectory;
		std::array<char, AssetPathCapacity> AssetPathBuffer{};
		std::string LastSuggestedPath;
	};

	// Tracks an immediate-mode import popup's deferred open request.
	class FImportDialogModalState
	{
	public:
		auto RequestOpen() -> void { bOpenRequested = true; }
		auto OpenPopupIfRequested(const char* PopupName) -> void;

	private:
		bool bOpenRequested = false;
	};

	auto DrawImportDialogWarning(std::string_view Message) -> void;
} // namespace Durin
