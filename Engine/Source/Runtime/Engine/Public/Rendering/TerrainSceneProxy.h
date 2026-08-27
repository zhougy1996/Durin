#pragma once

#include "Rendering/PrimitiveSceneProxy.h"

namespace Durin
{
	struct FTerrainHeightmapPayload;

	// Describes one deterministic full-density terrain patch in sample space.
	struct FTerrainPatchDescriptor
	{
		uint32 OriginX = 0;
		uint32 OriginY = 0;
		uint16 GridX = 0;
		uint16 GridY = 0;
		uint32 CellCountX = 0;
		uint32 CellCountY = 0;
		// Nested power-of-two sample steps and conservative object-space Z errors.
		std::vector<uint32> LODSteps{1};
		std::vector<double> LODErrors{0.0};
		FBox LocalBounds;
	};

	// Retains an immutable height revision and copied terrain values for render-thread use.
	class FTerrainSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		ENGINE_API FTerrainSceneProxy(
			std::shared_ptr<const FTerrainHeightmapPayload> InPayload,
			uint64 InRevision, double InSpacingX, double InSpacingY,
			double InHeightScale, double InHeightOffset,
			std::vector<FTerrainPatchDescriptor> InPatches,
			FBox InLocalBounds, FMaterialRenderProxyRef InMaterial,
			uint64 InMaterialComponentRevision);

		auto GetKind() const -> EPrimitiveSceneProxyKind override { return EPrimitiveSceneProxyKind::Terrain; }
		auto GetLocalBounds() const -> FBox override { return LocalBounds; }
		auto GetPayload() const -> const std::shared_ptr<const FTerrainHeightmapPayload>& { return Payload; }
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetSpacingX() const -> double { return SpacingX; }
		auto GetSpacingY() const -> double { return SpacingY; }
		auto GetHeightScale() const -> double { return HeightScale; }
		auto GetHeightOffset() const -> double { return HeightOffset; }
		auto GetPatches() const -> std::span<const FTerrainPatchDescriptor> { return Patches; }
		auto GetLODMetadataBytes() const -> size_t { return LODMetadataBytes; }
		ENGINE_API auto ResolveMaterialRenderData_RenderThread() const -> const FMaterialRenderData&;
		ENGINE_API auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate& Update) -> bool override;

	private:
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		uint64 Revision = 0;
		double SpacingX = 1.0;
		double SpacingY = 1.0;
		double HeightScale = 1.0;
		double HeightOffset = 0.0;
		std::vector<FTerrainPatchDescriptor> Patches;
		size_t LODMetadataBytes = 0;
		FBox LocalBounds;
		FMaterialRenderProxyRef Material;
		uint64 MaterialComponentRevision = 0;
	};
}
