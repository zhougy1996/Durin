#pragma once

#include "Renderers/SceneVisibility.h"
#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"

#include "EnvironmentLighting/EnvironmentLighting.h"
#include "IScene.h"
#include "SceneView.h"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <vector>

namespace Durin
{
	class FTextureCubePreviewSceneProxy;

	enum class EPreparedTranslucentGeometryFamily : uint8
	{
		StaticMesh,
		SkeletalMesh,
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
		FDirectionalLightSceneData DirectionalLight;
		FSkyBoxSceneData SkyBox;
		bool bHasSkyBox = false;
		FPreparedStaticMeshView StaticMeshes;
		FPreparedSkeletalMeshView SkeletalMeshes;
		std::vector<FPreparedTranslucentSceneDraw> TranslucentGeometry;
		std::vector<const FTextureCubePreviewSceneProxy*> TextureCubePreviews;
		FViewRenderCounters Counters;
	};

	inline auto PrepareCombinedTranslucentGeometry(FPreparedSceneView& View)
		-> void
	{
		View.TranslucentGeometry.clear();
		View.TranslucentGeometry.reserve(View.StaticMeshes.Translucent.size()
			+ View.SkeletalMeshes.Translucent.size());
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
		auto CompareValue = [](const auto& A, const auto& B) {
			if (std::ranges::lexicographical_compare(A, B)) return -1;
			if (std::ranges::lexicographical_compare(B, A)) return 1;
			return 0;
		};
		std::ranges::sort(View.TranslucentGeometry,
			[&](const auto& A, const auto& B) {
				if (A.DistanceSquared != B.DistanceSquared)
					return A.DistanceSquared > B.DistanceSquared;
				if (const int Value = CompareValue(
					A.SortKey.Pipeline, B.SortKey.Pipeline)) return Value < 0;
				if (const int Value = CompareValue(
					A.SortKey.MaterialUniform, B.SortKey.MaterialUniform))
					return Value < 0;
				if (const int Value = CompareValue(
					A.SortKey.VertexFactory, B.SortKey.VertexFactory))
					return Value < 0;
				if (const int Value = CompareValue(
					A.SortKey.Geometry, B.SortKey.Geometry)) return Value < 0;
				if (A.SortKey.PrimitiveId != B.SortKey.PrimitiveId)
					return A.SortKey.PrimitiveId < B.SortKey.PrimitiveId;
				if (A.SortKey.SectionIndex != B.SortKey.SectionIndex)
					return A.SortKey.SectionIndex < B.SortKey.SectionIndex;
				return static_cast<uint8>(A.Family)
					< static_cast<uint8>(B.Family);
			});
	}
} // namespace Durin
