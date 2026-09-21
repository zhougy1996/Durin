#pragma once

#include "Import/AssetDestinationValidation.h"
#include "DurinEdAPI.h"

namespace Durin::Editor
{
	// Routes feature-owned import-dialog outcomes through host-owned services.
	struct FImportDialogCallbacks
	{
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		// Host-owned post-save presentation hook. Refresh/reveal/open policy stays
		// outside IAssetTools and the concrete factory.
		std::function<void(std::string)> AssetCreated;
		std::function<void(std::string)> ImportedDirectory;

		DURINED_API auto Clear() const -> void;
		DURINED_API auto Report(std::string Message) const -> void;
		DURINED_API auto NotifyAssetCreated(std::string_view AssetPath) const -> void;
		DURINED_API auto NotifyImportedDirectory(std::string_view DirectoryPath) const -> void;
	};

	// Shared text state and presentation for virtual asset and directory paths.
	class FImportDialogPathModel
	{
	public:
		static constexpr size_t PathCapacity = 256;

		DURINED_API auto Reset(std::string_view PreferredDirectory = {}) -> void;
		auto GetPath() const -> std::string_view { return PathBuffer.data(); }
		DURINED_API auto MakeSuggestedPath(std::string_view Name,
			std::string_view FallbackDirectory) const -> std::string;
		DURINED_API auto SuggestPath(std::string_view SuggestedPath) -> void;
		DURINED_API auto SetPath(std::string_view Path) -> bool;
		DURINED_API auto DrawRow(const char* Label, const char* InputId, const char* Hint,
			const char* BrowseLabel, float BrowseButtonWidth) -> bool;

	protected:
		std::array<char, PathCapacity> PathBuffer{};

	private:
		std::string PreferredDirectory;
		std::string LastSuggestedPath;
	};

	// Adds asset validation and file browsing to the shared path model.
	class FImportDialogDestinationModel : public FImportDialogPathModel
	{
	public:
		static constexpr size_t AssetPathCapacity = PathCapacity;
		auto GetPathBuffer() -> std::array<char, AssetPathCapacity>& { return PathBuffer; }
		auto GetPathBuffer() const -> const std::array<char, AssetPathCapacity>& { return PathBuffer; }
		DURINED_API auto Inspect(FAssetDestinationOccupancyQuery OccupancyQuery = nullptr) const
			-> FAssetDestinationValidation;
		DURINED_API auto Browse(std::string_view Title, std::string_view DefaultFileName,
			std::string_view TooLongMessage, std::string_view OutsideMountMessage,
			const FImportDialogCallbacks& Callbacks) -> bool;
	};

	// Adds directory validation and folder browsing for multi-output imports.
	class FImportDialogDirectoryModel : public FImportDialogPathModel
	{
	public:
		static constexpr size_t DirectoryPathCapacity = PathCapacity;
		DURINED_API auto Inspect() const -> FContentDirectoryValidation;
		DURINED_API auto Browse(std::string_view Title, std::string_view TooLongMessage,
			std::string_view OutsideMountMessage,
			const FImportDialogCallbacks& Callbacks) -> bool;
	};

	// Tracks an immediate-mode import popup's deferred open request.
	class FImportDialogModalState
	{
	public:
		auto RequestOpen() -> void { bOpenRequested = true; }
		DURINED_API auto OpenPopupIfRequested(const char* PopupName) -> void;

	private:
		bool bOpenRequested = false;
	};

	DURINED_API auto DrawImportDialogWarning(std::string_view Message) -> void;
} // namespace Durin::Editor
