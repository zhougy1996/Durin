#include "Renderers/SceneFrameGraphContributors.h"

namespace Durin
{
#define DURIN_RESOURCE_MEMBER(Field, Wrapper, Kind, ResourceKind, RangeKind, Use, Access, ...) \
	MakeRenderGraphResourceParameterMemberMetadata<FParameters, \
		decltype(FParameters::Field), Wrapper>(#Field, offsetof(FParameters, Field), \
			Kind, ResourceKind, RangeKind, Use, Access __VA_OPT__(,) __VA_ARGS__)


#define DURIN_TEXTURE(Field, Use, Access, ...) \
	DURIN_RESOURCE_MEMBER(Field, FRenderGraphTextureParameter, \
		ERenderGraphParameterMemberKind::Texture, ERenderGraphResourceKind::Texture, \
		ERenderGraphParameterRangeKind::TextureSubresource, Use, Access __VA_OPT__(,) __VA_ARGS__)
#define DURIN_MANAGED_TEXTURE(Field, EntryAccess, Discard, ResultAccess) \
	DURIN_RESOURCE_MEMBER(Field, FRenderGraphManagedTextureParameter, \
		ERenderGraphParameterMemberKind::ManagedTexture, ERenderGraphResourceKind::Texture, \
		ERenderGraphParameterRangeKind::TextureSubresource, ERenderGraphUse::ReadWrite, \
		EntryAccess, Discard, ERHIRenderTargetLoadAction::Load, \
		ERHIRenderTargetStoreAction::Store, true, ResultAccess)
#define DURIN_MANAGED_COLOR(Field, Discard, Load, ResultAccess) \
	DURIN_RESOURCE_MEMBER(Field, FRenderGraphColorAttachmentParameter, \
		ERenderGraphParameterMemberKind::ManagedColorAttachment, \
		ERenderGraphResourceKind::Texture, \
		ERenderGraphParameterRangeKind::TextureSubresource, ERenderGraphUse::ReadWrite, \
		ERHIAccess::ColorAttachmentReadWrite, Discard, Load, \
		ERHIRenderTargetStoreAction::Store, true, ResultAccess)
#define DURIN_MANAGED_DEPTH(Field, Discard, Load, ResultAccess) \
	DURIN_RESOURCE_MEMBER(Field, FRenderGraphDepthStencilAttachmentParameter, \
		ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment, \
		ERenderGraphResourceKind::Texture, \
		ERenderGraphParameterRangeKind::TextureSubresource, ERenderGraphUse::ReadWrite, \
		ERHIAccess::DepthStencilReadWrite, Discard, Load, \
		ERHIRenderTargetStoreAction::Store, true, ResultAccess)
#define DURIN_DEFINE_RESOURCE_METADATA(TypeName, ...) \
	auto TypeName::GetRenderGraphParametersMetadata() \
		-> const FRenderGraphParametersMetadata* \
	{ \
		using FParameters = TypeName; \
		static const std::array Members = {__VA_ARGS__}; \
		static const auto Metadata = MakeInlineRenderGraphParametersMetadata< \
			FParameters>(#TypeName, Members); \
		return &Metadata; \
	}

	DURIN_DEFINE_RESOURCE_METADATA(FDirectionalShadowPassResources,
		DURIN_MANAGED_DEPTH(DirectionalShadowOutput, true,
			ERHIRenderTargetLoadAction::Clear, ERHIAccess::GraphicsShaderRead));
	DURIN_DEFINE_RESOURCE_METADATA(FAmbientOcclusionPassResources,
		DURIN_TEXTURE(GBuffer, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(SceneDepth, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(AmbientOcclusionManaged,
			ERHIAccess::GraphicsShaderRead, true, ERHIAccess::GraphicsShaderRead));
	DURIN_DEFINE_RESOURCE_METADATA(FVolumetricCloudShadowPassResources,
		DURIN_TEXTURE(SceneDepth, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(SceneDepthCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_TEXTURE(CloudBaseDensity, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudDetailDensity, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudWeather, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudBaseDensityCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_TEXTURE(CloudDetailDensityCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_TEXTURE(CloudWeatherCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_MANAGED_TEXTURE(CloudShadowFragmentOutput,
			ERHIAccess::GraphicsShaderRead, true, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudShadowComputeOutput, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true));
	DURIN_DEFINE_RESOURCE_METADATA(FDeferredDirectionalLightingPassResources,
		DURIN_TEXTURE(DirectionalShadow, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(GBuffer, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(SceneDepth, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(AmbientOcclusion, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(ContactShadowFragment, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(ContactShadowCompute, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudShadowFragment, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudShadowCompute, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(DefaultWhite, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(DefaultShadowArray, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(EnvironmentIrradiance, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(EnvironmentPrefiltered, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(EnvironmentBrdfLut, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_COLOR(IsolatedDeferredOutput, true,
			ERHIRenderTargetLoadAction::Clear, ERHIAccess::GraphicsShaderRead));
	DURIN_DEFINE_RESOURCE_METADATA(FBaseScenePassResources,
		DURIN_TEXTURE(DirectionalShadow, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(DefaultWhite, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(DefaultShadowArray, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(EnvironmentIrradiance, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(EnvironmentPrefiltered, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(EnvironmentBrdfLut, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_COLOR(SceneColorOutput, true,
			ERHIRenderTargetLoadAction::Clear, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(SceneDepthGraphicsToGraphics,
			ERHIAccess::GraphicsShaderRead, false, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(SceneDepthGraphicsToDepth,
			ERHIAccess::GraphicsShaderRead, false, ERHIAccess::DepthStencilReadWrite),
		DURIN_MANAGED_TEXTURE(SceneDepthDepthToGraphics,
			ERHIAccess::DepthStencilReadWrite, true, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(SceneDepthDepthToDepth,
			ERHIAccess::DepthStencilReadWrite, true, ERHIAccess::DepthStencilReadWrite));
	DURIN_DEFINE_RESOURCE_METADATA(FVolumetricCloudSpatialPassResources,
		DURIN_TEXTURE(SceneDepth, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(SceneDepthCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_TEXTURE(CloudBaseDensity, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudDetailDensity, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudWeather, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudBaseDensityCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_TEXTURE(CloudDetailDensityCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_TEXTURE(CloudWeatherCompute, ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead),
		DURIN_MANAGED_TEXTURE(CloudFragmentOutput,
			ERHIAccess::GraphicsShaderRead, true, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudComputeOutput, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true));
	DURIN_DEFINE_RESOURCE_METADATA(FVolumetricCloudCompositePassResources,
		DURIN_TEXTURE(SceneColor, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(SceneDepth, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudBaseDensity, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudDetailDensity, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudWeather, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudShadowFragment, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudShadowCompute, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudFragment, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudCompute, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(CloudCompositeOutput,
			ERHIAccess::GraphicsShaderRead, true, ERHIAccess::GraphicsShaderRead));
	DURIN_DEFINE_RESOURCE_METADATA(FSceneColorPassResources,
		DURIN_MANAGED_TEXTURE(SceneColorManaged,
			ERHIAccess::ColorAttachmentReadWrite, false, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(SceneDepthManaged,
			ERHIAccess::GraphicsShaderRead, false, ERHIAccess::DepthStencilReadWrite));
	DURIN_DEFINE_RESOURCE_METADATA(FPostProcessPassResources,
		DURIN_MANAGED_COLOR(OutputPresent, true,
			ERHIRenderTargetLoadAction::Clear, ERHIAccess::Present),
		DURIN_MANAGED_COLOR(OutputOffscreen, true,
			ERHIRenderTargetLoadAction::Clear, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_COLOR(OutputForEditor, true,
			ERHIRenderTargetLoadAction::Clear, ERHIAccess::ColorAttachmentReadWrite),
		DURIN_TEXTURE(SceneColor, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(CloudComposite, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(SceneDepth, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_COLOR(GBufferDebugOutput, true,
			ERHIRenderTargetLoadAction::Clear, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(GBuffer, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead),
		DURIN_TEXTURE(IsolatedDeferred, ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead));
	DURIN_DEFINE_RESOURCE_METADATA(FEditorAssistancePassResources,
		DURIN_MANAGED_COLOR(EditorOutputPresent, false,
			ERHIRenderTargetLoadAction::Load, ERHIAccess::Present),
		DURIN_MANAGED_COLOR(EditorOutputOffscreen, false,
			ERHIRenderTargetLoadAction::Load, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_DEPTH(EditorDepth, false,
			ERHIRenderTargetLoadAction::Load, ERHIAccess::DepthStencilReadWrite));

#undef DURIN_DEFINE_RESOURCE_METADATA
#undef DURIN_MANAGED_DEPTH
#undef DURIN_MANAGED_COLOR
#undef DURIN_MANAGED_TEXTURE
#undef DURIN_TEXTURE

#undef DURIN_RESOURCE_MEMBER

#define DURIN_VALUE_MEMBER(Field, ValueType) \
	MakeRenderGraphValueParameterMemberMetadata<FParameters, \
		decltype(FParameters::Field), ValueType>(#Field, offsetof(FParameters, Field))
#define DURIN_NESTED_RESOURCES \
	MakeRenderGraphNestedParameterMemberMetadata<FParameters, \
		decltype(FParameters::Resources)>("Resources", offsetof(FParameters, Resources), \
			decltype(FParameters::Resources)::GetRenderGraphParametersMetadata())
#define DURIN_DEFINE_PASS_METADATA(TypeName, ...) \
	auto TypeName::GetRenderGraphParametersMetadata() \
		-> const FRenderGraphParametersMetadata* \
	{ \
		using FParameters = TypeName; \
		static const std::array Members = {__VA_ARGS__, DURIN_NESTED_RESOURCES}; \
		static const auto Metadata = MakeInlineRenderGraphParametersMetadata< \
			FParameters>(#TypeName, Members); \
		return &Metadata; \
	}

	DURIN_DEFINE_PASS_METADATA(FDirectionalShadowPassParameters,
		DURIN_VALUE_MEMBER(Completion, FDirectionalShadowPassResult));
	DURIN_DEFINE_PASS_METADATA(FAmbientOcclusionPassParameters,
		DURIN_VALUE_MEMBER(GBufferCompletion, FGBufferPassResult),
		DURIN_VALUE_MEMBER(Completion, FGroundTruthAmbientOcclusionPassResult));
	DURIN_DEFINE_PASS_METADATA(FVolumetricCloudShadowPassParameters,
		DURIN_VALUE_MEMBER(GBufferCompletion, FGBufferPassResult),
		DURIN_VALUE_MEMBER(Completion, FVolumetricCloudShadowPassResult));
	DURIN_DEFINE_PASS_METADATA(FDeferredDirectionalLightingPassParameters,
		DURIN_VALUE_MEMBER(DirectionalShadow, FDirectionalShadowPassResult),
		DURIN_VALUE_MEMBER(GBufferCompletion, FGBufferPassResult),
		DURIN_VALUE_MEMBER(AmbientOcclusion,
			FGroundTruthAmbientOcclusionPassResult),
		DURIN_VALUE_MEMBER(ContactShadow, FContactShadowVisibilityPassResult),
		DURIN_VALUE_MEMBER(CloudShadow, FVolumetricCloudShadowPassResult),
		DURIN_VALUE_MEMBER(Completion, FIsolatedDeferredPassResult));
	DURIN_DEFINE_PASS_METADATA(FBaseScenePassParameters,
		DURIN_VALUE_MEMBER(DeferredLighting, FIsolatedDeferredPassResult),
		DURIN_VALUE_MEMBER(Completion, FSceneColorPassResult));
	DURIN_DEFINE_PASS_METADATA(FVolumetricCloudSpatialPassParameters,
		DURIN_VALUE_MEMBER(BaseScene, FSceneColorPassResult),
		DURIN_VALUE_MEMBER(Completion, FVolumetricCloudSpatialPassResult));
	DURIN_DEFINE_PASS_METADATA(FVolumetricCloudCompositePassParameters,
		DURIN_VALUE_MEMBER(BaseScene, FSceneColorPassResult),
		DURIN_VALUE_MEMBER(Spatial, FVolumetricCloudSpatialPassResult),
		DURIN_VALUE_MEMBER(CloudShadow, FVolumetricCloudShadowPassResult),
		DURIN_VALUE_MEMBER(Completion, FVolumetricCloudPassResult));
	DURIN_DEFINE_PASS_METADATA(FSceneColorPassParameters,
		DURIN_VALUE_MEMBER(BaseScene, FSceneColorPassResult),
		DURIN_VALUE_MEMBER(VolumetricCloud, FVolumetricCloudPassResult),
		DURIN_VALUE_MEMBER(Completion, FSceneColorPassResult));

	auto FPostProcessPassParameters::GetRenderGraphParametersMetadata()
		-> const FRenderGraphParametersMetadata*
	{
		using FParameters = FPostProcessPassParameters;
		static const std::array Members = {
			DURIN_VALUE_MEMBER(SceneColor, FSceneColorPassResult),
			DURIN_VALUE_MEMBER(GBufferCompletion, FGBufferPassResult),
			DURIN_VALUE_MEMBER(DeferredLighting, FIsolatedDeferredPassResult),
			DURIN_VALUE_MEMBER(Completion, FPostProcessPassResult),
			MakeRenderGraphResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::OutputCompletion), FRenderGraphTokenParameter>(
					"OutputCompletion", offsetof(FParameters, OutputCompletion),
					ERenderGraphParameterMemberKind::Token,
					ERenderGraphResourceKind::Token,
					ERenderGraphParameterRangeKind::None,
					ERenderGraphUse::Write, ERHIAccess::None, true),
			DURIN_NESTED_RESOURCES};
		static const auto Metadata = MakeInlineRenderGraphParametersMetadata<
			FParameters>("FPostProcessPassParameters", Members);
		return &Metadata;
	}

	auto FEditorAssistancePassParameters::GetRenderGraphParametersMetadata()
		-> const FRenderGraphParametersMetadata*
	{
		using FParameters = FEditorAssistancePassParameters;
		static const std::array Members = {
			DURIN_VALUE_MEMBER(PostProcess, FPostProcessPassResult),
			MakeRenderGraphResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::OutputCompletion), FRenderGraphTokenParameter>(
					"OutputCompletion", offsetof(FParameters, OutputCompletion),
					ERenderGraphParameterMemberKind::Token,
					ERenderGraphResourceKind::Token,
					ERenderGraphParameterRangeKind::None,
					ERenderGraphUse::Write, ERHIAccess::None, true),
			DURIN_NESTED_RESOURCES};
		static const auto Metadata = MakeInlineRenderGraphParametersMetadata<
			FParameters>("FEditorAssistancePassParameters", Members);
		return &Metadata;
	}

#undef DURIN_DEFINE_PASS_METADATA
#undef DURIN_NESTED_RESOURCES
#undef DURIN_VALUE_MEMBER
} // namespace Durin
