#pragma once

#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderPlan.h"
#include "RenderGraph.h"

namespace Durin
{
	enum class ESceneFrameRoute : uint8
	{
		Disabled,
		Fragment,
		Compute,
	};

	enum class ESceneFrameBackingClass : uint8
	{
		Scene,
		GBuffer,
		AmbientOcclusion,
		ContactShadowVisibilityFragment,
		ContactShadowVisibilityCompute,
		VolumetricCloudShadowFragment,
		VolumetricCloudShadowCompute,
		VolumetricCloudFragment,
		VolumetricCloudCompute,
		VolumetricCloudComposite,
		Deferred,
		GBufferDebug,
	};

	[[nodiscard]] constexpr auto GetSceneFrameBackingClassName(
		ESceneFrameBackingClass Class) -> std::string_view
	{
		switch (Class)
		{
		case ESceneFrameBackingClass::Scene: return "renderer.scene";
		case ESceneFrameBackingClass::GBuffer: return "renderer.gbuffer";
		case ESceneFrameBackingClass::AmbientOcclusion:
			return "renderer.ambient-occlusion";
		case ESceneFrameBackingClass::ContactShadowVisibilityFragment:
			return "renderer.contact-shadow-visibility.fragment";
		case ESceneFrameBackingClass::ContactShadowVisibilityCompute:
			return "renderer.contact-shadow-visibility.compute";
		case ESceneFrameBackingClass::VolumetricCloudShadowFragment:
			return "renderer.cloud-shadow.fragment";
		case ESceneFrameBackingClass::VolumetricCloudShadowCompute:
			return "renderer.cloud-shadow.compute";
		case ESceneFrameBackingClass::VolumetricCloudFragment:
			return "renderer.cloud.fragment";
		case ESceneFrameBackingClass::VolumetricCloudCompute:
			return "renderer.cloud.compute";
		case ESceneFrameBackingClass::VolumetricCloudComposite:
			return "renderer.cloud.composite";
		case ESceneFrameBackingClass::Deferred: return "renderer.deferred";
		case ESceneFrameBackingClass::GBufferDebug:
			return "renderer.gbuffer-debug";
		}
		return {};
	}

	[[nodiscard]] inline auto ParseSceneFrameBackingClass(std::string_view Name)
		-> std::optional<ESceneFrameBackingClass>
	{
		for (uint8 Value = 0;
			Value <= static_cast<uint8>(ESceneFrameBackingClass::GBufferDebug);
			++Value)
		{
			const auto Class = static_cast<ESceneFrameBackingClass>(Value);
			if (GetSceneFrameBackingClassName(Class) == Name) return Class;
		}
		return std::nullopt;
	}

	struct FSceneFrameTopology
	{
		uint32 Width = 0;
		uint32 Height = 0;
		bool bGBuffer = false;
		bool bGroundTruthAmbientOcclusion = false;
		ESceneFrameRoute ContactShadowVisibility = ESceneFrameRoute::Disabled;
		ESceneFrameRoute VolumetricCloudShadow = ESceneFrameRoute::Disabled;
		bool bIsolatedDeferred = false;
		bool bGBufferDebug = false;
		ESceneFrameRoute VolumetricCloud = ESceneFrameRoute::Disabled;
		bool bVolumetricCloudComposite = false;
		EGroundTruthAmbientOcclusionQuality AmbientOcclusionQuality =
			EGroundTruthAmbientOcclusionQuality::FullResolution;
		FIntPoint VolumetricCloudExtent{0, 0};

		[[nodiscard]] auto UsesContactShadowVisibilityFragment() const -> bool
		{
			return ContactShadowVisibility == ESceneFrameRoute::Fragment;
		}
		[[nodiscard]] auto UsesContactShadowVisibilityCompute() const -> bool
		{
			return ContactShadowVisibility == ESceneFrameRoute::Compute;
		}
		[[nodiscard]] auto UsesCloudShadowFragment() const -> bool
		{
			return VolumetricCloudShadow == ESceneFrameRoute::Fragment;
		}
		[[nodiscard]] auto UsesCloudShadowCompute() const -> bool
		{
			return VolumetricCloudShadow == ESceneFrameRoute::Compute;
		}
		[[nodiscard]] auto UsesCloudFragment() const -> bool
		{
			return VolumetricCloud == ESceneFrameRoute::Fragment;
		}
		[[nodiscard]] auto UsesCloudCompute() const -> bool
		{
			return VolumetricCloud == ESceneFrameRoute::Compute;
		}
	};

	struct FResolvedSceneFrameTargets
	{
		std::optional<FPostProcessRenderer::FSceneTargets> Scene;
		std::optional<FGBufferRenderer::FTargets> GBuffer;
		std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
			GroundTruthAmbientOcclusion;
		std::optional<FContactShadowVisibilityRenderer::FTargets>
			ContactShadowVisibilityFragment;
		std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
			ContactShadowVisibilityCompute;
		std::optional<FVolumetricCloudShadowRenderer::FTargets>
			VolumetricCloudShadowFragment;
		std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
			VolumetricCloudShadowCompute;
		std::optional<FDeferredDirectionalLightingRenderer::FTargets>
			IsolatedDeferred;
		std::optional<FGBufferDebugRenderer::FTargets> GBufferDebug;
		std::optional<FVolumetricCloudRenderer::FTargets> VolumetricCloudFragment;
		std::optional<FVolumetricCloudRenderer::FComputeTargets>
			VolumetricCloudCompute;
		std::optional<FVolumetricCloudRenderer::FTargets> VolumetricCloudComposite;
	};

	struct FResolvedSceneFrame
	{
		FResolvedLighting Lighting;
		FResolvedReceiverGeometry Receiver;
		std::optional<FResolvedDirectionalShadow> DirectionalShadow;
		std::optional<FResolvedVolumetricCloud> VolumetricCloud;
		FResolvedSceneFrameTargets Targets;
	};

	template <typename TResult>
	struct TSceneFrameGraphValue
	{
		TRenderGraphValueHandle<TResult> Handle;
	};

	enum class ESceneFrameGraphExecutionStatus : uint8
	{
		CompileFailed,
		ExecutionFailed,
		Executed,
	};

	using FSceneFrameGraphExecute = std::function<ESceneFrameGraphExecutionStatus(
		FRenderGraphBuilder&)>;
} // namespace Durin
