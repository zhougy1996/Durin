#pragma once

#include "Components/MeshComponent.h"

#include "TerrainComponent.gen.h"

namespace Durin
{
	class DMaterialInterface;
	class DTerrainHeightmap;
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
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
#if DURIN_WITH_EDITOR
		ENGINE_API auto GetEditorPickingLocalBounds(FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool override;
#endif

	private:
		friend class FTerrainHeightmapRenderStateRecreateContext;
		auto HandleHeightmapRevisionChanged(DTerrainHeightmap* ChangedHeightmap) -> void;
		ENGINE_API auto BuildMaterialRenderProxyBindingUpdate(FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool override;
		auto ValidateProperties(std::string& OutError) const -> bool;

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

		uint64 MaterialComponentRevision = 1;
	};
} // namespace Durin
