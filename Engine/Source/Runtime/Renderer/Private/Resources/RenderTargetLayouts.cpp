#include "Resources/RenderTargetLayouts.h"

namespace Durin::RenderTargetLayouts
{
	namespace
	{
		auto MakeColorAttachment(EPixelFormat Format, ERHIRenderTargetLoadAction LoadAction, ERHITextureLayout InitialLayout, ERHIAccess InitialAccess,
			ERHITextureLayout FinalLayout, ERHIAccess FinalAccess) -> FRHIAttachmentLayout
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

		auto MakeDirectionalDirectAttachment() -> FRHIAttachmentLayout
		{
			FRHIAttachmentLayout Layout;
			Layout.Format = EPixelFormat::R11G11B10_FLOAT;
			Layout.LoadAction = ERHIRenderTargetLoadAction::Clear;
			Layout.InitialLayout = ERHITextureLayout::Undefined;
			Layout.InitialAccess = ERHIAccess::None;
			Layout.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			Layout.FinalAccess = ERHIAccess::GraphicsShaderRead;
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
		Layout.NumColorRenderTargets = 2;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::RGBA16_FLOAT,
			ERHIRenderTargetLoadAction::Clear,
			ERHITextureLayout::Undefined,
			ERHIAccess::None,
			ERHITextureLayout::ShaderReadOnly,
			ERHIAccess::GraphicsShaderRead
		);
		Layout.ColorAttachments[1].RenderTarget =
			MakeDirectionalDirectAttachment();
		Layout.bHasDepthStencil = true;
		Layout.DepthStencilAttachment = MakePreservedDepthAttachment(ERHIRenderTargetLoadAction::Clear);
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

	auto MakeContactShadowOutput() -> FRHIRenderTargetLayout
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

	auto MakeScenePostProcessOutput() -> FRHIRenderTargetLayout
	{
		FRHIRenderTargetLayout Layout;
		Layout.NumColorRenderTargets = 1;
		Layout.ColorAttachments[0].RenderTarget = MakeColorAttachment(
			EPixelFormat::SRGBA8_UNORM,
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
			EPixelFormat::SRGBA8_UNORM,
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
			EPixelFormat::SRGBA8_UNORM,
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
