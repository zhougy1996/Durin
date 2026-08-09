#include "Resources/RenderTargetLayouts.h"

namespace Durin::RenderTargetLayouts
{
	namespace
	{
		auto MakeColorAttachment(ERHIRenderTargetLoadAction LoadAction, ERHITextureLayout InitialLayout, ERHIAccess InitialAccess,
			ERHITextureLayout FinalLayout, ERHIAccess FinalAccess) -> FRHIAttachmentLayout
		{
			FRHIAttachmentLayout Layout;
			Layout.Format = EPixelFormat::SRGBA8_UNORM;
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
			Layout.StoreAction = LoadAction == ERHIRenderTargetLoadAction::Load
				? ERHIRenderTargetStoreAction::DontCare
				: ERHIRenderTargetStoreAction::Store;
			Layout.InitialLayout = LoadAction == ERHIRenderTargetLoadAction::Load
				? ERHITextureLayout::DepthStencilAttachment
				: ERHITextureLayout::Undefined;
			Layout.InitialAccess = LoadAction == ERHIRenderTargetLoadAction::Load
				? ERHIAccess::DepthStencilReadWrite
				: ERHIAccess::None;
			Layout.FinalLayout = ERHITextureLayout::DepthStencilAttachment;
			Layout.FinalAccess = ERHIAccess::DepthStencilReadWrite;
			return Layout;
		}
	}

	auto MakeSceneTargets() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
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

	auto MakeScenePostProcessOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ColorAttachment,
			ERHIAccess::ColorAttachmentReadWrite
		);
		return Layout;
	}

	auto MakeFinalScenePostProcessOutput(EViewportOutput Output)
		-> FRHIRenderTargetLayout
	{
		const bool bPresent = Output == EViewportOutput::Present;
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			bPresent ? ERHITextureLayout::Present : ERHITextureLayout::ShaderReadOnly,
			bPresent ? ERHIAccess::Present : ERHIAccess::GraphicsShaderRead
		);
		return Layout;
	}

	auto MakeEditorAssistanceOutput(EViewportOutput Output) -> FRHIRenderTargetLayout
	{
		const bool bPresent = Output == EViewportOutput::Present;
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
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
