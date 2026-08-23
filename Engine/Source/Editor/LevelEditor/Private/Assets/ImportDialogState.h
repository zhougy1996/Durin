#pragma once

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "AsyncImport.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	enum class ETextureUsage : uint8;
	enum class EVolumeTextureSourceChannels : uint8;
}

namespace Durin::Editor::Level
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
		auto Begin(Asset::FAsyncImportPlanHandle InHandle) -> void;
		auto ApplySnapshot(Asset::FImportOperationSnapshot InSnapshot) -> void;
		auto Refresh() -> void;
		auto Reset() -> void;
		auto RequestCancel() -> bool;
		auto RunInBackground() -> bool;

		auto GetState() const -> EImportDialogOperationState;
		auto GetSnapshot() const -> const Asset::FImportOperationSnapshot& { return Snapshot; }
		auto HasOperation() const -> bool { return Handle.IsValid(); }
		auto CanCancel() const -> bool { return Snapshot.bCancelable && !Snapshot.IsTerminal(); }

	private:
		Asset::FAsyncImportPlanHandle Handle;
		Asset::FImportOperationSnapshot Snapshot;
	};

	// Routes import-dialog outcomes to the owning editor workspace.
	struct FImportDialogCallbacks
	{
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(std::string)> Imported;
		std::function<void(std::string)> ImportedDirectory;
		std::function<void(Asset::FAsyncImportPlanHandle, std::string)> ImportStarted;

		auto Clear() const -> void;
		auto Report(std::string Message) const -> void;
		auto NotifyImported(std::string_view AssetPath) const -> void;
		auto NotifyImportedDirectory(std::string_view DirectoryPath) const -> void;
		auto NotifyImportStarted(
			Asset::FAsyncImportPlanHandle Handle, std::string_view Title) const -> void;
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

	// Selects the concrete texture asset created by the unified import workflow.
	enum class ETextureImportAssetType : uint8
	{
		Texture2D,
		TextureCube,
		VolumeTexture
	};

	struct FTexture2DImportFormState
	{
		auto Reset() -> void;

		FMountedSourceImportFormModel Source;
		ETextureUsage Usage = static_cast<ETextureUsage>(0);
	};

	struct FVolumeTextureImportFormState
	{
		auto Reset() -> void;

		FMountedSourceImportFormModel Source;
		EVolumeTextureSourceChannels Channels =
			static_cast<EVolumeTextureSourceChannels>(0);
		uint32 SliceWidth = 128;
		uint32 SliceHeight = 128;
		uint32 Depth = 128;
		uint32 TilesX = 12;
		uint32 TilesY = 12;
	};

	struct FTextureCubeImportFormState
	{
		auto Reset() -> void;

		std::array<std::array<char, 512>, TextureCubeFaceCount> FacePathBuffers{};
		std::array<std::array<char, 512>, TextureCubeFaceCount> FaceDestinationBuffers{};
		std::array<std::string, TextureCubeFaceCount> LastSuggestedFaceDestinations;
		std::array<char, 512> PanoramaPathBuffer{};
		std::array<char, 512> PanoramaDestinationBuffer{};
		std::string LastSuggestedPanoramaDestination;
		std::string SourceValidationMessage;
		ETextureCubeSourceLayout SourceLayout =
			ETextureCubeSourceLayout::EquirectangularPanorama;
		uint32 PanoramaFaceDimension = 0;
		uint32 PanoramaCustomFaceDimension = 0;
		float PanoramaExposureEV = 0.0f;
		uint32 ValidatedSourceWidth = 0;
		uint32 ValidatedSourceHeight = 0;
		uint32 ValidatedDimension = 0;
		uint32 ValidatedMipCount = 0;
		EPixelFormat ValidatedPixelFormat = EPixelFormat::Unknown;
		bool bValidatedHDR = false;
		bool bSourcesValid = false;
	};

	// Owns reset and inactive-form retention for one unified texture import modal.
	class FTextureImportDialogState
	{
	public:
		auto Reset() -> void;

		auto GetAssetType() const -> ETextureImportAssetType { return AssetType; }
		auto SetAssetType(ETextureImportAssetType InAssetType) -> void
		{
			AssetType = InAssetType;
		}
		auto GetSourceMode() const -> EMountedSourceImportMode { return SourceMode; }
		auto SetSourceMode(EMountedSourceImportMode InSourceMode) -> void
		{
			SourceMode = InSourceMode;
		}
		auto GetTexture2D() -> FTexture2DImportFormState& { return Texture2D; }
		auto GetTexture2D() const -> const FTexture2DImportFormState& { return Texture2D; }
		auto GetTextureCube() -> FTextureCubeImportFormState& { return TextureCube; }
		auto GetTextureCube() const -> const FTextureCubeImportFormState& { return TextureCube; }
		auto GetVolumeTexture() -> FVolumeTextureImportFormState& { return VolumeTexture; }
		auto GetVolumeTexture() const -> const FVolumeTextureImportFormState& { return VolumeTexture; }

	private:
		ETextureImportAssetType AssetType = ETextureImportAssetType::Texture2D;
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
		FTexture2DImportFormState Texture2D;
		FTextureCubeImportFormState TextureCube;
		FVolumeTextureImportFormState VolumeTexture;
	};

	auto DrawImportDialogWarning(std::string_view Message) -> void;
} // namespace Durin::Editor::Level
