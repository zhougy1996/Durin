#pragma once

#include "Renderers/SceneVisibility.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/TerrainRenderPreparation.h"

#include "EnvironmentLighting/EnvironmentLighting.h"
#include "Renderers/ForwardLighting.h"
#include "IScene.h"
#include "SceneView.h"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <vector>

namespace Durin
{
	enum class EPreparedTranslucentGeometryFamily : uint8
	{
		StaticMesh,
		SkeletalMesh,
		Terrain,
	};

	struct FPreparedTranslucentSceneDraw
	{
		EPreparedTranslucentGeometryFamily Family =
			EPreparedTranslucentGeometryFamily::StaticMesh;
		uint32 DrawIndex = 0;
		double DistanceSquared = 0.0;
		FStaticMeshDrawSortKey SortKey;
	};

	// Command-local immutable scene data used by Scene Color execution.
	struct FPreparedSceneView
	{
		FSceneView View;
		FPreparedLightView Lights;
		FPreparedDirectionalShadowView DirectionalShadow;
		FDirectionalShadowCasterTable DirectionalShadowCasters;
		FPreparedSkeletalPaletteTable SkeletalPalettes;
		std::array<FPreparedStaticMeshView,
			DirectionalShadowCascadeCount> ShadowStaticMeshes;
		std::array<FPreparedSkeletalMeshView,
			DirectionalShadowCascadeCount> ShadowSkeletalMeshes;
		std::array<FPreparedTerrainView,
			DirectionalShadowCascadeCount> ShadowTerrains;
		FRHIUniformBufferRange LightingUniformBuffer;
		FSkyBoxSceneData SkyBox;
		bool bHasSkyBox = false;
		FRHITexture* ViewEnvironmentTexture = nullptr;
		bool bHasViewEnvironment = false;
		FPreparedStaticMeshView StaticMeshes;
		FPreparedSkeletalMeshView SkeletalMeshes;
		FPreparedTerrainView Terrains;
		std::vector<FPreparedTranslucentSceneDraw> TranslucentGeometry;
		FViewRenderCounters Counters;
	};

	inline auto PrepareCombinedTranslucentGeometry(FPreparedSceneView& View)
		-> void
	{
		View.TranslucentGeometry.clear();
		View.TranslucentGeometry.reserve(View.StaticMeshes.Translucent.size()
			+ View.SkeletalMeshes.Translucent.size()
			+ View.Terrains.Translucent.size());
		for (uint32 Index = 0; Index < View.StaticMeshes.Translucent.size(); ++Index)
		{
			const auto& Draw = View.StaticMeshes.Translucent[Index];
			View.TranslucentGeometry.push_back({
				EPreparedTranslucentGeometryFamily::StaticMesh, Index,
				Draw.TranslucentDistanceSquared, Draw.SortKey});
		}
		for (uint32 Index = 0; Index < View.SkeletalMeshes.Translucent.size(); ++Index)
		{
			const auto& Draw = View.SkeletalMeshes.Translucent[Index];
			View.TranslucentGeometry.push_back({
				EPreparedTranslucentGeometryFamily::SkeletalMesh, Index,
				Draw.TranslucentDistanceSquared, Draw.SortKey});
		}
		for (uint32 Index = 0; Index < View.Terrains.Translucent.size(); ++Index)
		{
			const auto& Draw = View.Terrains.Translucent[Index];
			View.TranslucentGeometry.push_back({
				EPreparedTranslucentGeometryFamily::Terrain, Index,
				Draw.TranslucentDistanceSquared, Draw.SortKey});
		}
		std::ranges::sort(View.TranslucentGeometry,
			[](const auto& A, const auto& B) {
				if (A.DistanceSquared != B.DistanceSquared)
					return A.DistanceSquared > B.DistanceSquared;
				if (const auto Order = A.SortKey <=> B.SortKey; Order != 0)
					return Order < 0;
				return static_cast<uint8>(A.Family)
					< static_cast<uint8>(B.Family);
			});
	}
} // namespace Durin
