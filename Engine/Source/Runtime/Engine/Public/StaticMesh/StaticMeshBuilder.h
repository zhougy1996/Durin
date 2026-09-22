#pragma once

#include <expected>

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "EngineAPI.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	struct FStaticMeshAuthoredBuildError;

	// Immutable object facts captured before StaticMesh recipe work begins.
	struct FStaticMeshReconciliationSnapshot
	{
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		FXxHash128 SourceIdentity;
		FObjectKey Body;
		uint64 BodyRevision = 0;
		EBodySetupCollisionSourceMode CollisionMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy CollisionPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
	};

	// Detached Engine request; cache policy is not forwarded to recipe code.
	struct FStaticMeshBuildRequest
	{
		FStaticMeshReconciliationSnapshot Reconciliation;
		FStaticMeshSource Source;
		bool bPersistDerivedData = true;
	};

	enum class EStaticMeshBuildOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	enum class EStaticMeshCacheOperation : uint8 { Read, Decode, Write };
	// Nonfatal cache failure. Clean hits/misses create no error record.
	struct FStaticMeshCacheError
	{
		FStaticMeshCacheError(EStaticMeshRecipeKind InKind, EStaticMeshCacheOperation InOperation, std::string InMessage)
			: Kind(InKind), Operation(InOperation), Message(InMessage.substr(0, 960)) {}
		EStaticMeshRecipeKind Kind;
		EStaticMeshCacheOperation Operation;
		ENGINE_API auto ToString() const -> std::string;
	private:
		std::string Message;
	};

	// Bounded value-only observation, without payload ownership or backend paths.
	struct FStaticMeshBuildObservation
	{
		EStaticMeshBuildOrigin Origin = EStaticMeshBuildOrigin::Rebuilt;
		FCacheKeyProxy DerivedDataKey;
	};

	// Render product exposes owned geometry and reconciled material data only.
	struct FStaticMeshBuildProduct
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		auto GetObservation() const -> const FStaticMeshBuildObservation& { return Observation; }
		auto GetCacheErrors() const -> const std::vector<FStaticMeshCacheError>& { return CacheErrors; }
	private:
		FStaticMeshBuildObservation Observation;
		std::vector<FStaticMeshCacheError> CacheErrors;
		bool bSlotMetadataChanged = false;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 ProviderRegistration = 0;
		friend class FStaticMeshBuilder;
		friend class FStaticMeshAuthoredCandidate;
	};

	struct FStaticMeshCollisionBuildProduct
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		auto GetObservation() const -> const FStaticMeshBuildObservation& { return Observation; }
		auto GetCacheErrors() const -> const std::vector<FStaticMeshCacheError>& { return CacheErrors; }
	private:
		FStaticMeshBuildObservation Observation;
		std::vector<FStaticMeshCacheError> CacheErrors;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 ProviderRegistration = 0;
		friend class FStaticMeshBuilder;
		friend class FStaticMeshAuthoredCandidate;
	};

	// Value-only worker input; material object bindings remain in the owner-thread snapshot.
	struct FStaticMeshAuthoredBuildRequest
	{
		FStaticMeshSource Source;
		std::vector<FStaticMeshRecipeMaterialSlot> MaterialSlots;
		float NormalizedSize = 1.5f;
		EBodySetupCollisionSourceMode CollisionMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy CollisionPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		bool bPersistDerivedData = true;
	};

	// Sealed CPU product: only the builder can create one, and only owner-thread application consumes it.
	class FStaticMeshAuthoredCandidate
	{
	public:
		auto GetRenderObservation() const -> FStaticMeshBuildObservation
		{ return Render.Observation; }
		auto GetCollisionObservation() const -> FStaticMeshBuildObservation
		{ return Collision.Observation; }
		auto GetCacheErrors() const -> std::vector<FStaticMeshCacheError>
		{
			auto Errors = Render.CacheErrors;
			Errors.insert(Errors.end(), Collision.CacheErrors.begin(), Collision.CacheErrors.end());
			return Errors;
		}

		auto GetRenderData() const -> const FStaticMeshRenderData* { return Render.RenderData.get(); }
		auto GetCollision() const -> const FStaticMeshCollisionBuildProduct& { return Collision; }
		auto GetProviderRegistration() const -> uint64 { return Render.ProviderRegistration; }
		auto GetSourceIdentity() const -> FXxHash128 { return Request.Source.GetIdentity(); }

	private:
		FStaticMeshAuthoredCandidate() = default;
		FStaticMeshAuthoredBuildRequest Request;
		FStaticMeshBuildProduct Render;
		FStaticMeshCollisionBuildProduct Collision;
		friend class DStaticMesh;
		friend class FStaticMeshBuilder;
	};

	enum class EStaticMeshDerivedDataError : uint8
	{
		Cancelled, Unavailable, ProviderDescriptor, ProviderInvocation,
		Key, Source, Recipe, Payload, MissingLOD, InvalidGeometry, InvalidProduct
	};
	struct FStaticMeshDerivedDataError
	{
		EStaticMeshDerivedDataError Code;
		std::string Message;
		ENGINE_API auto ToString() const -> std::string;
	};

	enum class EStaticMeshPublicationError : uint8
	{
		None, MissingRenderData, LODPolicy, CollisionBuild, ResourceInitialization
	};
	struct FStaticMeshPublicationError
	{
		EStaticMeshPublicationError Code = EStaticMeshPublicationError::None;
		std::optional<FStaticMeshLODPolicyError> LODCause;
		std::optional<FStaticMeshDerivedDataError> CollisionCause;
	};

	ENGINE_API auto FormatStaticMeshPublicationError(const FStaticMeshPublicationError& Error) -> std::string;

	enum class EStaticMeshApplicationError : uint8
	{
		None, Cancelled, InvalidCandidate, ImportAllocation, ImportOwnership, ImportValidation,
		OwnerChanged, CandidateMismatch, MaterialBindings, Publication
	};
	struct FStaticMeshApplicationState
	{
		FXxHash128 SourceIdentity;
		float NormalizedSize = 0;
		FObjectKey Body;
		uint64 BodyRevision = 0;
		EBodySetupCollisionSourceMode CollisionMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy CollisionPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		uint64 SlotCount = 0;
	};
	struct FStaticMeshApplicationSlot
	{
		std::string Name;
		std::string SourceName;
		uint32 SourceMaterialIndex = 0;
		FObjectKey DefaultMaterial;
	};
	struct FStaticMeshApplicationError
	{
		EStaticMeshApplicationError Code = EStaticMeshApplicationError::None;
		FObjectKey Owner;
		FObjectKey ImportData;
		FObjectKey ImportOuter;
		std::string ImportClass;
		bool OwnerValid = false;
		bool CandidatePresent = false;
		bool RenderDataPresent = false;
		uint64 SlotIndex = 0;
		std::optional<FStaticMeshApplicationState> Expected;
		std::optional<FStaticMeshApplicationState> Current;
		std::optional<FStaticMeshApplicationState> Input;
		std::optional<FStaticMeshApplicationSlot> ExpectedSlot;
		std::optional<FStaticMeshApplicationSlot> CurrentSlot;
		std::optional<FStaticMeshApplicationSlot> InputSlot;
		std::optional<FAssetImportDataError> ImportCause;
		std::optional<FStaticMeshPublicationError> PublicationCause;
	};

	ENGINE_API auto FormatStaticMeshApplicationError(const FStaticMeshApplicationError& Error) -> std::string;

	enum class EStaticMeshAuthoredBuildError : uint8
	{
		NotStarted, Cancelled, Input, SourceBudget, RenderBuild, MaterialSlots,
		SlotName, UVChannels, MetadataBudget, FinalizationBudget, Payload, LODPolicy,
		CollisionBuild, ProviderChanged, RetainedBudget, WorkerException, TaskRetired
	};
	// The orchestration boundary exposes its own codes, not the lower-level error tree.
	// Message owns bounded diagnostic text; callers must not parse it for control flow.
	struct FStaticMeshAuthoredBuildError
	{
		EStaticMeshAuthoredBuildError Code = EStaticMeshAuthoredBuildError::NotStarted;
		std::string Message;
		ENGINE_API auto ToString() const -> std::string;
	};

	// Advanced detached building API. Ordinary asset callers use DStaticMesh::Build/AsyncBuild.
	class FStaticMeshBuilder
	{
	public:
		ENGINE_API static auto Build(
			FStaticMeshBuildRequest Request,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<FStaticMeshBuildProduct, FStaticMeshDerivedDataError>;
		ENGINE_API static auto BuildCollision(
			const FStaticMeshRenderData& RenderData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			bool bPersistDerivedData = true,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<FStaticMeshCollisionBuildProduct, FStaticMeshDerivedDataError>;
		ENGINE_API static auto BuildCandidate(FStaticMeshAuthoredBuildRequest Request,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<std::unique_ptr<FStaticMeshAuthoredCandidate>, FStaticMeshAuthoredBuildError>;
		ENGINE_API static auto ApplyCandidate(DStaticMesh& Mesh,
			std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate,
			const FStaticMeshReconciliationSnapshot& Snapshot,
			bool bMarkPackageDirty = true, const FStaticMeshBuildExecutionControl& Control = {},
			DAssetImportData* PreparedImportData = nullptr) -> std::expected<void, FStaticMeshApplicationError>;
		ENGINE_API static auto MakeRequest(FStaticMeshSource Source,
			const FStaticMeshReconciliationSnapshot& Snapshot) -> FStaticMeshAuthoredBuildRequest;
		ENGINE_API static auto Capture(const DStaticMesh& Mesh)
			-> FStaticMeshReconciliationSnapshot;
	};
}
