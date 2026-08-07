#pragma once

#include "Asset/SourcePath.h"
#include "EngineAPI.h"
#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "RenderingThread.h"

#include "StaticMesh.gen.h"

namespace Durin
{
	class DMaterialInterface;

	// Reports only the semantic render-resource states required by nonblocking consumers.
	enum class EStaticMeshRenderResourceReadiness : uint8
	{
		Unavailable,
		Queued,
		Ready,
		Failed
	};

	// Pairs readiness with the asset-local revision that must still match before publication.
	// Revisions are never transferred between assets or reused: construction starts unavailable at
	// a non-zero revision, and every accepted CPU-data publication, initialization result, imported-
	// state exchange, resource invalidation, release, or destruction boundary advances it.
	struct FStaticMeshRenderResourceStatus
	{
		EStaticMeshRenderResourceReadiness Readiness =
			EStaticMeshRenderResourceReadiness::Unavailable;
		uint64 Revision = 0;

		auto IsReady() const -> bool
		{
			return Readiness == EStaticMeshRenderResourceReadiness::Ready
				&& Revision != 0;
		}
	};

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

	inline constexpr uint32 MaximumStaticMeshImportedUVChannels = 4;

	struct FStaticMeshImportedMaterialSlot
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
		std::string SourceName;
	};

	struct FStaticMeshImportedMesh
	{
		std::string Name;
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaximumStaticMeshImportedUVChannels> UVChannels;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		uint32 SourceMaterialIndex = 0;
	};

	struct FStaticMeshImportedData
	{
		std::vector<FStaticMeshImportedMaterialSlot> MaterialSlots;
		std::vector<FStaticMeshImportedMesh> Meshes;
	};

	using FStaticMeshSourceDecodeFunction = bool (*)(
		std::string_view FilePath,
		const FStaticMeshImportSettings& Settings,
		FStaticMeshImportedData& OutData,
		std::string& OutError);

	// Editor import modules register concrete source decoders without adding a
	// reverse Runtime Engine dependency on those modules or their third parties.
	ENGINE_API auto RegisterStaticMeshSourceDecoder(
		FStaticMeshSourceDecodeFunction Decoder) -> bool;
	ENGINE_API auto UnregisterStaticMeshSourceDecoder(
		FStaticMeshSourceDecodeFunction Decoder) -> void;

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
	class FStaticMeshImportedStateExchange;

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

	// Preserves one stable positional material slot and its source-import provenance.
	DSTRUCT()
	struct FStaticMeshMaterialSlotDefinition
	{
		GENERATED_BODY()

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

	// Owns imported mesh metadata, material slots, and rebuilt render resources.
	DCLASS()
	class DStaticMesh : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DStaticMesh(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DStaticMesh() override;
		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		// Returns one coherent, nonblocking snapshot for stale-work rejection.
		ENGINE_API auto GetRenderResourceStatus() const
			-> FStaticMeshRenderResourceStatus;
		// Returns a read-only copy only when CPU LOD 0 data has finite, non-degenerate bounds.
		ENGINE_API auto GetLOD0LocalBounds() const -> std::optional<FBox>;
		ENGINE_API auto InitResources() -> void;
		auto GetSourceFile() const -> const std::string& { return SourceImportData.SourcePath.Path; }
		auto GetImportSettings() const -> const FStaticMeshImportSettings& { return SourceImportData.ImportSettings; }
		auto GetSourceImportData() const -> const FStaticMeshSourceImportData& { return SourceImportData; }
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FStaticMeshMaterialSlotDefinition> { return MaterialSlots; }
		ENGINE_API auto GetMaterialSlot(uint32 SlotIndex) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(FName Name) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto GetMaterialIndex(FName Name) const -> std::optional<uint32>;
		ENGINE_API auto RenameMaterialSlot(uint32 SlotIndex, FName Name, std::string& OutError) -> bool;

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
			std::string_view SourceDestination = {},
			bool bEngineAuthoringContext = false) -> FStaticMeshImportResult;
		ENGINE_API auto InitializeFromImportedData(
			const FStaticMeshImportedData& ImportedData,
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
		// Transactionally applies a detached import candidate while preserving
		// this asset's package identity and component overrides. The displaced
		// CPU data is left on Other for symmetric bundle rollback; resource
		// state and destruction fences never move between assets.
		ENGINE_API auto ExchangeImportedState(
			DStaticMesh& Other,
			std::string& OutError) -> bool;
		// Performs all failable render-resource work up front. The returned token
		// commits and reverses the complete imported state without failure.
		ENGINE_API auto PrepareImportedStateExchange(
			DStaticMesh& Candidate,
			std::string& OutError) -> std::unique_ptr<FStaticMeshImportedStateExchange>;
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

		static constexpr uint64 RenderResourceStateBits = 3;
		static constexpr uint64 RenderResourceStateMask =
			(1ull << RenderResourceStateBits) - 1;
		static constexpr auto PackRenderResourceStatus(
			EStaticMeshRenderResourceState State,
			uint64 Revision) -> uint64
		{
			return (Revision << RenderResourceStateBits)
				| static_cast<uint64>(State);
		}
		static constexpr auto UnpackRenderResourceState(uint64 Packed)
			-> EStaticMeshRenderResourceState
		{
			return static_cast<EStaticMeshRenderResourceState>(
				Packed & RenderResourceStateMask);
		}
		static constexpr auto UnpackRenderResourceRevision(uint64 Packed)
			-> uint64
		{
			return Packed >> RenderResourceStateBits;
		}
		auto LoadRenderResourceState() const
			-> EStaticMeshRenderResourceState;
		auto PublishRenderResourceState(EStaticMeshRenderResourceState State)
			-> void;
		auto TryPublishRenderResourceState(
			EStaticMeshRenderResourceState Expected,
			EStaticMeshRenderResourceState State) -> bool;
		auto AdvanceRenderResourceRevision() -> void;
		auto ReleaseResources() -> void;
		auto BuildRenderData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto BuildRenderDataCandidate(
			std::string_view PhysicalFilePath,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			std::vector<FStaticMeshMaterialSlotDefinition>& OutMaterialSlots,
			bool& bOutSlotMetadataChanged,
			std::string& OutError) -> bool;
		auto BuildRenderDataCandidate(
			const FStaticMeshImportedData& ImportedData,
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
		FStaticMeshSourceImportData SourceImportData;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		DPROPERTY()
		std::vector<FStaticMeshMaterialSlotDefinition> MaterialSlots;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		FStaticMeshDerivedDataDiagnostic DerivedDataDiagnostic;
		FRenderCommandFence ReleaseResourcesFence;
		std::atomic<uint64> RenderResourceStatus{PackRenderResourceStatus(
			EStaticMeshRenderResourceState::Uninitialized, 1)};

		friend class FStaticMeshImportedStateExchange;
	};

	class ENGINE_API FStaticMeshImportedStateExchange
	{
	public:
		~FStaticMeshImportedStateExchange();
		FStaticMeshImportedStateExchange(const FStaticMeshImportedStateExchange&) = delete;
		auto operator=(const FStaticMeshImportedStateExchange&)
			-> FStaticMeshImportedStateExchange& = delete;

		auto Commit() noexcept -> void;
		auto Reverse() noexcept -> void;
		auto Finalize() noexcept -> void;

	private:
		FStaticMeshImportedStateExchange(DStaticMesh& InTarget, DStaticMesh& InCandidate);
		auto Swap() noexcept -> void;

		DStaticMesh* Target = nullptr;
		DStaticMesh* Candidate = nullptr;
		bool bCommitted = false;

		friend class DStaticMesh;
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
