#pragma once

#include <expected>

#include "Asset/DerivedDataCacheKeyProxy.h"
#include "EngineAPI.h"
#include "Asset/AssetCacheDiagnostic.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	struct FStaticMeshAuthoredBuildError;
	enum class ETaskState : uint8;

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

	// Bounded value-only observation, without payload ownership or backend paths.
	struct FStaticMeshBuildObservation
	{
		EStaticMeshBuildOrigin Origin = EStaticMeshBuildOrigin::Rebuilt;
		FCacheKeyProxy DerivedDataKey;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
		uint64 PayloadBytes = 0;
	};

	struct FStaticMeshCacheDiagnostics
	{
		FAssetCacheDiagnostic Read;
		FAssetCacheDiagnostic Write;
		std::optional<FStaticMeshCacheCodecError> DecodeCause;
	};
	struct FStaticMeshPersistenceDiagnostic
	{
		FStaticMeshCacheDiagnostics Render;
		FStaticMeshCacheDiagnostics Collision;
	};
	ENGINE_API auto FormatStaticMeshCacheDiagnostics(const FStaticMeshCacheDiagnostics& Diagnostic) -> std::string;
	ENGINE_API auto FormatStaticMeshPersistenceDiagnostic(const FStaticMeshPersistenceDiagnostic& Diagnostic) -> std::string;

	// Caller-owned observation; applying its values never installs this history.
	struct FStaticMeshBuildResult
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		FCacheKeyProxy DerivedDataKey;
		bool bSlotMetadataChanged = false;
		EStaticMeshBuildOrigin Origin = EStaticMeshBuildOrigin::Rebuilt;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 ProviderRegistration = 0;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
		uint64 PayloadBytes = 0;
		FStaticMeshCacheDiagnostics CacheDiagnostics;
	};

	// Detached collision geometry plus the current Engine operation's observation.
	struct FStaticMeshCollisionBuildResult
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		EStaticMeshBuildOrigin Origin = EStaticMeshBuildOrigin::Rebuilt;
		FCacheKeyProxy DerivedDataKey;
		FStaticMeshCacheDiagnostics CacheDiagnostics;
		uint64 PayloadBytes = 0;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 ProviderRegistration = 0;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
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
		{ return {Render.Origin, Render.DerivedDataKey, Render.CacheReadNanoseconds, Render.CacheWriteNanoseconds, Render.PayloadBytes}; }
		auto GetCollisionObservation() const -> FStaticMeshBuildObservation
		{ return {Collision.Origin, Collision.DerivedDataKey, Collision.CacheReadNanoseconds, Collision.CacheWriteNanoseconds, Collision.PayloadBytes}; }
		auto GetPersistenceDiagnostic() const -> FStaticMeshPersistenceDiagnostic
		{ return {Render.CacheDiagnostics, Collision.CacheDiagnostics}; }

		auto GetRenderData() const -> const FStaticMeshRenderData* { return Render.RenderData.get(); }
		auto GetCollision() const -> const FStaticMeshCollisionBuildResult& { return Collision; }
		auto GetProviderRegistration() const -> uint64 { return Render.ProviderRegistration; }
		auto GetSourceIdentity() const -> FXxHash128 { return Request.Source.GetIdentity(); }

	private:
		FStaticMeshAuthoredCandidate() = default;
		FStaticMeshAuthoredBuildRequest Request;
		FStaticMeshBuildResult Render;
		FStaticMeshCollisionBuildResult Collision;
		friend class DStaticMesh;
		friend ENGINE_API auto BuildStaticMeshAuthoredCandidate(FStaticMeshAuthoredBuildRequest,
			std::unique_ptr<FStaticMeshAuthoredCandidate>&,
			const FStaticMeshBuildExecutionControl&) -> std::expected<void, FStaticMeshAuthoredBuildError>;
		friend ENGINE_API auto ApplyStaticMeshAuthoredCandidate(DStaticMesh&,
			std::unique_ptr<FStaticMeshAuthoredCandidate>, const FStaticMeshReconciliationSnapshot&,
			bool, const FStaticMeshBuildExecutionControl&, DAssetImportData*) -> std::expected<void, FStaticMeshApplicationError>;
	};

	ENGINE_API auto MakeStaticMeshAuthoredBuildRequest(FStaticMeshSource Source,
		const FStaticMeshReconciliationSnapshot& Snapshot) -> FStaticMeshAuthoredBuildRequest;
	// Completes render, collision and ray acceleration without touching an object.
	ENGINE_API auto BuildStaticMeshAuthoredCandidate(FStaticMeshAuthoredBuildRequest Request,
		std::unique_ptr<FStaticMeshAuthoredCandidate>& OutCandidate,
		const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<void, FStaticMeshAuthoredBuildError>;
	// Validates owner freshness and cancellation before a single non-building application boundary.
	ENGINE_API auto ApplyStaticMeshAuthoredCandidate(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate,
		const FStaticMeshReconciliationSnapshot& Snapshot,
		bool bMarkPackageDirty = true, const FStaticMeshBuildExecutionControl& Control = {},
		DAssetImportData* PreparedImportData = nullptr) -> std::expected<void, FStaticMeshApplicationError>;

	enum class EStaticMeshDerivedDataError : uint8
	{
		None, Cancelled, Unavailable, ProviderDescriptor, ProviderInvocation,
		Key, Source, Recipe, Payload, MissingLOD, InvalidGeometry, InvalidProduct
	};
	struct FStaticMeshDerivedDataError
	{
		EStaticMeshDerivedDataError Code = EStaticMeshDerivedDataError::None;
		EStaticMeshRecipeKind Kind = EStaticMeshRecipeKind::Render;
		std::optional<EFeatureInvokeStatus> InvocationStatus;
		std::optional<FStaticMeshBuildProviderDescriptor> Descriptor;
		uint64 VertexCount = 0;
		uint64 IndexCount = 0;
		std::optional<FStaticMeshBuildKeyError> KeyCause;
		std::optional<FStaticMeshSourceError> SourceCause;
		std::optional<FStaticMeshRecipeError> RecipeCause;
		std::optional<FStaticMeshCacheCodecError> PayloadCause;
	};

	ENGINE_API auto FormatStaticMeshDerivedDataError(const FStaticMeshDerivedDataError& Error) -> std::string;

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
		None, NotStarted, Cancelled, Input, SourceBudget, RenderBuild, MaterialSlots,
		SlotName, UVChannels, MetadataBudget, FinalizationBudget, Payload, LODPolicy,
		CollisionBuild, ProviderChanged, RetainedBudget, WorkerException, TaskRetired
	};
	enum class EStaticMeshAuthoredBuildPhase : uint8 { Input, Render, Payload, Bounds, Ray, Collision, Retained };
	struct FStaticMeshAuthoredBuildError
	{
		EStaticMeshAuthoredBuildError Code = EStaticMeshAuthoredBuildError::None;
		EStaticMeshAuthoredBuildPhase Phase = EStaticMeshAuthoredBuildPhase::Input;
		bool SourceValid = false;
		float NormalizedSize = 0;
		EBodySetupCollisionSourceMode CollisionMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy CollisionPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		uint64 Index = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::string SlotName;
		std::optional<ETaskState> TaskState;
		std::optional<FStaticMeshBuildMemoryEstimate> MemoryCause;
		std::optional<FStaticMeshBuildProviderDescriptor> RenderDescriptor;
		std::optional<FStaticMeshBuildProviderDescriptor> CollisionDescriptor;
		std::optional<FStaticMeshDerivedDataError> DerivedDataCause;
		std::optional<FStaticMeshPayloadError> PayloadCause;
		std::optional<FStaticMeshLODPolicyError> LODCause;
	};

	ENGINE_API auto FormatStaticMeshAuthoredBuildError(const FStaticMeshAuthoredBuildError& Error) -> std::string;

	ENGINE_API auto BuildStaticMeshDerivedData(
		FStaticMeshBuildRequest Request,
		FStaticMeshBuildResult& OutProduct,
		const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<void, FStaticMeshDerivedDataError>;
	ENGINE_API auto BuildStaticMeshCollisionDerivedData(
		const FStaticMeshRenderData& RenderData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildResult& OutProduct,
		bool bPersistDerivedData = true,
		const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<void, FStaticMeshDerivedDataError>;

	// Capture on the asset's owner thread before dispatching detached work.
	ENGINE_API auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot;
	enum class EStaticMeshDirectBuildError : uint8 { None, Render, Collision };
	struct FStaticMeshDirectBuildError
	{
		EStaticMeshDirectBuildError Code = EStaticMeshDirectBuildError::None;
		std::optional<FStaticMeshReplacementError> RenderCause;
		std::optional<FStaticMeshCollisionError> CollisionCause;
	};

	ENGINE_API auto FormatStaticMeshDirectBuildError(const FStaticMeshDirectBuildError& Error) -> std::string;

	// Applies directly on the owner thread without rollback; reports CPU/collision build failure.
	ENGINE_API auto ApplyStaticMeshBuildResult(DStaticMesh& Mesh,
		FStaticMeshSource Source, FStaticMeshBuildResult Product,
		bool bMarkPackageDirty = true) -> std::expected<void, FStaticMeshDirectBuildError>;
	struct FStaticMeshSubmissionError;
	struct FStaticMeshCompilationDiagnostic;
	enum class EStaticMeshSynchronousError : uint8 { None, Source, Submission, NoObservation, Completion };
	struct FStaticMeshSynchronousError
	{
		EStaticMeshSynchronousError Code = EStaticMeshSynchronousError::None;
		FObjectKey Owner;
		std::optional<FStaticMeshSourceError> SourceCause;
		std::shared_ptr<const FStaticMeshSubmissionError> SubmissionCause;
		std::shared_ptr<const FStaticMeshCompilationDiagnostic> CompletionCause;
	};
	struct FStaticMeshSynchronousResult
	{
		FStaticMeshSynchronousError Error;
		FStaticMeshPersistenceDiagnostic PersistenceDiagnostic;
		explicit operator bool() const { return Error.Code == EStaticMeshSynchronousError::None; }
	};
	ENGINE_API auto FormatStaticMeshSynchronousError(const FStaticMeshSynchronousError& Error) -> std::string;

	ENGINE_API auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		const FStaticMeshSource& Source) -> FStaticMeshSynchronousResult;
	// Fresh authored input boundary: capture once, build from seeded residency, then release.
	ENGINE_API auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		FStaticMeshDecodedGeometry Geometry) -> FStaticMeshSynchronousResult;
}
