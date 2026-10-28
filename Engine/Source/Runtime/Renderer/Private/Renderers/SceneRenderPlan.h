#pragma once

#include "CoreMinimal.h"

#include "RendererAPI.h"

#include "Renderers/DirectionalShadowView.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneViewState.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/VolumetricCloudRenderer.h"

#include "IRendererModule.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Renderers/ForwardLighting.h"
#include "SceneView.h"

namespace Durin
{
	// Represents one complete optional sky/environment input.
	struct FPreparedEnvironment
	{
		FSkyBoxSceneData SkyBox;
		FRHITexture* Texture = nullptr;
	};

	// Owns selected lights without per-submission GPU allocation state.
	struct FPreparedLighting
	{
		FPreparedLightView Lights;
	};

	// Owns the command-local packed lighting upload resolved for execution.
	struct FResolvedLighting
	{
		FRHIUniformBufferRange UniformBuffer;
	};

	// Owns immutable receiver geometry.
	struct FPreparedReceiverGeometry
	{
		FPreparedStaticMeshView StaticMeshes;
	};

	// Owns one complete directional-shadow preparation and its cascade draws.
	struct FPreparedDirectionalShadow
	{
		FPreparedDirectionalShadowView View;
		std::array<FPreparedStaticMeshView,
			DirectionalShadowCascadeCount> StaticMeshes;
	};

	// Owns receiver execution resources separately from immutable logical preparation.
	struct FResolvedReceiverGeometry
	{
		FResolvedStaticMeshView StaticMeshes;
	};

	// Owns per-cascade execution resources separately from shadow membership.
	struct FResolvedDirectionalShadow
	{
		bool bEnabled = false;
		std::array<FResolvedStaticMeshView,
			DirectionalShadowCascadeCount> StaticMeshes;
	};

	// Represents one complete optional cloud preparation.
	struct FPreparedVolumetricCloud
	{
		FVolumetricCloudRenderer::FParameters Parameters;
		FVolumetricCloudRenderer::FTextureBindings Textures;
		uint64 HistoryKey = 0;
	};

	// Owns cloud bindings that require Renderer resource resolution.
	struct FResolvedVolumetricCloud
	{
		FVolumetricCloudRenderer::FTextureBindings Textures;
	};

	// Command-local owner of immutable feature-bounded logical preparation.
	struct FSceneRenderPlan
	{
		FSceneView View;
		std::optional<FPreparedEnvironment> Environment;
		FPreparedLighting Lighting;
		FPreparedReceiverGeometry Receiver;
		std::optional<FPreparedDirectionalShadow> DirectionalShadow;
		std::optional<FPreparedVolumetricCloud> VolumetricCloud;
	};

	// Publishes either one complete logical plan or its preparation failure.
	struct FSceneRenderPreparationResult
	{
		ERenderViewResult Result = ERenderViewResult::Success;
		std::optional<FSceneRenderPlan> Plan;

		[[nodiscard]] auto IsSuccess() const -> bool
		{
			return Result == ERenderViewResult::Success && Plan.has_value();
		}
	};

} // namespace Durin
