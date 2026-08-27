#pragma once

#include "Asset/AssetImportData.h"
#include "Asset/Cook.h"
#include "Asset/EditorBulkData.h"
#include "Asset/SourcePath.h"
#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "DObject/ObjectPtr.h"
#include "Materials/MeshMaterialSlot.h"
#include "RenderingThread.h"

#include "StaticMesh.gen.h"

namespace Durin
{
	inline constexpr FGuid StaticMeshImportedGeometryPayloadId{
		0x442898cd, 0x801d49ed, 0x93459533, 0x4531fc1d};
	inline constexpr uint32 StaticMeshImportedDataSchemaVersion = 1;
	inline constexpr uint64 MaximumStaticMeshImportedDataBytes =
		1024ull * 1024ull * 1024ull;
	inline constexpr uint32 MaximumStaticMeshImportedUVChannels = 4;

	namespace Asset { class FStaticMeshBuildOperations; }
	class DBodySetup;
	class FCollisionGeometryRef;
	enum class EBodySetupCollisionSourceMode : uint8;
	enum class EBodySetupCollisionQueryPolicy : uint8;
	enum class EBodySetupCollisionBuildStatus : uint8;
	enum class ECollisionGeometryKind : uint8;

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

	// Carries source identity and settings transiently through mesh build operations.
	struct FStaticMeshSourceImportData
	{
		// Empty means the transient build input has no source dependency.
		FSourcePath SourcePath;

		// Lowercase XXH3-128 of the exact source bytes.
		std::string SourceContentHash;
		std::string ImporterId;
		uint32 ImporterVersion = 0;
		FStaticMeshImportSettings ImportSettings;

		auto HasSource() const -> bool { return !SourcePath.IsEmpty(); }
		auto operator==(const FStaticMeshSourceImportData&) const -> bool = default;
	};

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

	// Owns canonical imported geometry and material-source mapping in authored bulk.
	DSTRUCT()
	struct FStaticMeshImportedData
	{
		GENERATED_BODY()

		DPROPERTY()
		Asset::FEditorBulkData Geometry;

		DPROPERTY()
		uint32 MaterialSlotCount = 0;

		DPROPERTY()
		uint32 MeshCount = 0;

		DPROPERTY()
		uint32 SchemaVersion = StaticMeshImportedDataSchemaVersion;

		std::vector<FStaticMeshImportedMaterialSlot> MaterialSlots;
		std::vector<FStaticMeshImportedMesh> Meshes;

		ENGINE_API auto CaptureDecodedData(std::string& OutError) -> bool;
		ENGINE_API auto Decode(std::string& OutError) const -> FStaticMeshImportedData;
		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
	};

	struct FStaticMeshBuildData;
	struct FStaticMeshRenderData;
	struct FStaticMeshImportResult;
	struct FStaticMeshBuildProduct;
	class FStaticMeshImportedStateExchange;

	enum class EStaticMeshDerivedDataStatus : uint8
	{
		None,
		Hit,
		Missing,
		Corrupt,
		Incompatible,
		Rebuilt,
		WriteFailure,
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

