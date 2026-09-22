#pragma once

#include <expected>

#include "EngineAPI.h"
#include "StaticMesh/StaticMeshBuildFailure.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
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

	struct FStaticMeshCollisionBuildProduct
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		auto GetCacheErrors() const -> const std::vector<FStaticMeshCacheError>& { return CacheErrors; }
	private:
		std::vector<FStaticMeshCacheError> CacheErrors;
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
		auto GetCacheErrors() const -> std::vector<FStaticMeshCacheError>
		{
			auto Errors = CacheErrors;
			Errors.insert(Errors.end(), Collision.CacheErrors.begin(), Collision.CacheErrors.end());
			return Errors;
		}

		auto GetRenderData() const -> const FStaticMeshRenderData* { return Render.get(); }
		auto GetCollision() const -> const FStaticMeshCollisionBuildProduct& { return Collision; }
		auto GetProviderRegistration() const -> uint64 { return ProviderRegistration; }
		auto GetSourceIdentity() const -> FXxHash128 { return Request.Source.GetIdentity(); }

	private:
		FStaticMeshAuthoredCandidate() = default;
		FStaticMeshAuthoredBuildRequest Request;
		std::unique_ptr<FStaticMeshRenderData> Render;
		std::vector<FStaticMeshCacheError> CacheErrors;
		uint64 ProviderRegistration = 0;
		FStaticMeshCollisionBuildProduct Collision;
		friend class DStaticMesh;
		friend class FStaticMeshBuilder;
	};

	// Advanced detached building API. Ordinary asset callers use DStaticMesh::Build/AsyncBuild.
	class FStaticMeshBuilder
	{
	public:
		ENGINE_API static auto Build(
			FStaticMeshBuildRequest Request,
			const FStaticMeshBuildExecutionControl& Control = {},
			std::vector<FStaticMeshCacheError>* OutCacheErrors = nullptr,
			uint64* OutProviderRegistration = nullptr) -> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>;
		ENGINE_API static auto BuildCollision(
			const FStaticMeshRenderData& RenderData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			bool bPersistDerivedData = true,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<FStaticMeshCollisionBuildProduct, FStaticMeshBuildFailure>;
		ENGINE_API static auto BuildCandidate(FStaticMeshAuthoredBuildRequest Request,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<std::unique_ptr<FStaticMeshAuthoredCandidate>, FStaticMeshBuildFailure>;
		ENGINE_API static auto ApplyCandidate(DStaticMesh& Mesh,
			std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate,
			const FStaticMeshReconciliationSnapshot& Snapshot,
			bool bMarkPackageDirty = true, const FStaticMeshBuildExecutionControl& Control = {},
			DAssetImportData* PreparedImportData = nullptr,
			std::vector<FMeshMaterialSlotDefinition>* PreparedMaterialSlots = nullptr) -> std::expected<void, FStaticMeshBuildFailure>;
		ENGINE_API static auto MakeRequest(FStaticMeshSource Source,
			const FStaticMeshReconciliationSnapshot& Snapshot) -> FStaticMeshAuthoredBuildRequest;
		ENGINE_API static auto Capture(const DStaticMesh& Mesh)
			-> FStaticMeshReconciliationSnapshot;
	};
}
