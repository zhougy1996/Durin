#pragma once

#include "Editor/Import/AssetDestinationValidation.h"
#include "DurinEdAPI.h"
#include "StaticMesh/StaticMesh.h"

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

	// Owns editable asset-destination state and its suggestion and browse rules.
	class FImportDialogDestinationModel
	{
	public:
		static constexpr size_t AssetPathCapacity = 256;

		DURINED_API auto Reset(std::string_view PreferredDirectory = {}) -> void;
		auto GetPathBuffer() -> std::array<char, AssetPathCapacity>& { return AssetPathBuffer; }
		auto GetPathBuffer() const -> const std::array<char, AssetPathCapacity>& { return AssetPathBuffer; }
		auto GetPath() const -> std::string_view { return AssetPathBuffer.data(); }
		DURINED_API auto MakeSuggestedPath(std::string_view AssetName,
			std::string_view FallbackDirectory) const -> std::string;
		DURINED_API auto SuggestPath(std::string_view SuggestedPath) -> void;
		DURINED_API auto SetPath(std::string_view AssetPath) -> bool;
		DURINED_API auto Inspect(FAssetDestinationOccupancyQuery OccupancyQuery = nullptr) const
			-> FAssetDestinationValidation;

		DURINED_API auto DrawRow(const char* Label, const char* InputId, const char* Hint,
			const char* BrowseLabel, float BrowseButtonWidth) -> bool;
		DURINED_API auto Browse(std::string_view Title, std::string_view DefaultFileName,
			std::string_view TooLongMessage, std::string_view OutsideMountMessage,
			const FImportDialogCallbacks& Callbacks) -> bool;

	private:
		std::string PreferredDirectory;
		std::array<char, AssetPathCapacity> AssetPathBuffer{};
		std::string LastSuggestedPath;
	};

	// Owns an editable virtual asset directory for multi-output imports.
	class FImportDialogDirectoryModel
	{
	public:
		static constexpr size_t DirectoryPathCapacity = 256;

		DURINED_API auto Reset(std::string_view PreferredDirectory = {}) -> void;
		auto GetPath() const -> std::string_view { return DirectoryPathBuffer.data(); }
		DURINED_API auto MakeSuggestedPath(std::string_view DirectoryName,
			std::string_view FallbackDirectory) const -> std::string;
		DURINED_API auto SuggestPath(std::string_view SuggestedPath) -> void;
		DURINED_API auto SetPath(std::string_view DirectoryPath) -> bool;
		DURINED_API auto Inspect() const -> FContentDirectoryValidation;

		DURINED_API auto DrawRow(const char* Label, const char* InputId, const char* Hint,
			const char* BrowseLabel, float BrowseButtonWidth) -> bool;
		DURINED_API auto Browse(std::string_view Title, std::string_view TooLongMessage,
			std::string_view OutsideMountMessage,
			const FImportDialogCallbacks& Callbacks) -> bool;

	private:
		std::string PreferredDirectory;
		std::array<char, DirectoryPathCapacity> DirectoryPathBuffer{};
		std::string LastSuggestedPath;
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

	class FMeshCoordinateImportModel
	{
	public:
		enum class EPreset : uint8
		{
			Durin,
			YUpNegativeZForward,
			Custom
		};

		DURINED_API auto Reset() -> void;
		DURINED_API auto SetPreset(EPreset InPreset) -> void;
		DURINED_API auto Draw() -> void;
		auto GetSettings() -> FStaticMeshImportSettings& { return Settings; }
		auto GetSettings() const -> const FStaticMeshImportSettings& { return Settings; }

	private:
		FStaticMeshImportSettings Settings = FStaticMeshImportSettings::MakeDurin();
		EPreset Preset = EPreset::Durin;
	};

	DURINED_API auto DrawImportDialogWarning(std::string_view Message) -> void;
} // namespace Durin::Editor
