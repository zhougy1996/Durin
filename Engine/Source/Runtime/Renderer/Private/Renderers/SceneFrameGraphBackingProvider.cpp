#include "Renderers/SceneFrameGraphBackingProvider.h"

namespace Durin
{
	auto FSceneFrameGraphBackingProvider::BuildRetainedTopology(
		std::span<const FRenderGraphPreparationRequest> Requests,
		const FSceneFrameTopology& Frame,
		std::string& Error) -> std::optional<FSceneFrameTopology>
	{
		FSceneFrameTopology Retained{
			.Width = Frame.Width,
			.Height = Frame.Height,
			.AmbientOcclusionQuality = Frame.AmbientOcclusionQuality};
		for (const FRenderGraphPreparationRequest& Request : Requests)
		{
			const auto Class = ParseSceneFrameBackingClass(Request.BackingClass);
			if (!Class)
			{
				Error = "unknown renderer backing class '" + Request.BackingClass + "'";
				return std::nullopt;
			}
			switch (*Class)
			{
			case ESceneFrameBackingClass::Scene: break;
			case ESceneFrameBackingClass::GBuffer: Retained.bGBuffer = true; break;
			case ESceneFrameBackingClass::AmbientOcclusion:
				Retained.bGroundTruthAmbientOcclusion = true; break;
			case ESceneFrameBackingClass::ContactShadowVisibilityFragment:
				Retained.ContactShadowVisibility = ESceneFrameRoute::Fragment; break;
			case ESceneFrameBackingClass::ContactShadowVisibilityCompute:
				Retained.ContactShadowVisibility = ESceneFrameRoute::Compute; break;
			case ESceneFrameBackingClass::VolumetricCloudShadowFragment:
				Retained.VolumetricCloudShadow = ESceneFrameRoute::Fragment; break;
			case ESceneFrameBackingClass::VolumetricCloudShadowCompute:
				Retained.VolumetricCloudShadow = ESceneFrameRoute::Compute; break;
			case ESceneFrameBackingClass::VolumetricCloudFragment:
			case ESceneFrameBackingClass::VolumetricCloudCompute:
				Retained.VolumetricCloud =
					*Class == ESceneFrameBackingClass::VolumetricCloudFragment
					? ESceneFrameRoute::Fragment : ESceneFrameRoute::Compute;
				Retained.VolumetricCloudExtent = Request.TextureDesc.Extent;
				break;
			case ESceneFrameBackingClass::VolumetricCloudComposite:
				Retained.bVolumetricCloudComposite = true; break;
			case ESceneFrameBackingClass::Deferred:
				Retained.bIsolatedDeferred = true; break;
			case ESceneFrameBackingClass::GBufferDebug:
				Retained.bGBufferDebug = true; break;
			}
		}
		return Retained;
	}

	auto FSceneFrameGraphBackingProvider::Publish(
		std::span<const FRenderGraphPreparationRequest> Requests,
		FRenderGraphResourceBackings& Backings,
		const FSceneFrameGraphResources& Resources,
		const FResolvedSceneFrameTargets& Targets,
		std::string& Error) -> bool
	{
		if (!Targets.Scene) return false;
		auto IsRequested = [&](FRenderGraphTextureHandle Handle) {
			return std::ranges::any_of(Requests,
				[&](const FRenderGraphPreparationRequest& Request) {
					return Request.Kind == ERenderGraphResourceKind::Texture
						&& Request.Texture == Handle;
				});
		};
		bool Complete = Backings.SetTexture(Resources.SceneColor, Targets.Scene->Color)
			&& Backings.SetTexture(Resources.SceneDepth, Targets.Scene->Depth);
		auto SetOptional = [&](const auto& Logical, FRHITexture* Physical) {
			if (!Logical || !IsRequested(*Logical)) return true;
			return Physical != nullptr && Backings.SetTexture(*Logical, Physical);
		};
		if (Resources.GBuffer[0] && IsRequested(*Resources.GBuffer[0]))
		{
			if (!Targets.GBuffer) return false;
			const std::array Physical{Targets.GBuffer->Material, Targets.GBuffer->Normals,
				Targets.GBuffer->Surface, Targets.GBuffer->Emissive};
			for (uint32 Index = 0; Index < Resources.GBuffer.size(); ++Index)
				Complete = SetOptional(Resources.GBuffer[Index], Physical[Index]) && Complete;
		}
		if (Resources.GroundTruthAmbientOcclusion[0]
			&& IsRequested(*Resources.GroundTruthAmbientOcclusion[0]))
		{
			if (!Targets.GroundTruthAmbientOcclusion) return false;
			const std::array<FRHITexture*, 4> Physical{
				Targets.GroundTruthAmbientOcclusion->Raw,
				Targets.GroundTruthAmbientOcclusion->Scratch,
				Targets.GroundTruthAmbientOcclusion->Selector,
				Targets.GroundTruthAmbientOcclusion->Resolved};
			for (uint32 Index = 0;
				Index < Resources.GroundTruthAmbientOcclusion.size(); ++Index)
				Complete = SetOptional(Resources.GroundTruthAmbientOcclusion[Index],
					Physical[Index]) && Complete;
		}
		Complete = SetOptional(Resources.ContactShadowVisibilityFragment,
			Targets.ContactShadowVisibilityFragment ? Targets.ContactShadowVisibilityFragment->Visibility : nullptr)
			&& Complete;
		Complete = SetOptional(Resources.ContactShadowVisibilityCompute,
			Targets.ContactShadowVisibilityCompute ? Targets.ContactShadowVisibilityCompute->Visibility : nullptr)
			&& Complete;
		Complete = SetOptional(Resources.VolumetricCloudShadowFragment,
			Targets.VolumetricCloudShadowFragment
				? Targets.VolumetricCloudShadowFragment->Visibility : nullptr) && Complete;
		Complete = SetOptional(Resources.VolumetricCloudShadowCompute,
			Targets.VolumetricCloudShadowCompute
				? Targets.VolumetricCloudShadowCompute->Visibility : nullptr) && Complete;
		Complete = SetOptional(Resources.VolumetricCloudFragment,
			Targets.VolumetricCloudFragment
				? Targets.VolumetricCloudFragment->Cloud : nullptr) && Complete;
		Complete = SetOptional(Resources.VolumetricCloudCompute,
			Targets.VolumetricCloudCompute
				? Targets.VolumetricCloudCompute->Cloud : nullptr) && Complete;
		Complete = SetOptional(Resources.VolumetricCloudComposite,
			Targets.VolumetricCloudComposite
				? Targets.VolumetricCloudComposite->Cloud : nullptr) && Complete;
		Complete = SetOptional(Resources.IsolatedDeferred,
			Targets.IsolatedDeferred ? Targets.IsolatedDeferred->Color : nullptr)
			&& Complete;
		Complete = SetOptional(Resources.GBufferDebug,
			Targets.GBufferDebug ? Targets.GBufferDebug->Color : nullptr) && Complete;
		if (!Complete) Error = "renderer graph backing publication was incomplete";
		return Complete;
	}
} // namespace Durin
