#include "Resources/RenderTargetLayouts.h"
#include "RDG/RDGParameters.h"

namespace Durin::RenderTargetLayouts
{
	namespace
	{
		auto MakeColorAttachment(EPixelFormat Format, ERHIRenderTargetLoadAction LoadAction, ERHITextureLayout InitialLayout, ERHIAccess InitialAccess, ERHITextureLayout FinalLayout, ERHIAccess FinalAccess) -> FRHIAttachmentLayout
		{
			FRHIAttachmentLayout Layout;
			Layout.Format = Format;
			Layout.LoadAction = LoadAction;
			Layout.InitialLayout = InitialLayout;
			Layout.InitialAccess = InitialAccess;
			Layout.FinalLayout = FinalLayout;
			Layout.FinalAccess = FinalAccess;
			return Layout;
		}

		auto MakePreservedDepthAttachment(ERHIRenderTargetLoadAction LoadAction) -> FRHIAttachmentLayout
		{
			FRHIAttachmentLayout Layout;
			Layout.Format = EPixelFormat::D32;
			Layout.LoadAction = LoadAction;
			Layout.StoreAction = LoadAction == ERHIRenderTargetLoadAction::Load ? ERHIRenderTargetStoreAction::DontCare : ERHIRenderTargetStoreAction::Store;
			Layout.InitialLayout = LoadAction == ERHIRenderTargetLoadAction::Load ? ERHITextureLayout::DepthStencilAttachment : ERHITextureLayout::Undefined;
			Layout.InitialAccess = LoadAction == ERHIRenderTargetLoadAction::Load ? ERHIAccess::DepthStencilReadWrite : ERHIAccess::None;
			Layout.FinalLayout = ERHITextureLayout::DepthStencilAttachment;
			Layout.FinalAccess = ERHIAccess::DepthStencilReadWrite;
			return Layout;
		}
	} // namespace