	// Bounded value-only collision facts for diagnostics and the read-only Inspector.
	struct FStaticMeshCollisionInspection
	{
		EBodySetupCollisionSourceMode Mode;
		EBodySetupCollisionQueryPolicy Policy;
		EBodySetupCollisionBuildStatus BuildStatus;
		ECollisionGeometryKind GeometryKind;
		bool bHasGeometry = false;
		uint32 SourceTriangles = 0;
		uint32 RetainedTriangles = 0;
		uint32 RemovedTriangles = 0;
		uint32 Nodes = 0;
		std::optional<FBox> Bounds;
		uint64 PayloadBytes = 0;
		uint64 RuntimeBytes = 0;
		uint32 BuilderVersion = 0;
		uint32 SchemaVersion = 0;
		uint64 BuildRevision = 0;
		bool bRevisionCoherent = false;
		std::string CacheKey;
		std::string Diagnostic;
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
		ENGINE_API auto GetBodySetup() const -> DBodySetup*;
		ENGINE_API auto SetBodySetup(DBodySetup* InBodySetup) -> bool;
		ENGINE_API auto SetCollisionSourceMode(
			EBodySetupCollisionSourceMode Mode,
			std::string& OutError) -> bool;
		ENGINE_API auto SetCollisionQueryPolicy(
			EBodySetupCollisionQueryPolicy Policy,
			std::string& OutError) -> bool;
		ENGINE_API auto RebuildCollision(std::string& OutError) -> bool;
		// Creates the qualified built-in Box setup from verified CPU bounds; arbitrary meshes remain collision-free.
		ENGINE_API auto EnsureQualifiedBoxBodySetup() -> DBodySetup*;
		ENGINE_API auto InitResources() -> void;
		auto GetImportedSource() const -> const AssetImport::FSourceFile*
		{
			return AssetImportData
				? AssetImportData->GetSourceData().FindByRole("source") : nullptr;
		}
		auto GetSourceFile() const -> const std::string&
		{
			if (const AssetImport::FSourceFile* Source = GetImportedSource())
				return Source->Hint;
			static const std::string Empty;
			return Empty;
		}
		auto GetSourceHintBase() const -> AssetImport::ESourceHintBase
		{
			if (const AssetImport::FSourceFile* Source = GetImportedSource())
				return Source->HintBase;
			return AssetImport::ESourceHintBase::AssetRelative;
		}
		auto GetAssetImportData() const -> const AssetImport::DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> AssetImport::DAssetImportData*
		{
			return AssetImportData.Get();
		}
		ENGINE_API auto PublishAssetImportData(
			AssetImport::DAssetImportData& Value, std::string& OutError) -> bool;
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FMeshMaterialSlotDefinition> { return MaterialSlots; }
		ENGINE_API auto GetMaterialSlot(uint32 SlotIndex) const -> const FMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(FName Name) const -> const FMeshMaterialSlotDefinition*;
		ENGINE_API auto GetMaterialIndex(FName Name) const -> std::optional<uint32>;
		ENGINE_API auto RenameMaterialSlot(uint32 SlotIndex, FName Name, std::string& OutError) -> bool;

		ENGINE_API auto InspectCollision() const -> FStaticMeshCollisionInspection;
		auto GetDerivedDataDiagnostic() const -> const FStaticMeshDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetImportedData() const -> const FStaticMeshImportedData& { return ImportedData; }
		auto GetImportedDataIdentity() const -> FXxHash128 { return ImportedData.GetIdentity(); }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		// Contributes deterministic DMSH data and descriptor-bearing runtime metadata to a cook.
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;

		ENGINE_API static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		// Seeds a detached candidate with only the slot state required by the
		// conservative reimport reconciliation algorithm.
		ENGINE_API auto SeedMaterialReconciliationFrom(
			const DStaticMesh& Previous) -> void;
		ENGINE_API auto PublishImportedProduct(
			FStaticMeshBuildProduct Product,
			std::string& OutError) -> bool;
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
		auto PublishRenderData(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
			bool bSlotMetadataChanged,
			std::string& OutError) -> bool;
		auto CommitRenderDataCandidate(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition>*
				InMaterialSlots,
			std::string& OutError,
			bool bBuildAuthoredCollision = true) -> bool;
		auto LoadCookedRenderData(std::string& OutError) -> bool;
		auto RefreshQualifiedBoxBodySetup() -> void;
		auto BuildCollisionCandidate(
			const FStaticMeshRenderData& SourceRenderData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FCollisionGeometryRef& OutSimple,
			FCollisionGeometryRef& OutComplex,
			EBodySetupCollisionBuildStatus& OutStatus,
			std::string& OutKey,
			std::string& OutDiagnostic,
			uint64& OutPayloadBytes,
			std::string& OutError) const -> bool;

		DPROPERTY(EditorOnly)
		TObjectPtr<AssetImport::DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FStaticMeshImportedData ImportedData;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		DPROPERTY()
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		DPROPERTY()
		TObjectPtr<DBodySetup> BodySetup;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		FStaticMeshDerivedDataDiagnostic DerivedDataDiagnostic;
		FRenderCommandFence ReleaseResourcesFence;
		std::atomic<uint64> RenderResourceStatus{PackRenderResourceStatus(
			EStaticMeshRenderResourceState::Uninitialized, 1)};

		friend class FStaticMeshImportedStateExchange;
		friend class Asset::FStaticMeshBuildOperations;
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

namespace Durin::Asset
{
	inline constexpr uint32 MaximumStaticMeshImportedUVChannels =
		::Durin::MaximumStaticMeshImportedUVChannels;
	using FStaticMeshImportedMaterialSlot = ::Durin::FStaticMeshImportedMaterialSlot;
	using FStaticMeshImportedMesh = ::Durin::FStaticMeshImportedMesh;
	using FStaticMeshImportedData = ::Durin::FStaticMeshImportedData;
}
