#pragma once

#include "CoreMinimal.h"

#include "Components/MeshComponent.h"
#include "Collision/CollisionGeometry.h"
#include "Spline/SplineTypes.h"
#include "Physics/CollisionDataProvider.h"
#include "Physics/BodySetupTypes.h"

#include "SplineMeshComponent.gen.h"

namespace Durin
{
	class DMaterialInterface;
	class DStaticMesh;
	class DBodySetup;
	class FStaticMeshRenderStateRecreateContext;

	enum class ESplineMeshDerivedStateStatus : uint8
	{
		Valid,
		NoStaticMesh,
		SourceDataUnavailable,
		InvalidSourceData,
		InvalidDeformation
	};

	DENUM()
	enum class ESplineMeshCollisionMode : uint8
	{
		Disabled,
		DeformedTriangleMesh,
		DeformedSimpleCollision,
		DeformedSimpleAndComplex
	};

	// Immutable render deformation snapshot. Collision is owned separately by BodySetup.
	struct FSplineMeshDerivedState
	{
		FSplineMeshParams Params;
		FBox ConservativeLocalBounds;
		// CPU inputs used to derive deformation; independent of GPU readiness.
		FBox SourceLocalBounds;
		ESplineMeshDerivedStateStatus Status = ESplineMeshDerivedStateStatus::NoStaticMesh;
		std::string Diagnostic;

		auto IsValid() const -> bool { return Status == ESplineMeshDerivedStateStatus::Valid; }
	};

	// Deforms a borrowed StaticMesh through a value-only Hermite interval.
	DCLASS()
	class DSplineMeshComponent : public DMeshComponent, public IInterface_CollisionDataProvider
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSplineMeshComponent(const FObjectInitializer& ObjectInitializer);

		// Deferred setters change authored values only. Call UpdateMesh before leaving the batch;
		// registration flushes pending edits. Existing render/physics snapshots remain published meanwhile.
		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh, bool bUpdateMesh = true) -> void;
		auto GetStaticMesh() const -> DStaticMesh* { return StaticMesh.Get(); }
		// Stores authored parameters; validation and normalization happen during UpdateMesh.
		ENGINE_API auto SetSplineMeshParams(const FSplineMeshParams& InParams, bool bUpdateMesh = true) -> void;
		auto GetSplineMeshParams() const -> const FSplineMeshParams& { return SplineMeshParams; }
		auto GetSplineMeshCollisionMode() const -> ESplineMeshCollisionMode { return CollisionMode; }
		ENGINE_API auto SetSplineMeshCollisionMode(ESplineMeshCollisionMode InMode, bool bUpdateMesh = true) -> void;
		// Applies pending edits, invalidates failed results, and logs diagnostics internally.
		// Also retries a failed update; clean successful updates do nothing.
		ENGINE_API auto UpdateMesh() -> void;
		ENGINE_API auto HandleStaticMeshCollisionSourceChanged(DStaticMesh* ChangedMesh, EBodySetupChange Changes) -> void;
		auto GetMeshUpdateError() const -> const std::string& { return MeshUpdateError; }
		auto GetCollisionBuildError() const -> const std::string& { return CollisionBuildError; }
		auto IsMeshDirty() const -> bool { return bSourceDirty || bDeformationDirty || bCollisionDirty; }
		// Reads the published snapshot without building CPU geometry. IsValid describes render readiness.
		ENGINE_API auto GetDerivedState() const -> std::shared_ptr<const FSplineMeshDerivedState>;
		auto GetBodySetup() const -> DBodySetup* { return BodySetup.Get(); }
		ENGINE_API auto ContainsPhysicsTriMeshData() const -> bool override;
		// Owner-thread capture of source collision triangles deformed with the applied parameters.
		ENGINE_API auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override;
		ENGINE_API auto PopulateCollisionGeometry(FPhysicsBodyDesc& Desc) const -> bool override;

		ENGINE_API auto GetNumMaterials() const -> uint32 override;
		ENGINE_API auto GetMaterialIndex(FName SlotName) const -> std::optional<uint32> override;
		ENGINE_API auto GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface* override;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
		ENGINE_API auto CollectPSOPrecacheData(const FPSOPrecacheParams& Params, FComponentPSOPrecacheParamsList& OutParams) -> void override;
		ENGINE_API auto OnRegister() -> void override;

		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		friend class FStaticMeshRenderStateRecreateContext;
		auto RebuildDerivedState(std::string* OutError = nullptr) -> bool;
		auto HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void;
		auto PushDynamicDataToScene() -> void;
		auto GetCollisionStateRevision() const -> uint64 override;
		auto RebuildCollisionGeometryForPublishedState(bool bRebuildSimple, bool bRebuildComplex) -> void;

		DPROPERTY(Category = "Static Mesh", Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		DPROPERTY(Category = "Deformation", Edit)
		FSplineMeshParams SplineMeshParams;

		DPROPERTY(Category = "Collision", Edit)
		ESplineMeshCollisionMode CollisionMode = ESplineMeshCollisionMode::Disabled;

		DPROPERTY(Transient, NonTransactional)
		TObjectPtr<DBodySetup> BodySetup;

		std::shared_ptr<const FSplineMeshDerivedState> DerivedState;
		bool bSourceDirty = false;
		bool bDeformationDirty = false;
		bool bCollisionDirty = false;
		bool bSimpleCollisionDirty = false;
		bool bComplexCollisionDirty = false;
		// Resource requests can synchronously notify this component while UpdateMesh is rebuilding it.
		bool bUpdatingMesh = false;
		std::string MeshUpdateError;
		std::string CollisionBuildError;
		// Physics reads the applied mode until deferred authored edits are applied.
		ESplineMeshCollisionMode PublishedCollisionMode = ESplineMeshCollisionMode::Disabled;
		uint64 PublishedCollisionSourceRevision = 0;
	};
}