	auto MakeSceneTargets() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::RGBA16_FLOAT,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		Layout.bHasDepthStencil = true;
		Layout.DepthStencilAttachment = MakePreservedDepthAttachment(ERHIRenderTargetLoadAction::Clear);
		return Layout;
	}

	auto MakeGBufferTargets() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 4;
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			Layout.ColorAttachments[Index].RenderTarget = MakeColorAttachment(
				EPixelFormat::RGBA8_UNORM,
				ERHIRenderTargetLoadAction::Clear,
				ERHITextureLayout::Undefined,
				ERHIAccess::None,
				ERHITextureLayout::ShaderReadOnly,
				ERHIAccess::GraphicsShaderRead
			);
		}
		Layout.ColorAttachments[3].RenderTarget = MakeColorAttachment(
			EPixelFormat::R11G11B10_FLOAT,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		Layout.bHasDepthStencil = true;
		Layout.DepthStencilAttachment = MakeDirectionalShadowDepth()
											.DepthStencilAttachment;
		Layout.DepthStencilAttachment.FinalLayout =
			ERHITextureLayout::ShaderReadOnly;
		Layout.DepthStencilAttachment.FinalAccess =
			ERHIAccess::GraphicsShaderRead;
		return Layout;
	}

	auto MakeDirectionalShadowDepth() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.bHasDepthStencil = true;
		Layout.DepthStencilAttachment.Format = EPixelFormat::D32;
		Layout.DepthStencilAttachment.LoadAction =
			ERHIRenderTargetLoadAction::Clear;
		Layout.DepthStencilAttachment.StoreAction =
			ERHIRenderTargetStoreAction::Store;
		Layout.DepthStencilAttachment.InitialLayout =
			ERHITextureLayout::Undefined;
		Layout.DepthStencilAttachment.InitialAccess = ERHIAccess::None;
		Layout.DepthStencilAttachment.FinalLayout =
			ERHITextureLayout::ShaderReadOnly;
		Layout.DepthStencilAttachment.FinalAccess =
			ERHIAccess::GraphicsShaderRead;
		return Layout;
	}

	auto MakeContactVisibilityOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::R8_UNORM,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::ColorAttachment,
			ERHIAccess::ColorAttachmentReadWrite,
			ERHITextureLayout::ColorAttachment,
			ERHIAccess::ColorAttachmentReadWrite
		);
		return Layout;
	}

	auto MakeVolumetricCloudShadowOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::R8_UNORM,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		return Layout;
	}

	auto MakeVolumetricCloudOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::RGBA16_FLOAT,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		return Layout;
	}

	auto MakeVolumetricCloudComposite() -> FRHIRenderTargetLayout
	{
		return MakeVolumetricCloudOutput();
	}

	auto MakeGBufferDebugOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::RGBA16_FLOAT,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		return Layout;
	}

	auto MakeDeferredDirectionalOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::RGBA16_FLOAT,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		return Layout;
	}

	auto MakeGroundTruthAmbientOcclusionOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::R8_UNORM,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		return Layout;
	}

	auto MakeGraphHybridDeferredOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(
			EPixelFormat::RGBA16_FLOAT, ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::Load);
		return Layout;
	}

	auto MakeGraphHybridRetainedForward() -> FRHIRenderTargetLayout
	{
		auto Layout = MakeGraphHybridDeferredOutput();
		Layout.bHasDepthStencil = true;
		Layout.DepthStencilAttachment = MakeRDGAttachmentLayout(
			EPixelFormat::D32, ERHIAccess::DepthStencilReadWrite, ERHIRenderTargetLoadAction::Load);
		return Layout;
	}

	auto MakeGraphHybridSceneBootstrap() -> FRHIRenderTargetLayout
	{
		auto Layout = MakeGraphHybridRetainedForward();
		Layout.ColorAttachments[0].RenderTarget = MakeRDGAttachmentLayout(
			EPixelFormat::RGBA16_FLOAT, ERHIAccess::ColorAttachmentReadWrite, ERHIRenderTargetLoadAction::Clear);
		return Layout;
	}

	auto MakeGraphSortedTranslucency() -> FRHIRenderTargetLayout
	{
		return MakeGraphHybridRetainedForward();
	}

	namespace
	{
		auto MakeGraphBoundary(FRHIRenderTargetLayout Layout) -> FRHIRenderTargetLayout
		{
			for (uint32 Index = 0; Index < Layout.NumColorRenderTargets; ++Index)
			{
				auto& Color = Layout.ColorAttachments[Index].RenderTarget;
				Color = MakeRDGAttachmentLayout(Color.Format, ERHIAccess::ColorAttachmentReadWrite,
					Color.LoadAction, Color.StoreAction, Color.NumSamples);
			}
			if (Layout.bHasDepthStencil)
			{
				auto& Depth = Layout.DepthStencilAttachment;
				Depth = MakeRDGAttachmentLayout(Depth.Format, ERHIAccess::DepthStencilReadWrite,
					Depth.LoadAction, Depth.StoreAction, Depth.NumSamples);
			}
			return Layout;
		}
	}

	auto MakeGraphSceneTargets() -> FRHIRenderTargetLayout { return MakeGraphBoundary(MakeSceneTargets()); }
	auto MakeGraphGBufferTargets() -> FRHIRenderTargetLayout { return MakeGraphBoundary(MakeGBufferTargets()); }

	auto MakeScenePostProcessOutput(EPixelFormat OutputFormat)
		-> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			OutputFormat,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ColorAttachment,
			ERHIAccess::ColorAttachmentReadWrite
		);
		return Layout;
	}

	auto MakeFinalScenePostProcessOutput(EViewportOutput Output,
		EPixelFormat OutputFormat)
		-> FRHIRenderTargetLayout
	{
		const bool bPresent = Output == EViewportOutput::Present;
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			OutputFormat,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			bPresent ? ERHITextureLayout::Present : ERHITextureLayout::ShaderReadOnly,
			bPresent ? ERHIAccess::Present : ERHIAccess::GraphicsShaderRead
		);
		return Layout;
	}

	auto MakeEditorAssistanceOutput(EViewportOutput Output,
		EPixelFormat OutputFormat) -> FRHIRenderTargetLayout
	{
		const bool bPresent = Output == EViewportOutput::Present;
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			OutputFormat,
			ERHIRenderTargetLoadAction::Load,
			ERHITextureLayout::ColorAttachment,
			ERHIAccess::ColorAttachmentReadWrite,
			bPresent ? ERHITextureLayout::Present : ERHITextureLayout::ShaderReadOnly,
			bPresent ? ERHIAccess::Present : ERHIAccess::GraphicsShaderRead
		);
		Layout.bHasDepthStencil = true;
		Layout.DepthStencilAttachment = MakePreservedDepthAttachment(ERHIRenderTargetLoadAction::Load);
		return Layout;
	}
} // namespace Durin::RenderTargetLayouts
