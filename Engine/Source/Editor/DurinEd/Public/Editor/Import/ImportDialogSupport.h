#pragma once

#include "Editor/Import/AssetDestinationValidation.h"
#include "Editor/Import/MountedSourceImport.h"
#include "AssetForge/Operations/ImportOperation.h"
#include "DurinEdAPI.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Import
{
	enum class EImportDialogOperationState : uint8
	{
		Editing,
		Preparing,
		Finalizing,
		Succeeded,
		Failed,
		Canceled
	};

	// Adapts value-only import observation to dialog controls without retaining a Widget.
	class FImportDialogProgressModel
	{
	public:
		DURINED_API auto Begin(AssetForge::FImportOperationHandle InHandle) -> void;
		DURINED_API auto ApplySnapshot(AssetForge::FImportOperationSnapshot InSnapshot) -> void;
		DURINED_API auto Refresh() -> void;
		DURINED_API auto Reset() -> void;
		DURINED_API auto RequestCancel() -> bool;
		DURINED_API auto RunInBackground() -> bool;

		DURINED_API auto GetState() const -> EImportDialogOperationState;
		auto GetSnapshot() const -> const AssetForge::FImportOperationSnapshot& { return Snapshot; }
		auto HasOperation() const -> bool { return Handle.IsValid(); }
		auto CanCancel() const -> bool { return Snapshot.bCancelable && !Snapshot.IsTerminal(); }

	private:
		AssetForge::FImportOperationHandle Handle;
		AssetForge::FImportOperationSnapshot Snapshot;
	};

	// Routes feature-owned import-dialog outcomes through host-owned services.
	struct FImportDialogCallbacks
	{
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;
		std::function<void(std::string)> ImportedDirectory;
		std::function<void(AssetForge::FImportOperationHandle, std::string)> ImportStarted;

		DURINED_API auto Clear() const -> void;
		DURINED_API auto Report(std::string Message) const -> void;
		DURINED_API auto NotifyImported(std::string_view AssetPath) const -> void;
		DURINED_API auto NotifyImportedDirectory(std::string_view DirectoryPath) const -> void;
		DURINED_API auto NotifyImportStarted(
			AssetForge::FImportOperationHandle Handle, std::string_view Title) const -> void;
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

	class FMountedSourceImportFormModel
	{
	public:
		static constexpr size_t PathCapacity = 512;

		DURINED_API auto Reset() -> void;
		DURINED_API auto SuggestDestination(std::string_view SuggestedPath) -> void;
		DURINED_API auto SetDestination(std::string_view VirtualPath) -> bool;
		DURINED_API auto Inspect(std::string_view ReferencingPath,
			bool bEngineAuthoringContext = false) const -> FMountedSourceImportDiagnostic;
		DURINED_API auto DrawMode(std::string_view ExternalDescription) -> void;
		DURINED_API auto DrawSourceRow(const char* InputId, const char* Hint,
			float BrowseButtonWidth) -> bool;
		DURINED_API auto DrawDestinationRow(const char* InputId, const char* Hint,
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

	DURINED_API auto DrawImportDialogWarning(std::string_view Message) -> void;
} // namespace Durin::Editor::Import
