#pragma once

#include "Components/MeshComponent.h"

#include "TerrainComponent.gen.h"

namespace Durin
{
	class DMaterialInterface;
	class DTerrainHeightmap;
	struct FTerrainHeightmapPayload;
	class FTerrainHeightmapRenderStateRecreateContext;

	// Reports why the component can or cannot publish a complete Terrain proxy.
	DENUM(DisplayName = "Terrain Render Status")
	enum class ETerrainRenderStatus : uint8
	{
		Unavailable,
		Ready,
		InvalidProperties,
		MissingHeightmap,
		InvalidPayload,
		ExtentRejected
	};

	// Reports why the component can or cannot publish Terrain query collision.
	DENUM(DisplayName = "Terrain Collision Status")
	enum class ETerrainCollisionStatus : uint8
	{
		Unavailable,
		Ready,
		InvalidProperties,
		MissingHeightmap,
		InvalidPayload,
		ExtentRejected,
		BuildFailed
	};

	// Bounded read-only facts for inspecting one component's current collision generation.
	struct FTerrainCollisionFacts
	{
		ETerrainCollisionStatus Status = ETerrainCollisionStatus::Unavailable;
		uint64 AssetRevision = 0;
		uint64 CollisionRevision = 0;
		uint64 ResourceIdentity = 0;
		uint64 RetainedBytes = 0;
		uint64 EstimatedPeakBytes = 0;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Cells = 0;
		uint32 Nodes = 0;
		uint32 MaximumDepth = 0;
		ECollisionGeometryBuildStatus BuildStatus = ECollisionGeometryBuildStatus::InvalidInput;
	};

	// Captures one immutable full-resolution Terrain generation for editor surface queries.
	struct FTerrainPickingSnapshot
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		FMatrix LocalToWorld{1.0};
		double SpacingX = 0.0;
		double SpacingY = 0.0;
		double HeightScale = 0.0;
		double HeightOffset = 0.0;
		uint64 AssetRevision = 0;
	};

	// Binds an immutable heightmap revision to one finite full-density terrain primitive.
	DCLASS(DisplayName = "Terrain Component")
	class DTerrainComponent final : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DTerrainComponent(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto SetHeightmap(DTerrainHeightmap* InHeightmap) -> void;
		auto GetHeightmap() const -> DTerrainHeightmap* { return Heightmap.Get(); }
		ENGINE_API auto SetSampleSpacing(double InSpacingX, double InSpacingY) -> bool;
		auto GetSpacingX() const -> double { return SpacingX; }
		auto GetSpacingY() const -> double { return SpacingY; }
		ENGINE_API auto SetHeightRange(double InScale, double InOffset) -> bool;
		auto GetHeightScale() const -> double { return HeightScale; }
		auto GetHeightOffset() const -> double { return HeightOffset; }
		ENGINE_API auto SetMaterial(DMaterialInterface* InMaterial) -> void;
		auto GetMaterial() const -> DMaterialInterface* { return Material.Get(); }
		auto GetRenderStatus() const -> ETerrainRenderStatus { return RenderStatus; }
		auto GetLastRenderDiagnostic() const -> const std::string& { return LastRenderDiagnostic; }
		auto GetCollisionStatus() const -> ETerrainCollisionStatus { return CollisionStatus; }
		auto GetLastCollisionDiagnostic() const -> const std::string& { return LastCollisionDiagnostic; }
		ENGINE_API auto GetCollisionFacts() const -> FTerrainCollisionFacts;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
		ENGINE_API auto BuildCollisionGeometry(
			FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
#if DURIN_WITH_EDITOR
		ENGINE_API auto GetEditorPickingLocalBounds(FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool override;
		ENGINE_API auto CaptureEditorPickingSnapshot(FTerrainPickingSnapshot& OutSnapshot) const -> bool;
#endif

	private:
		friend class FTerrainHeightmapRenderStateRecreateContext;
		auto PrepareForHeightmapRevisionChange() -> void;
		auto HandleHeightmapRevisionChanged(DTerrainHeightmap* ChangedHeightmap) -> void;
		ENGINE_API auto BuildMaterialRenderProxyBindingUpdate(FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool override;
		auto ValidateProperties(std::string& OutError) const -> bool;
		auto GetCollisionStateRevision() const -> uint64 override { return CollisionRevision; }
		auto SetCollisionFailure(ETerrainCollisionStatus Status, std::string Diagnostic) const -> bool;

		DPROPERTY(Edit)
		TObjectPtr<DTerrainHeightmap> Heightmap;

		// World units between adjacent samples on local X.
		DPROPERTY(Edit)
		double SpacingX = 100.0;

		// World units between adjacent samples on local Y.
		DPROPERTY(Edit)
		double SpacingY = 100.0;

		DPROPERTY(Edit)
		double HeightScale = 1000.0;

		DPROPERTY(Edit)
		double HeightOffset = 0.0;

		DPROPERTY(Edit)
		TObjectPtr<DMaterialInterface> Material;

		DPROPERTY(Transient)
		ETerrainRenderStatus RenderStatus = ETerrainRenderStatus::Unavailable;

		DPROPERTY(Transient)
		std::string LastRenderDiagnostic;

		DPROPERTY(Transient)
		ETerrainCollisionStatus CollisionStatus = ETerrainCollisionStatus::Unavailable;

		DPROPERTY(Transient)
		std::string LastCollisionDiagnostic;

		uint64 MaterialComponentRevision = 1;
		uint64 CollisionRevision = 1;
		mutable uint64 CachedHeightmapRevision = 0;
		mutable std::shared_ptr<const FTerrainHeightmapPayload> CachedCollisionPayload;
		mutable FCollisionGeometryRef CachedTerrainCollision;
		mutable FCollisionGeometryBuildDiagnostics CachedCollisionDiagnostics;
		mutable double CachedCollisionSpacingX = 0.0;
		mutable double CachedCollisionSpacingY = 0.0;
		mutable double CachedCollisionHeightScale = 0.0;
		mutable double CachedCollisionHeightOffset = 0.0;
	};
} // namespace Durin
