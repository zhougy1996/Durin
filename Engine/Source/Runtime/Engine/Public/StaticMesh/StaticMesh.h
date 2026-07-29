#pragma once

#include "Asset/SourcePath.h"
#include "EngineAPI.h"
#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "RenderingThread.h"

#include "StaticMesh.gen.h"

namespace Durin
{
	inline constexpr uint32 StaticModelImportManifestVersion = 1;
	inline constexpr uint32 StaticModelMaterialMapperVersion = 1;

	class DMaterialInterface;
	class DMaterialInstance;
	class DTexture2D;
	namespace Asset
	{
		struct FImportedSceneData;
	}

	// Selects a signed source axis when converting imported geometry to Durin space.
	DENUM()
	enum class EStaticMeshImportAxis : int8
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ
	};

	// Defines the orthogonal source basis used during static-mesh import.
	DSTRUCT()
	struct FStaticMeshImportSettings
	{
		GENERATED_BODY()

		DPROPERTY()
		EStaticMeshImportAxis ForwardAxis = EStaticMeshImportAxis::PositiveX;

		DPROPERTY()
		EStaticMeshImportAxis RightAxis = EStaticMeshImportAxis::PositiveY;

		DPROPERTY()
		EStaticMeshImportAxis UpAxis = EStaticMeshImportAxis::PositiveZ;

		ENGINE_API auto IsValid(std::string* OutError = nullptr) const -> bool;

		ENGINE_API static auto MakeDurin() -> FStaticMeshImportSettings;
		ENGINE_API static auto MakeYUpNegativeZForward() -> FStaticMeshImportSettings;

		auto operator==(const FStaticMeshImportSettings&) const -> bool = default;
	};

	// Stores optional portable source provenance used only for editor rebuild and reimport.
	DSTRUCT()
	struct FStaticMeshSourceImportData
	{
		GENERATED_BODY()

		// Empty means the asset has no source dependency; otherwise this is a complete mounted source path.
		DPROPERTY()
		FSourcePath SourcePath;

		// Lowercase XXH3-128 of the exact source bytes.
		DPROPERTY()
		std::string SourceContentHash;

		DPROPERTY()
		std::string ImporterId;

		DPROPERTY()
		uint32 ImporterVersion = 0;

		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;

		auto HasSource() const -> bool { return !SourcePath.IsEmpty(); }
		auto operator==(const FStaticMeshSourceImportData&) const -> bool = default;
	};

	struct FStaticMeshBuildData;
	struct FStaticMeshRenderData;
	struct FStaticMeshImportResult;

	enum class EStaticMeshSourceStatus : uint8
	{
		NoSource,
		Available,
		Changed,
		Missing,
		Invalid
	};

	// Describes editor-facing source availability without making source data a runtime requirement.
	struct FStaticMeshSourceDiagnostic
	{
		EStaticMeshSourceStatus Status = EStaticMeshSourceStatus::NoSource;
		std::string ResolvedPath;
		std::string Message;

		auto IsAvailable() const -> bool
		{
			return Status == EStaticMeshSourceStatus::Available
				|| Status == EStaticMeshSourceStatus::Changed;
		}
	};

	enum class EStaticMeshDerivedDataStatus : uint8
	{
		None,
		Hit,
		Missing,
		Corrupt,
		Incompatible,
		Rebuilt,
		WriteFailure,
		SourceUnavailableCached,
		SourceUnavailable,
		CookedLoaded,
		CookedFailure
	};

	// Describes the most recent native-payload cache decision for editor diagnostics.
	struct FStaticMeshDerivedDataDiagnostic
	{
		EStaticMeshDerivedDataStatus Status = EStaticMeshDerivedDataStatus::None;
		std::string Key;
		std::string Message;
		bool bSourceImporterInvoked = false;
	};

	// Preserves one material slot's stable identity and source-import provenance.
	DSTRUCT()
	struct FStaticMeshMaterialSlotDefinition
	{
		GENERATED_BODY()

		// Stable identity used to retain component overrides across reimport and reordering.
		DPROPERTY()
		FGuid SlotId;

		DPROPERTY()
		FName Name;

		DPROPERTY()
		std::string SourceName;

		// Original importer index used only for source reconciliation.
		DPROPERTY()
		uint32 SourceMaterialIndex = 0;

		DPROPERTY()
		TObjectPtr<DMaterialInterface> DefaultMaterial;
	};

	DSTRUCT()
	struct FStaticModelImportDependencyRecord
	{
		GENERATED_BODY()

		DPROPERTY()
		uint8 Role = 0;

		DPROPERTY()
		std::string StableIdentity;

		DPROPERTY()
		FSourcePath SourcePath;

		DPROPERTY()
		std::string ContentHash;

		DPROPERTY()
		uint64 ByteCount = 0;
	};

	DSTRUCT()
	struct FStaticModelImportMaterialRecord
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid SlotId;

		DPROPERTY()
		uint32 SourceMaterialIndex = 0;

		DPROPERTY()
		std::string SourceName;

		DPROPERTY()
		FVector4 BaseColorFactor{1.0};

		DPROPERTY()
		FAssetPath GeneratedMaterialPath;

		DPROPERTY()
		bool bImporterManaged = true;

		DPROPERTY()
		TObjectPtr<DMaterialInstance> GeneratedMaterial;
	};

	DSTRUCT()
	struct FStaticModelImportTextureRecord
	{
		GENERATED_BODY()

		DPROPERTY()
		std::string StableIdentity;

		DPROPERTY()
		uint8 Semantic = 0;

		DPROPERTY()
		FAssetPath GeneratedTexturePath;

		DPROPERTY()
		TObjectPtr<DTexture2D> GeneratedTexture;
	};

	DSTRUCT()
	struct FStaticModelImportManifest
	{
		GENERATED_BODY()

		DPROPERTY()
		uint32 Version = 0;

		DPROPERTY()
		std::string DependencyFingerprint;

		DPROPERTY()
		uint32 ImporterVersion = 0;

		DPROPERTY()
		uint32 MaterialMapperVersion = 0;

		DPROPERTY()
		std::vector<FStaticModelImportDependencyRecord> Dependencies;

		DPROPERTY()
		std::vector<FStaticModelImportMaterialRecord> Materials;

		DPROPERTY()
		std::vector<FStaticModelImportTextureRecord> Textures;

		DPROPERTY()
		std::vector<std::string> Warnings;

		auto IsValid() const -> bool
		{
			return Version > 0 && !DependencyFingerprint.empty()
				&& ImporterVersion > 0 && MaterialMapperVersion > 0;
		}
	};

	// Owns imported mesh metadata, material slots, and rebuilt render resources.
	DCLASS()
	class DStaticMesh : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DStaticMesh(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DStaticMesh() override;
		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		ENGINE_API auto InitResources() -> void;
		auto GetSourceFile() const -> const std::string& { return SourceImportData.SourcePath.Path; }
		auto GetImportSettings() const -> const FStaticMeshImportSettings& { return SourceImportData.ImportSettings; }
		auto GetSourceImportData() const -> const FStaticMeshSourceImportData& { return SourceImportData; }
		auto GetImportManifest() const -> const FStaticModelImportManifest& { return ImportManifest; }
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FStaticMeshMaterialSlotDefinition> { return MaterialSlots; }
		ENGINE_API auto GetMaterialSlot(uint32 SlotIndex) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(const FGuid& SlotId) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(FName Name) const -> const FStaticMeshMaterialSlotDefinition*;

		ENGINE_API auto InspectSource() const -> FStaticMeshSourceDiagnostic;
		auto GetDerivedDataDiagnostic() const -> const FStaticMeshDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		ENGINE_API auto ChangeSourceReference(
			std::string_view SourceVirtualPath, std::string& OutError) -> bool;
		ENGINE_API auto IngestAndChangeSource(
			std::string_view FilePath,
			std::string_view TargetSourceVirtualPath,
			std::string& OutError) -> bool;
		ENGINE_API auto RepairSourcePath(std::string_view FilePath, std::string& OutError) -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		// Contributes deterministic DMSH data and descriptor-bearing runtime metadata to a cook.
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError,
			bool bRetainDiagnosticSourceMetadata = false) -> bool;

		ENGINE_API static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		// Creates unpackaged geometry for tests and runtime-generated content; editor previews use retained assets.
		ENGINE_API static auto CreateTransientFromFile(
			std::string_view FilePath,
			DObject* Outer,
			std::string_view ObjectName,
			std::string& OutError,
			const FStaticMeshImportSettings& InImportSettings = {}
		) -> DStaticMesh*;
		ENGINE_API static auto ImportAsset(
			std::string_view FilePath,
			std::string_view AssetPath,
			const FStaticMeshImportSettings& InImportSettings = {},
			std::string_view SourceDestination = {}) -> FStaticMeshImportResult;
		ENGINE_API auto InitializeFromImportedScene(
			const Asset::FImportedSceneData& ImportedScene,
			const FStaticMeshSourceImportData& InSourceImportData,
			std::string_view SourceLabel,
			std::string& OutError) -> bool;
		// Seeds a detached candidate with only the slot state required by the
		// conservative reimport reconciliation algorithm.
		ENGINE_API auto SeedMaterialReconciliationFrom(
			const DStaticMesh& Previous) -> void;
		ENGINE_API auto SetImportedDefaultMaterial(
			uint32 SourceMaterialIndex,
			DMaterialInterface* Material,
			std::string& OutError) -> bool;
		ENGINE_API auto SetImportManifest(
			FStaticModelImportManifest InManifest,
			std::string& OutError) -> bool;
		// Transactionally applies a detached import candidate while preserving
		// this asset's package identity and component overrides. The displaced
		// CPU data is left on Other for symmetric bundle rollback; resource
		// state and destruction fences never move between assets.
		ENGINE_API auto ExchangeImportedState(
			DStaticMesh& Other,
			std::string& OutError) -> bool;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto FinishDestroy() -> void override;

	private:
		enum class EStaticMeshRenderResourceState : uint8
		{
			Uninitialized,
			InitializationQueued,
			Ready,
			Failed,
			ReleaseQueued,
			Released
		};

		auto ReleaseResources() -> void;
		auto BuildRenderData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto BuildRenderDataCandidate(
			std::string_view PhysicalFilePath,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			std::vector<FStaticMeshMaterialSlotDefinition>& OutMaterialSlots,
			bool& bOutSlotMetadataChanged,
			std::string& OutError) -> bool;
		auto BuildRenderDataCandidate(
			const Asset::FImportedSceneData& ImportedScene,
			std::string_view SourceLabel,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			std::vector<FStaticMeshMaterialSlotDefinition>& OutMaterialSlots,
			bool& bOutSlotMetadataChanged,
			std::string& OutError) -> bool;
		auto PublishRenderData(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FStaticMeshMaterialSlotDefinition> InMaterialSlots,
			bool bSlotMetadataChanged,
			std::string& OutError) -> bool;
		auto CommitRenderDataCandidate(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FStaticMeshMaterialSlotDefinition>*
				InMaterialSlots,
			std::string& OutError) -> bool;
		auto LoadCookedRenderData(std::string& OutError) -> bool;

		DPROPERTY()
		std::string SourceFile;

		DPROPERTY()
		FStaticMeshSourceImportData SourceImportData;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		// Retained only so packages with the removed legacy schema can be diagnosed
		// and rejected without losing serialized field compatibility.
		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;

		DPROPERTY()
		uint32 MaterialSlotsVersion = 0;

		DPROPERTY()
		std::vector<FStaticMeshMaterialSlotDefinition> MaterialSlots;

		DPROPERTY()
		FStaticModelImportManifest ImportManifest;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		FStaticMeshDerivedDataDiagnostic DerivedDataDiagnostic;
		FRenderCommandFence ReleaseResourcesFence;
		std::atomic<EStaticMeshRenderResourceState> RenderResourceState{
			EStaticMeshRenderResourceState::Uninitialized};
	};

	// Reports static-mesh import success and the created asset, when available.
	struct FStaticMeshImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DStaticMesh* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}
