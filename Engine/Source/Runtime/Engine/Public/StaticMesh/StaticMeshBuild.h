#pragma once

#include "DerivedDataCacheKeyProxy.h"
#include "EngineAPI.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	// Immutable object facts captured before StaticMesh recipe work begins.
	struct FStaticMeshReconciliationSnapshot
	{
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		float NormalizedSize = 1.5f;
		FXxHash128 SourceIdentity;
		FObjectHandle Body;
		uint64 BodyRevision = 0;
		EBodySetupCollisionSourceMode CollisionMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy CollisionPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
	};

	// Detached Engine request; cache policy is not forwarded to recipe code.
	struct FStaticMeshBuildRequest
	{
		FStaticMeshReconciliationSnapshot Reconciliation;
		FStaticMeshImportedData ImportedData;
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
		std::string DiagnosticMessage;
	};

	// Detached collision geometry plus the current Engine operation's observation.
	struct FStaticMeshCollisionBuildResult
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		EStaticMeshBuildOrigin Origin = EStaticMeshBuildOrigin::Rebuilt;
		FCacheKeyProxy DerivedDataKey;
		std::string Diagnostic;
		uint64 PayloadBytes = 0;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 ProviderRegistration = 0;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
	};

	// Value-only worker input; material object bindings remain in the owner-thread snapshot.
	struct FStaticMeshAuthoredBuildRequest
	{
		FStaticMeshImportedData Source;
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
		auto GetPersistenceDiagnostic() const -> std::string
		{
			if (Render.DiagnosticMessage.empty()) return Collision.Diagnostic;
			if (Collision.Diagnostic.empty()) return Render.DiagnosticMessage;
			return (Render.DiagnosticMessage + " " + Collision.Diagnostic).substr(0, MaximumStaticMeshBuildDiagnosticBytes);
		}
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
		friend auto BuildStaticMeshAuthoredCandidate(FStaticMeshAuthoredBuildRequest,
			std::unique_ptr<FStaticMeshAuthoredCandidate>&, std::string&,
			const FStaticMeshBuildExecutionControl&) -> FStaticMeshBuildOutcome;
		friend auto ApplyStaticMeshAuthoredCandidate(DStaticMesh&,
			std::unique_ptr<FStaticMeshAuthoredCandidate>, const FStaticMeshReconciliationSnapshot&,
			std::string&, bool, const FStaticMeshBuildExecutionControl&, DAssetImportData*) -> FStaticMeshBuildOutcome;
	};

	ENGINE_API auto MakeStaticMeshAuthoredBuildRequest(FStaticMeshImportedData Source,
		const FStaticMeshReconciliationSnapshot& Snapshot) -> FStaticMeshAuthoredBuildRequest;
	// Completes render, collision and ray acceleration without touching an object.
	ENGINE_API auto BuildStaticMeshAuthoredCandidate(FStaticMeshAuthoredBuildRequest Request,
		std::unique_ptr<FStaticMeshAuthoredCandidate>& OutCandidate, std::string& OutError,
		const FStaticMeshBuildExecutionControl& Control = {}) -> FStaticMeshBuildOutcome;
	// Validates owner freshness and cancellation before a single non-building application boundary.
	ENGINE_API auto ApplyStaticMeshAuthoredCandidate(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate,
		const FStaticMeshReconciliationSnapshot& Snapshot, std::string& OutError,
		bool bMarkPackageDirty = true, const FStaticMeshBuildExecutionControl& Control = {},
		DAssetImportData* PreparedImportData = nullptr) -> FStaticMeshBuildOutcome;

	ENGINE_API auto BuildStaticMeshDerivedData(
		FStaticMeshBuildRequest Request,
		FStaticMeshBuildResult& OutProduct,
		std::string& OutError,
		const FStaticMeshBuildExecutionControl& Control = {}) -> FStaticMeshBuildOutcome;
	ENGINE_API auto BuildStaticMeshCollisionDerivedData(
		const FStaticMeshRenderData& RenderData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildResult& OutProduct,
		std::string& OutError,
		bool bPersistDerivedData = true,
		const FStaticMeshBuildExecutionControl& Control = {}) -> FStaticMeshBuildOutcome;

	// Capture on the asset's owner thread before dispatching detached work.
	ENGINE_API auto CaptureStaticMeshReconciliation(const DStaticMesh& Mesh)
		-> FStaticMeshReconciliationSnapshot;
	// Applies on the owner thread; candidate failure preserves existing resources.
	ENGINE_API auto ApplyStaticMeshBuildResult(DStaticMesh& Mesh,
		FStaticMeshImportedData Source, FStaticMeshBuildResult Product, std::string& OutError,
		bool bMarkPackageDirty = true) -> bool;
	ENGINE_API auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		const FStaticMeshImportedData& ImportedData,
		std::string& OutError) -> bool;
	// Fresh authored input boundary: capture once, build from seeded residency, then release.
	ENGINE_API auto BuildStaticMeshSynchronously(DStaticMesh& Mesh,
		FStaticMeshDecodedGeometry Geometry,
		std::string& OutError) -> bool;
}
