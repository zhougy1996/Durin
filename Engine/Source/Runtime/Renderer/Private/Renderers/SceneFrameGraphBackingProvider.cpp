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
		const FResolvedSceneFrameTargets& Targets,
		std::string& Error) -> bool
	{
		bool Complete = true;
		for (const FRenderGraphPreparationRequest& Request : Requests)
		{
			FRHITexture* Physical = nullptr;
			if (Request.Name == "Scene.Color")
				Physical = Targets.Scene ? Targets.Scene->Color : nullptr;
			else if (Request.Name == "Scene.Depth")
				Physical = Targets.Scene ? Targets.Scene->Depth : nullptr;
			else if (Request.Name == "Scene.GBuffer.Debug")
				Physical = Targets.GBufferDebug ? Targets.GBufferDebug->Color : nullptr;
			else if (Request.Name.starts_with("Scene.GBuffer."))
			{
				if (Targets.GBuffer)
				{
					if (Request.Name.ends_with("Material")) Physical = Targets.GBuffer->Material;
					else if (Request.Name.ends_with("Normals")) Physical = Targets.GBuffer->Normals;
					else if (Request.Name.ends_with("Surface")) Physical = Targets.GBuffer->Surface;
					else if (Request.Name.ends_with("Emissive")) Physical = Targets.GBuffer->Emissive;
				}
			}
			else if (Request.Name.starts_with("Scene.AmbientOcclusion."))
			{
				if (Targets.GroundTruthAmbientOcclusion)
				{
					if (Request.Name.ends_with("Raw")) Physical = Targets.GroundTruthAmbientOcclusion->Raw;
					else if (Request.Name.ends_with("Scratch")) Physical = Targets.GroundTruthAmbientOcclusion->Scratch;
					else if (Request.Name.ends_with("Selector")) Physical = Targets.GroundTruthAmbientOcclusion->Selector;
					else if (Request.Name.ends_with("Resolved")) Physical = Targets.GroundTruthAmbientOcclusion->Resolved;
				}
			}
			else if (Request.Name == "Scene.ContactShadowVisibility.Fragment")
				Physical = Targets.ContactShadowVisibilityFragment
					? Targets.ContactShadowVisibilityFragment->Visibility : nullptr;
			else if (Request.Name == "Scene.ContactShadowVisibility.Compute")
				Physical = Targets.ContactShadowVisibilityCompute
					? Targets.ContactShadowVisibilityCompute->Visibility : nullptr;
			else if (Request.Name == "Scene.VolumetricCloudShadow.Fragment")
				Physical = Targets.VolumetricCloudShadowFragment
					? Targets.VolumetricCloudShadowFragment->Visibility : nullptr;
			else if (Request.Name == "Scene.VolumetricCloudShadow.Compute")
				Physical = Targets.VolumetricCloudShadowCompute
					? Targets.VolumetricCloudShadowCompute->Visibility : nullptr;
			else if (Request.Name == "Scene.VolumetricCloud.Fragment")
				Physical = Targets.VolumetricCloudFragment
					? Targets.VolumetricCloudFragment->Cloud : nullptr;
			else if (Request.Name == "Scene.VolumetricCloud.Compute")
				Physical = Targets.VolumetricCloudCompute
					? Targets.VolumetricCloudCompute->Cloud : nullptr;
			else if (Request.Name == "Scene.VolumetricCloud.Composite")
				Physical = Targets.VolumetricCloudComposite
					? Targets.VolumetricCloudComposite->Cloud : nullptr;
			else if (Request.Name == "Scene.DeferredDirectionalLighting.Isolated")
				Physical = Targets.IsolatedDeferred
					? Targets.IsolatedDeferred->Color : nullptr;
			Complete = Physical != nullptr
				&& Backings.SetTexture(Request.Texture, Physical) && Complete;
		}
		if (!Complete) Error = "renderer graph backing publication was incomplete";
		return Complete;
	}
} // namespace Durin
