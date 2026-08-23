#pragma once

#include "Renderers/DirectionalShadowView.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneViewState.h"
#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/TerrainRenderPreparation.h"
#include "Renderers/VolumetricCloudRenderer.h"

#include "EnvironmentLighting/EnvironmentLighting.h"
#include "Renderers/ForwardLighting.h"
#include "IScene.h"
#include "SceneView.h"

#include <algorithm>
#include <cstddef>
#include <optional>
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
		FMeshDrawSortKey SortKey;
	};

	// Owns the fitted view and transactional temporal inputs for one command.
	struct FPreparedViewContext
	{
		FSceneView View;
		FSceneViewTemporalContext TemporalContext;
		FSceneViewState* ViewState = nullptr;
	};

	// Represents one complete optional sky/environment input.
	struct FPreparedEnvironment
	{
		FSkyBoxSceneData SkyBox;
		FRHITexture* Texture = nullptr;
	};

	// Owns selected lights and their command-local packed uniform upload.
	struct FPreparedLighting
	{
		FPreparedLightView Lights;
		FRHIUniformBufferRange UniformBuffer;
	};

	// Owns immutable receiver geometry and its shared submission-local palettes.
	struct FPreparedReceiverGeometry
	{
		FPreparedSkeletalPaletteTable SkeletalPalettes;
		FPreparedStaticMeshView StaticMeshes;
		FPreparedSkeletalMeshView SkeletalMeshes;
		FPreparedTerrainView Terrains;
		std::vector<FPreparedTranslucentSceneDraw> TranslucentGeometry;
	};

	// Owns one complete directional-shadow preparation and its cascade draws.
	struct FPreparedDirectionalShadow
	{
		FPreparedDirectionalShadowView View;
		FDirectionalShadowCasterTable Casters;
		std::array<FPreparedStaticMeshView,
			DirectionalShadowCascadeCount> StaticMeshes;
		std::array<FPreparedSkeletalMeshView,
			DirectionalShadowCascadeCount> SkeletalMeshes;
		std::array<FPreparedTerrainView,
			DirectionalShadowCascadeCount> Terrains;
	};

	// Owns receiver execution resources separately from immutable logical preparation.
	struct FResolvedReceiverGeometry
	{
		FResolvedStaticMeshView StaticMeshes;
		FResolvedSkeletalMeshView SkeletalMeshes;
		FResolvedTerrainView Terrains;
	};

	// Owns per-cascade execution resources separately from shadow membership.
	struct FResolvedDirectionalShadow
	{
		std::array<FResolvedStaticMeshView,
			DirectionalShadowCascadeCount> StaticMeshes;
		std::array<FResolvedSkeletalMeshView,
			DirectionalShadowCascadeCount> SkeletalMeshes;
		std::array<FResolvedTerrainView,
			DirectionalShadowCascadeCount> Terrains;
	};

	// Narrows development qualification to cloud route policy only.
	struct FVolumetricCloudQualificationOptions
	{
		bool bForceFragment = false;
	};

	// Represents one complete optional cloud preparation.
	struct FPreparedVolumetricCloud
	{
		FVolumetricCloudRenderer::FParameters Parameters;
		FVolumetricCloudRenderer::FTextureBindings Textures;
		uint64 HistoryKey = 0;
		bool bForceFragmentForQualification = false;
	};

	// Command-local owner of all feature-bounded frame preparation.
	struct FSceneRenderPlan
	{
		FPreparedViewContext Context;
		std::optional<FPreparedEnvironment> Environment;
		FPreparedLighting Lighting;
		FPreparedReceiverGeometry Receiver;
		FResolvedReceiverGeometry ResolvedReceiver;
		std::optional<FPreparedDirectionalShadow> DirectionalShadow;
		std::optional<FResolvedDirectionalShadow> ResolvedDirectionalShadow;
		std::optional<FPreparedVolumetricCloud> VolumetricCloud;
		FRHITexture* VolumetricCloudShadowVisibility = nullptr;
		FSceneRenderTelemetry Telemetry;
	};

	inline auto PrepareCombinedTranslucentGeometry(
		FPreparedReceiverGeometry& Geometry) -> void
	{
		Geometry.TranslucentGeometry.clear();
		Geometry.TranslucentGeometry.reserve(
			Geometry.StaticMeshes.Translucent.size()
			+ Geometry.SkeletalMeshes.Translucent.size()
			+ Geometry.Terrains.Translucent.size());
		for (uint32 Index = 0;
			 Index < Geometry.StaticMeshes.Translucent.size(); ++Index)
		{
			const auto& Draw = Geometry.StaticMeshes.Translucent[Index];
			Geometry.TranslucentGeometry.push_back({
				EPreparedTranslucentGeometryFamily::StaticMesh, Index,
				Draw.TranslucentDistanceSquared, Draw.SortKey});
		}
		for (uint32 Index = 0;
			 Index < Geometry.SkeletalMeshes.Translucent.size(); ++Index)
		{
			const auto& Draw = Geometry.SkeletalMeshes.Translucent[Index];
			Geometry.TranslucentGeometry.push_back({
				EPreparedTranslucentGeometryFamily::SkeletalMesh, Index,
				Draw.TranslucentDistanceSquared, Draw.SortKey});
		}
		for (uint32 Index = 0;
			 Index < Geometry.Terrains.Translucent.size(); ++Index)
		{
			const auto& Draw = Geometry.Terrains.Translucent[Index];
			Geometry.TranslucentGeometry.push_back({
				EPreparedTranslucentGeometryFamily::Terrain, Index,
				Draw.TranslucentDistanceSquared, Draw.SortKey});
		}
		std::ranges::sort(Geometry.TranslucentGeometry,
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
