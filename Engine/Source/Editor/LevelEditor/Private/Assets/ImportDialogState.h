#pragma once

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Level
{
	// Routes import-dialog outcomes to the owning editor workspace.
	struct FImportDialogCallbacks
	{
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;
		std::function<void(std::string)> ImportedDirectory;

		auto Clear() const -> void;
		auto Report(std::string Message) const -> void;
		auto NotifyImported(std::string_view AssetPath) const -> void;
		auto NotifyImportedDirectory(std::string_view DirectoryPath) const -> void;
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

	// Owns an editable virtual asset directory for multi-output imports.
	class FImportDialogDirectoryModel
	{
	public:
		static constexpr size_t DirectoryPathCapacity = 256;

		auto Reset(std::string_view PreferredDirectory = {}) -> void;
		auto GetPath() const -> std::string_view { return DirectoryPathBuffer.data(); }
		auto MakeSuggestedPath(std::string_view DirectoryName,
			std::string_view FallbackDirectory) const -> std::string;
		auto SuggestPath(std::string_view SuggestedPath) -> void;
		auto SetPath(std::string_view DirectoryPath) -> bool;
		auto Inspect() const -> FContentDirectoryValidation;

		auto DrawRow(const char* Label, const char* InputId, const char* Hint,
			const char* BrowseLabel, float BrowseButtonWidth) -> bool;
		auto Browse(std::string_view Title, std::string_view TooLongMessage,
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
		auto OpenPopupIfRequested(const char* PopupName) -> void;

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

		auto Reset() -> void;
		auto SetPreset(EPreset InPreset) -> void;
		auto Draw() -> void;
		auto GetSettings() -> FStaticMeshImportSettings& { return Settings; }
		auto GetSettings() const -> const FStaticMeshImportSettings& { return Settings; }

	private:
		FStaticMeshImportSettings Settings = FStaticMeshImportSettings::MakeDurin();
		EPreset Preset = EPreset::Durin;
	};

	class FMountedSourceImportFormModel
	{
	public:
		static constexpr size_t PathCapacity = 512;

		auto Reset() -> void;
		auto SuggestDestination(std::string_view SuggestedPath) -> void;
		auto SetDestination(std::string_view VirtualPath) -> bool;
		auto Inspect(std::string_view ReferencingPath,
			bool bEngineAuthoringContext = false) const -> FMountedSourceImportDiagnostic;
		auto DrawMode(std::string_view ExternalDescription) -> void;
		auto DrawSourceRow(const char* InputId, const char* Hint,
			float BrowseButtonWidth) -> bool;
		auto DrawDestinationRow(const char* InputId, const char* Hint,
			float BrowseButtonWidth) -> bool;
		auto GetSourcePathBuffer() -> std::array<char, PathCapacity>& { return SourcePath; }
		auto GetDestinationBuffer() -> std::array<char, PathCapacity>& { return Destination; }
		auto GetMode() -> EMountedSourceImportMode& { return Mode; }

	private:
		std::array<char, PathCapacity> SourcePath{};
		std::array<char, PathCapacity> Destination{};
		std::string LastSuggestion;
		EMountedSourceImportMode Mode = EMountedSourceImportMode::IngestExternal;
	};

	auto DrawImportDialogWarning(std::string_view Message) -> void;
} // namespace Durin::Editor::Level
